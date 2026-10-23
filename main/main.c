#include <stdbool.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nimble/hci_common.h"
#include "host/util/util.h"
#include "miot_link.h"
#include "gateway.h"

static const char *TAG = "smartled_scan";
#define LEMESH_WY0D02_PID 19733
#define SEEN_CAPACITY 64
#define REPEAT_LOG_US (10LL * 1000 * 1000)
#define TARGET_CONNECT_COOLDOWN_US (60LL * 1000 * 1000)

struct seen_device {
    ble_addr_t addr;
    uint8_t event_type;
    uint8_t mesh_types;
    uint16_t pid;
    bool has_pid;
    int64_t last_log_us;
};

static struct seen_device seen[SEEN_CAPACITY];

static uint16_t fe95_start_handle;
static uint16_t fe95_end_handle;
static bool target_connecting;
static int64_t last_target_connect_us;

#if CONFIG_SMARTLED_SCAN_ALL
#define SMARTLED_SCAN_ALL 1
#else
#define SMARTLED_SCAN_ALL 0
#endif

#if CONFIG_SMARTLED_LOG_RAW_ADV
#define SMARTLED_LOG_RAW_ADV 1
#else
#define SMARTLED_LOG_RAW_ADV 0
#endif

static bool has_fe95(const struct ble_hs_adv_fields *fields)
{
    for (int i = 0; i < fields->num_uuids16; ++i) {
        if (fields->uuids16[i].value == 0xfe95) {
            return true;
        }
    }
    return fields->svc_data_uuid16_len >= 2 &&
           fields->svc_data_uuid16[0] == 0x95 &&
           fields->svc_data_uuid16[1] == 0xfe;
}

/* MiBeacon service data: FE95 UUID (2), frame control (2), PID (2), counter (1).
 * A matching PID identifies a product type, not the owner's individual unit. */
static bool mibeacon_pid(const struct ble_hs_adv_fields *fields, uint16_t *pid)
{
    const uint8_t *data = fields->svc_data_uuid16;
    if (data == NULL || fields->svc_data_uuid16_len < 7 ||
        data[0] != 0x95 || data[1] != 0xfe) {
        return false;
    }
    *pid = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    return true;
}

static uint8_t mesh_ad_types(const struct ble_gap_disc_desc *disc)
{
    uint8_t result = 0;
    for (size_t pos = 0; pos < disc->length_data;) {
        uint8_t length = disc->data[pos];
        if (length == 0 || pos + 1 + length > disc->length_data) {
            break;
        }
        uint8_t type = disc->data[pos + 1];
        if (type == BLE_HS_ADV_TYPE_MESH_PROV) {
            result |= 1;
        } else if (type == BLE_HS_ADV_TYPE_MESH_MESSAGE) {
            result |= 2;
        } else if (type == BLE_HS_ADV_TYPE_MESH_BEACON) {
            result |= 4;
        }
        pos += 1 + length;
    }
    return result;
}

static bool should_log(const struct ble_gap_disc_desc *disc, uint8_t mesh_types,
                       bool has_pid, uint16_t pid)
{
    int64_t now = esp_timer_get_time();
    int replace = 0;
    for (int i = 0; i < SEEN_CAPACITY; ++i) {
        if (seen[i].last_log_us == 0) {
            replace = i;
            break;
        }
        if (seen[i].addr.type == disc->addr.type &&
            memcmp(seen[i].addr.val, disc->addr.val, sizeof(disc->addr.val)) == 0 &&
            seen[i].event_type == disc->event_type) {
            if (seen[i].mesh_types == mesh_types && seen[i].has_pid == has_pid &&
                (!has_pid || seen[i].pid == pid) &&
                now - seen[i].last_log_us < REPEAT_LOG_US) {
                return false;
            }
            replace = i;
            break;
        }
        if (seen[i].last_log_us < seen[replace].last_log_us) {
            replace = i;
        }
    }
    seen[replace] = (struct seen_device){
        .addr = disc->addr,
        .event_type = disc->event_type,
        .mesh_types = mesh_types,
        .pid = pid,
        .has_pid = has_pid,
        .last_log_us = now,
    };
    return true;
}

