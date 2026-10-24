#include "gateway.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

#include "esp_event.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "miot_link.h"
#include "gateway_protocol.h"

static const char *TAG = "smartled_gateway";
static esp_mqtt_client_handle_t mqtt;
static QueueHandle_t commands;
static volatile bool mqtt_connected;
static char command_topic[96];
static char state_topic[96];
static const char *v1_set = "xknob/v1/light/main/set";
static const char *v1_ack = "xknob/v1/light/main/ack";
static const char *v1_state = "xknob/v1/light/main/state";
static const char *v1_availability = "xknob/v1/light/main/availability";
static gateway_dedup_t dedup;

static bool matches(const char *topic, int len, const char *expected)
{
    return len == (int)strlen(expected) && memcmp(topic, expected, len) == 0;
}

static void publish_ack(const char *id, bool success, const char *stage)
{
    if (!mqtt_connected || !id || !id[0]) return;
    cJSON *doc = cJSON_CreateObject();
    if (!doc) return;
    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddStringToObject(doc, "command_id", id);
    cJSON_AddBoolToObject(doc, "success", success);
    cJSON_AddStringToObject(doc, "ack_stage", stage);
    char *body = cJSON_PrintUnformatted(doc);
    if (body) { esp_mqtt_client_publish(mqtt, v1_ack, body, 0, 1, 0); free(body); }
    cJSON_Delete(doc);
}

static bool publish_state(bool ok, const char *command_id,
                          const gateway_command_t *desired, miot_light_state_t *reported)
{
    if (!mqtt_connected) return false;
    miot_light_state_t state;
    char body[160];
    bool read_ok = miot_link_read_state(&state) == 0;
    if (read_ok && reported) *reported = state;
    if (read_ok && desired) {
        if (desired->v1) {
            ok = ok && (!desired->set_power || state.power == desired->power) &&
                (!desired->set_brightness || state.brightness == desired->brightness) &&
                (!desired->set_temperature || state.temperature == desired->temperature);
        } else if (desired->kind == CMD_POWER) ok = ok && state.power == (desired->value != 0);
        else if (desired->kind == CMD_BRIGHTNESS) ok = ok && state.brightness == desired->value;
        else if (desired->kind == CMD_TEMPERATURE) ok = ok && state.temperature == desired->value;
    }
    if (read_ok) {
        snprintf(body, sizeof(body),
                 "{\"online\":true,\"ok\":%s,\"power\":%s,\"brightness\":%u,\"temperature\":%u}",
                 ok ? "true" : "false", state.power ? "true" : "false",
                 state.brightness, state.temperature);
    } else {
        snprintf(body, sizeof(body), "{\"online\":false,\"ok\":false}");
    }
    esp_mqtt_client_publish(mqtt, state_topic, body, 0, 1, 1);
    cJSON *doc = cJSON_CreateObject();
    if (!doc) return read_ok;
    cJSON_AddNumberToObject(doc, "v", 1);
    cJSON_AddStringToObject(doc, "device_id", "main");
    cJSON_AddBoolToObject(doc, "online", read_ok);
    cJSON_AddStringToObject(doc, "verification", read_ok ? "device_readback" : "unavailable");
    cJSON_AddBoolToObject(doc, "ok", ok && read_ok);
    if (command_id && command_id[0]) cJSON_AddStringToObject(doc, "last_command_id", command_id);
    if (read_ok) {
        cJSON_AddBoolToObject(doc, "power", state.power);
        cJSON_AddNumberToObject(doc, "brightness_percent", state.brightness);
        cJSON_AddNumberToObject(doc, "color_temperature_kelvin", state.temperature);
    }
    char *json = cJSON_PrintUnformatted(doc);
    if (json) { esp_mqtt_client_publish(mqtt, v1_state, json, 0, 1, 1); free(json); }
    cJSON_Delete(doc);
    return read_ok && ok;
}

