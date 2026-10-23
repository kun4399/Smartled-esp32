#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../main/miot_spec.h"

static void assert_packet(const uint8_t *actual, size_t actual_len,
                          const uint8_t *expected, size_t expected_len)
{
    assert(actual_len == expected_len);
    for (size_t i = 0; i < expected_len; ++i) {
        assert(actual[i] == expected[i]);
    }
}

int main(void)
{
    uint8_t packet[32];
    const uint8_t get_brightness[] = {0x09, 0x20, 0x01, 0x72, 0x02, 0x01, 0x02, 0x02, 0x00};
    size_t len = miot_build_get_property(packet, sizeof(packet), 0x7201, 2, 2);
    assert_packet(packet, len, get_brightness, sizeof(get_brightness));

    const uint8_t set_brightness[] = {0x0c, 0x20, 0x01, 0x73, 0x00, 0x01, 0x02,
                                      0x02, 0x00, 0x01, 0x10, 0x32};
    len = miot_build_set_uint(packet, sizeof(packet), 0x7301, 2, 2, 50, 1);
    assert_packet(packet, len, set_brightness, sizeof(set_brightness));

    const uint8_t set_color_temperature[] = {0x0d, 0x20, 0x02, 0x73, 0x00, 0x01, 0x02,
                                             0x03, 0x00, 0x02, 0x30, 0xa0, 0x0f};
    len = miot_build_set_uint(packet, sizeof(packet), 0x7302, 2, 3, 4000, 2);
    assert_packet(packet, len, set_color_temperature, sizeof(set_color_temperature));

    assert(miot_build_set_uint(packet, sizeof(packet), 1, 2, 2, 101, 1) != 0);
    assert(miot_build_set_uint(packet, sizeof(packet), 1, 2, 3, 6501, 2) != 0);
    assert(miot_build_set_uint(packet, sizeof(packet), 1, 2, 2, 256, 1) == 0);
    assert(miot_build_set_uint(packet, sizeof(packet), 1, 2, 3, 65536, 2) == 0);
    return 0;
}