static void scan_start(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static int service_discovered(uint16_t conn_handle, const struct ble_gatt_error *error,
                              const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0) {
        char uuid[BLE_UUID_STR_LEN];
        ESP_LOGI(TAG, "GATT service uuid=%s start=%u end=%u",
                 ble_uuid_to_str(&service->uuid.u, uuid),
                 service->start_handle, service->end_handle);
        if (service->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&service->uuid.u) == 0xfe95) {
            fe95_start_handle = service->start_handle;
            fe95_end_handle = service->end_handle;
        } else {
            /* Non-FE95 services are logged for diagnostics but not traversed. */
        }
    } else if (error->status == BLE_HS_EDONE) {
        if (fe95_start_handle == 0 || fe95_end_handle == 0) {
            ESP_LOGE(TAG, "candidate has no FE95 MIoT service");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        } else {
            ESP_LOGI(TAG, "FE95 service found; discovering control characteristics");
            miot_link_begin(conn_handle, fe95_start_handle, fe95_end_handle);
        }
    } else {
        ESP_LOGW(TAG, "GATT service discovery stopped: status=%d", error->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int mtu_exchanged(uint16_t conn_handle, const struct ble_gatt_error *error,
                         uint16_t mtu, void *arg)
{
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "ATT MTU exchange failed: status=%u; continuing with MTU %u",
                 error->status, mtu);
    } else {
        ESP_LOGI(TAG, "ATT MTU negotiated: %u", mtu);
    }
    int rc = ble_gattc_disc_all_svcs(conn_handle, service_discovered, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "cannot start GATT service discovery: rc=%d", rc);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
        miot_link_on_gap_event(event);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISC) {
        struct ble_hs_adv_fields fields;
        const struct ble_gap_disc_desc *disc = &event->disc;
        if (ble_hs_adv_parse_fields(&fields, disc->data, disc->length_data) != 0) {
            return 0;
        }
        bool fe95 = has_fe95(&fields);
        if (!fe95 && !SMARTLED_SCAN_ALL) {
            return 0;
        }
        char addr[18];
        snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 disc->addr.val[5], disc->addr.val[4], disc->addr.val[3],
                 disc->addr.val[2], disc->addr.val[1], disc->addr.val[0]);
        char name[32] = {0};
        uint16_t pid = 0;
        bool has_pid = mibeacon_pid(&fields, &pid);
        bool has_model_name = fields.name != NULL &&
                              fields.name_len == sizeof("lemesh.light.wy0d02") - 1 &&
                              memcmp(fields.name, "lemesh.light.wy0d02",
                                     sizeof("lemesh.light.wy0d02") - 1) == 0;
        bool target_model = (has_pid && pid == LEMESH_WY0D02_PID) || has_model_name;
        uint8_t mesh_types = mesh_ad_types(disc);
        bool log_report = should_log(disc, mesh_types, has_pid, pid);
        if (target_model) {
            if (log_report && SMARTLED_LOG_RAW_ADV) {
                ESP_LOG_BUFFER_HEX_LEVEL(TAG, disc->data, disc->length_data, ESP_LOG_INFO);
            }
            /* The product name is commonly in SCAN_RSP, so accept it there too.
             * The scan response follows the connectable ADV_IND from the same peer. */
            int64_t now = esp_timer_get_time();
            bool cooldown_elapsed = last_target_connect_us == 0 ||
                                    now - last_target_connect_us >= TARGET_CONNECT_COOLDOWN_US;
            if (!target_connecting && cooldown_elapsed &&
                (disc->event_type == BLE_HCI_ADV_TYPE_ADV_IND ||
                 (has_model_name && disc->event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP))) {
                uint8_t own_addr_type;
                int rc = ble_gap_disc_cancel();
                if (rc == 0 && ble_hs_id_infer_auto(0, &own_addr_type) == 0) {
                    target_connecting = true;
                    last_target_connect_us = now;
                    ESP_LOGI(TAG, "target PID/name advertisement found; starting read-only GATT connection");
                    rc = ble_gap_connect(own_addr_type, &disc->addr, 15000, NULL,
                                         gap_event, NULL);
                    if (rc != 0) {
                        target_connecting = false;
                        ESP_LOGW(TAG, "GATT connection request rejected: rc=%d", rc);
                        scan_start();
                    }
                } else {
                    ESP_LOGW(TAG, "cannot begin target GATT connection: rc=%d", rc);
                    scan_start();
                }
            }
        }
        if (!log_report) {
            return 0;
        }
        char pid_text[16];
        if (has_pid) {
            snprintf(pid_text, sizeof(pid_text), "%u", pid);
        } else {
            snprintf(pid_text, sizeof(pid_text), "unknown");
        }
        if (fields.name != NULL && fields.name_len > 0) {
            size_t length = fields.name_len < sizeof(name) - 1 ?
                            fields.name_len : sizeof(name) - 1;
            memcpy(name, fields.name, length);
            for (size_t i = 0; i < length; ++i) {
                if ((unsigned char)name[i] < 0x20 || (unsigned char)name[i] > 0x7e) {
                    name[i] = '?';
                }
            }
        }
        ESP_LOGI(TAG, "BLE addr=%s addr_type=%u adv_type=%u rssi=%d FE95=%s pid=%s model=%s mesh_ad=0x%x name=%s len=%u",
                 addr, disc->addr.type, disc->event_type, disc->rssi, fe95 ? "yes" : "no",
                 pid_text, has_pid && pid == LEMESH_WY0D02_PID ?
                 "lemesh.light.wy0d02" : "unknown", mesh_types,
                 name[0] ? name : "(none)", disc->length_data);
    } else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        if (target_connecting) {
            ESP_LOGI(TAG, "scan stopped for target connection: reason=%d",
                     event->disc_complete.reason);
        } else {
            ESP_LOGW(TAG, "scan ended: reason=%d; restarting", event->disc_complete.reason);
            scan_start();
        }
    } else if (event->type == BLE_GAP_EVENT_CONNECT) {
        if (event->connect.status == 0) {
            fe95_start_handle = fe95_end_handle = 0;
            ESP_LOGI(TAG, "target GATT connection established; negotiating ATT MTU");
            int rc = ble_gattc_exchange_mtu(event->connect.conn_handle,
                                            mtu_exchanged, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "cannot start ATT MTU exchange: rc=%d", rc);
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
        } else {
            ESP_LOGW(TAG, "read-only GATT connection failed: status=%d", event->connect.status);
            target_connecting = false;
            scan_start();
        }
    } else if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        miot_link_disconnected();
        fe95_start_handle = fe95_end_handle = 0;
        ESP_LOGI(TAG, "GATT connection closed; restarting model scan");
        target_connecting = false;
        scan_start();
    }
    return 0;
}

