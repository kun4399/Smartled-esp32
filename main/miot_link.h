#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "host/ble_gap.h"

void miot_link_init(void);
void miot_link_start_task(void);
void miot_link_begin(uint16_t conn_handle, uint16_t service_start, uint16_t service_end);
void miot_link_on_gap_event(struct ble_gap_event *event);
void miot_link_disconnected(void);
int miot_link_command_state(void);
int miot_link_command_brightness(uint32_t value);
int miot_link_command_temperature(uint32_t value);
int miot_link_command_power(bool on);
typedef struct {
    bool power;
    uint8_t brightness;
    uint16_t temperature;
} miot_light_state_t;
int miot_link_read_state(miot_light_state_t *state);
