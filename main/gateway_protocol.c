#include "gateway_protocol.h"

#include <string.h>
#include "cJSON.h"

static bool valid_command_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n >= GATEWAY_COMMAND_ID_SIZE) return false;
    for (size_t i = 0; i < n; ++i)
        if (!((id[i] >= 'a' && id[i] <= 'z') || (id[i] >= 'A' && id[i] <= 'Z') ||
              (id[i] >= '0' && id[i] <= '9') || id[i] == '-' || id[i] == '_')) return false;
    return true;
}

static bool bounded_int(const cJSON *item, int low, int high)
{
    return cJSON_IsNumber(item) && item->valuedouble >= low &&
           item->valuedouble <= high && item->valuedouble == item->valueint;
}

bool gateway_parse_v1(const char *data, int len, gateway_command_t *out)
{
    if (!data || !out || len <= 0 || len > 512) return false;
    const char *end = NULL;
    cJSON *doc = cJSON_ParseWithLengthOpts(data, len, &end, false);
    if (!doc) return false;
    while (end < data + len && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t')) ++end;
    if (end != data + len) { cJSON_Delete(doc); return false; }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(doc, "v");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(doc, "command_id");
    const cJSON *desired = cJSON_GetObjectItemCaseSensitive(doc, "desired");
    const cJSON *power = cJSON_GetObjectItemCaseSensitive(desired, "power");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(desired, "brightness_percent");
    const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(desired, "color_temperature_kelvin");
    bool valid = bounded_int(version, 1, 1) && cJSON_IsString(id) &&
        valid_command_id(id->valuestring) && cJSON_IsObject(desired) &&
        (power || brightness || temperature) &&
        (!power || cJSON_IsBool(power)) && (!brightness || bounded_int(brightness, 1, 100)) &&
        (!temperature || bounded_int(temperature, 2700, 6500));
    if (valid) {
        memset(out, 0, sizeof(*out));
        out->v1 = true;
        memcpy(out->command_id, id->valuestring, strlen(id->valuestring) + 1);
        out->set_power = power != NULL;
        out->power = cJSON_IsTrue(power);
        out->set_brightness = brightness != NULL;
        out->brightness = brightness ? brightness->valueint : 0;
        out->set_temperature = temperature != NULL;
        out->temperature = temperature ? temperature->valueint : 0;
    }
    cJSON_Delete(doc);
    return valid;
}

bool gateway_seen(const gateway_dedup_t *dedup, const char *id)
{
    for (unsigned i = 0; i < GATEWAY_RECENT_COMMANDS; ++i)
        if (strcmp(dedup->ids[i], id) == 0) return true;
    return false;
}

void gateway_remember(gateway_dedup_t *dedup, const char *id)
{
    size_t len = strlen(id);
    if (!valid_command_id(id)) return;
    memcpy(dedup->ids[dedup->next], id, len + 1);
    dedup->next = (dedup->next + 1) % GATEWAY_RECENT_COMMANDS;
}