static void scan_start(void)
{
    uint8_t own_addr_type;
    int rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot select BLE address: rc=%d", rc);
        return;
    }
    struct ble_gap_disc_params params = {0};
    params.passive = 0;
    params.filter_duplicates = 0;
    rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot start BLE scan: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "active survey started; FE95 filter=%s, raw=%s, PID target=%d",
                 SMARTLED_SCAN_ALL ? "off" : "on",
                 SMARTLED_LOG_RAW_ADV ? "on" : "off", LEMESH_WY0D02_PID);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "cannot initialize BLE identity: rc=%d", rc);
        return;
    }
    scan_start();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: reason=%d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int command_state(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (miot_link_command_state() != 0) {
        printf("State read failed. Wait for MIoT BLE login or reconnect.\n");
        return 1;
    }
    return 0;
}

static int parse_command_value(int argc, char **argv, uint32_t *value)
{
    if (argc != 2) return -1;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || parsed > UINT32_MAX) return -1;
    *value = (uint32_t)parsed;
    return 0;
}

static int command_brightness(int argc, char **argv)
{
    uint32_t value;
    if (parse_command_value(argc, argv, &value) != 0 || value < 1 || value > 100) {
        printf("Usage: brightness <1..100>\n");
        return 1;
    }
    if (miot_link_command_brightness(value) != 0) {
        printf("Brightness change failed or was not confirmed.\n");
        return 1;
    }
    return 0;
}

static int command_temperature(int argc, char **argv)
{
    uint32_t value;
    if (parse_command_value(argc, argv, &value) != 0 || value < 2700 || value > 6500) {
        printf("Usage: temperature <2700..6500 kelvin>\n");
        return 1;
    }
    if (miot_link_command_temperature(value) != 0) {
        printf("Color temperature change failed or was not confirmed.\n");
        return 1;
    }
    return 0;
}

static int command_power(int argc, char **argv)
{
    uint32_t value;
    if (parse_command_value(argc, argv, &value) != 0 || value > 1) {
        printf("Usage: power <0|1>\n");
        return 1;
    }
    if (miot_link_command_power(value != 0) != 0) {
        printf("Power change failed or was not confirmed.\n");
        return 1;
    }
    return 0;
}

static void start_serial_console(void)
{
    const esp_console_cmd_t commands[] = {
        {.command = "state", .help = "read power, brightness, and color temperature", .func = command_state},
        {.command = "brightness", .help = "set brightness from 1 to 100", .hint = "<1..100>", .func = command_brightness},
        {.command = "temperature", .help = "set color temperature in kelvin", .hint = "<2700..6500>", .func = command_temperature},
        {.command = "power", .help = "set light power, with read-back", .hint = "<0|1>", .func = command_power},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    }
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    repl_config.prompt = "smartled>";
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void)
{
    ESP_LOGI(TAG, "reset reason enum=%d", (int)esp_reset_reason());
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s; existing NVS will not be erased", esp_err_to_name(err));
        return;
    }
    miot_link_init();
    miot_link_start_task();
    start_serial_console();
    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
    gateway_start();
}
