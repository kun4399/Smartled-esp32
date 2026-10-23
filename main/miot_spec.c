#include "miot_spec.h"

static void put_le16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static size_t begin_packet(uint8_t *out, size_t capacity, uint16_t tid,
                           uint8_t opcode, size_t body_len)
{
    const size_t total = 5 + body_len;
    if (out == NULL || total > capacity || total > 0x0fff) {
        return 0;
    }
    put_le16(out, (uint16_t)(0x2000 | total));
    put_le16(out + 2, tid);
    out[4] = opcode;
    return total;
}

size_t miot_build_get_property(uint8_t *out, size_t capacity, uint16_t tid,
                               uint8_t siid, uint16_t piid)
{
    const size_t total = begin_packet(out, capacity, tid, 0x02, 4);
    if (total == 0) {
        return 0;
    }
    out[5] = 1;
    out[6] = siid;
    put_le16(out + 7, piid);
    return total;
}

size_t miot_build_set_bool(uint8_t *out, size_t capacity, uint16_t tid,
                           uint8_t siid, uint16_t piid, bool value)
{
    const size_t total = begin_packet(out, capacity, tid, 0x00, 7);
    if (total == 0) {
        return 0;
    }
    out[5] = 1;
    out[6] = siid;
    put_le16(out + 7, piid);
    put_le16(out + 9, 0x0001); /* BOOL type 0, one-byte length. */
    out[11] = value ? 1 : 0;
    return total;
}

size_t miot_build_set_uint(uint8_t *out, size_t capacity, uint16_t tid,
                           uint8_t siid, uint16_t piid, uint32_t value,
                           uint8_t width)
{
    uint8_t value_type;
    if (width == 1) {
        value_type = 1; /* UINT8 */
        if (value > UINT8_MAX) return 0;
    } else if (width == 2) {
        value_type = 3; /* UINT16 */
        if (value > UINT16_MAX) return 0;
    } else if (width == 4) {
        value_type = 5; /* UINT32 */
    } else {
        return 0;
    }
    const size_t total = begin_packet(out, capacity, tid, 0x00, 6 + width);
    if (total == 0) {
        return 0;
    }
    out[5] = 1;
    out[6] = siid;
    put_le16(out + 7, piid);
    put_le16(out + 9, (uint16_t)(((uint16_t)value_type << 12) | width));
    for (uint8_t i = 0; i < width; ++i) {
        out[11 + i] = (uint8_t)(value >> (8 * i));
    }
    return total;
}
