#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GATEWAY_COMMAND_ID_SIZE 64
#define GATEWAY_RECENT_COMMANDS 16

typedef struct {
    enum { CMD_STATE, CMD_POWER, CMD_BRIGHTNESS, CMD_TEMPERATURE } kind;
    uint16_t value;
    bool v1;
    bool set_power, power, set_brightness, set_temperature;
    uint16_t brightness, temperature;
    char command_id[GATEWAY_COMMAND_ID_SIZE];
} gateway_command_t;

typedef struct {
    char ids[GATEWAY_RECENT_COMMANDS][GATEWAY_COMMAND_ID_SIZE];
    unsigned next;
} gateway_dedup_t;

bool gateway_parse_v1(const char *data, int len, gateway_command_t *out);
bool gateway_seen(const gateway_dedup_t *dedup, const char *id);
void gateway_remember(gateway_dedup_t *dedup, const char *id);
