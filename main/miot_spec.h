#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* MIoT Spec-v2 property request builders. Returns encoded length, or 0. */
size_t miot_build_get_property(uint8_t *out, size_t capacity, uint16_t tid,
                               uint8_t siid, uint16_t piid);
size_t miot_build_set_bool(uint8_t *out, size_t capacity, uint16_t tid,
                           uint8_t siid, uint16_t piid, bool value);
size_t miot_build_set_uint(uint8_t *out, size_t capacity, uint16_t tid,
                           uint8_t siid, uint16_t piid, uint32_t value,
                           uint8_t width);