static bool parse_command(const char *data, int len, gateway_command_t *out)
{
    memset(out, 0, sizeof(*out));
    if (len == 5 && memcmp(data, "state", 5) == 0) {
        *out = (gateway_command_t){.kind = CMD_STATE};
        return true;
    }
    char buffer[32];
    if (len <= 0 || len >= (int)sizeof(buffer)) return false;
    memcpy(buffer, data, len);
    buffer[len] = 0;
    char *space = strchr(buffer, ' ');
    if (!space || space[1] == 0) return false;
    *space++ = 0;
    for (char *p = space; *p; ++p) if (*p < '0' || *p > '9') return false;
    unsigned long value = strtoul(space, NULL, 10);
    if (strcmp(buffer, "power") == 0 && value <= 1) out->kind = CMD_POWER;
    else if (strcmp(buffer, "brightness") == 0 && value >= 1 && value <= 100) out->kind = CMD_BRIGHTNESS;
    else if (strcmp(buffer, "temperature") == 0 && value >= 2700 && value <= 6500) out->kind = CMD_TEMPERATURE;
    else return false;
    out->value = (uint16_t)value;
    return true;
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        mqtt_connected = true;
        esp_mqtt_client_subscribe(mqtt, command_topic, 1);
        esp_mqtt_client_subscribe(mqtt, v1_set, 1);
        esp_mqtt_client_publish(mqtt, v1_availability, "online", 0, 1, 1);
        gateway_command_t cmd = {.kind = CMD_STATE};
        xQueueSend(commands, &cmd, 0);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        mqtt_connected = false;
    } else if (id == MQTT_EVENT_DATA && !event->retain &&
               (matches(event->topic, event->topic_len, command_topic) ||
                matches(event->topic, event->topic_len, v1_set)) &&
               event->current_data_offset == 0 && event->data_len == event->total_data_len) {
        gateway_command_t cmd;
        bool modern = matches(event->topic, event->topic_len, v1_set);
        if (modern ? gateway_parse_v1(event->data, event->data_len, &cmd) :
                     parse_command(event->data, event->data_len, &cmd)) {
            if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
                ESP_LOGW(TAG, "command queue full");
                if (cmd.v1) publish_ack(cmd.command_id, false, "queue_full");
            }
        } else {
            ESP_LOGW(TAG, "invalid command rejected");
        }
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_connected = false;
        esp_wifi_connect();
    }
}

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        gateway_command_t cmd;
        // Always Ready: lower idle traffic; each command still verifies immediately.
        if (xQueueReceive(commands, &cmd, pdMS_TO_TICKS(30000)) != pdTRUE) {
            publish_state(true, NULL, NULL, NULL);
            continue;
        }
        if (cmd.v1 && gateway_seen(&dedup, cmd.command_id)) {
            publish_ack(cmd.command_id, true, "duplicate");
            continue;
        }
        int rc = 0;
        if (cmd.v1) {
            if (cmd.set_brightness && miot_link_command_brightness(cmd.brightness) != 0) rc = -1;
            if (cmd.set_temperature && miot_link_command_temperature(cmd.temperature) != 0) rc = -1;
            if (cmd.set_power && miot_link_command_power(cmd.power) != 0) rc = -1;
        } else if (cmd.kind == CMD_POWER) rc = miot_link_command_power(cmd.value != 0);
        else if (cmd.kind == CMD_BRIGHTNESS) rc = miot_link_command_brightness(cmd.value);
        else if (cmd.kind == CMD_TEMPERATURE) rc = miot_link_command_temperature(cmd.value);
        bool confirmed = publish_state(rc == 0, cmd.v1 ? cmd.command_id : NULL, &cmd, NULL);
        if (cmd.v1) {
            if (confirmed) gateway_remember(&dedup, cmd.command_id);
            publish_ack(cmd.command_id, confirmed, confirmed ? "device_readback" : "failed");
        }
    }
}

void gateway_start(void)
{
    if (CONFIG_SMARTLED_WIFI_SSID[0] == 0 || CONFIG_SMARTLED_MQTT_URI[0] == 0) {
        ESP_LOGI(TAG, "local gateway disabled until Wi-Fi and MQTT are configured");
        return;
    }
    if (strlen(CONFIG_SMARTLED_WIFI_SSID) >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
        strlen(CONFIG_SMARTLED_WIFI_PASSWORD) >= sizeof(((wifi_config_t *)0)->sta.password)) {
        ESP_LOGE(TAG, "Wi-Fi SSID or password too long");
        return;
    }
    if (snprintf(command_topic, sizeof(command_topic), "%s/smartled/strip/set", CONFIG_SMARTLED_MQTT_TOPIC) >= sizeof(command_topic) ||
        snprintf(state_topic, sizeof(state_topic), "%s/smartled/strip/state", CONFIG_SMARTLED_MQTT_TOPIC) >= sizeof(state_topic)) {
        ESP_LOGE(TAG, "MQTT topic prefix too long");
        return;
    }
    commands = xQueueCreate(8, sizeof(gateway_command_t));
    if (!commands) return;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, CONFIG_SMARTLED_WIFI_SSID, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, CONFIG_SMARTLED_WIFI_PASSWORD, sizeof(wifi.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_SMARTLED_MQTT_URI,
        .credentials.username = CONFIG_SMARTLED_MQTT_USER,
        .credentials.authentication.password = CONFIG_SMARTLED_MQTT_PASSWORD,
        .session.last_will.topic = v1_availability,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
    };
    mqtt = esp_mqtt_client_init(&cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    xTaskCreate(worker, "gateway", 6144, NULL, 4, NULL);
}
