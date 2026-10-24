#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gateway_protocol.h"

static bool parse(const char *json, gateway_command_t *out)
{
    return gateway_parse_v1(json, (int)strlen(json), out);
}

int main(void)
{
    gateway_command_t cmd = {0};
    assert(parse("{\"v\":1,\"command_id\":\"knob_01\",\"desired\":{\"power\":true,\"brightness_percent\":1,\"color_temperature_kelvin\":6500}}", &cmd));
    assert(cmd.v1 && cmd.set_power && cmd.power && cmd.set_brightness &&
           cmd.brightness == 1 && cmd.set_temperature && cmd.temperature == 6500);
    assert(!parse("{\"v\":2,\"command_id\":\"x\",\"desired\":{\"power\":true}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"bad/id\",\"desired\":{\"power\":true}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{\"brightness_percent\":0}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{\"brightness_percent\":100.5}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{\"color_temperature_kelvin\":6501}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{\"power\":1}}", &cmd));
    assert(!parse("{\"v\":1,\"command_id\":\"x\",\"desired\":{\"power\":true}}garbage", &cmd));
    char long_id[65];
    memset(long_id, 'a', 64);
    long_id[64] = 0;
    char payload[160];
    snprintf(payload, sizeof(payload), "{\"v\":1,\"command_id\":\"%s\",\"desired\":{\"power\":true}}", long_id);
    assert(!parse(payload, &cmd));
    long_id[63] = 0;
    snprintf(payload, sizeof(payload), "{\"v\":1,\"command_id\":\"%s\",\"desired\":{\"power\":true}}", long_id);
    assert(parse(payload, &cmd));
    gateway_dedup_t dedup = {0};
    assert(!gateway_seen(&dedup, "id-0"));
    gateway_remember(&dedup, "id-0");
    assert(gateway_seen(&dedup, "id-0"));
    for (unsigned i = 1; i <= GATEWAY_RECENT_COMMANDS; ++i) {
        char id[16];
        snprintf(id, sizeof(id), "id-%u", i);
        gateway_remember(&dedup, id);
    }
    assert(!gateway_seen(&dedup, "id-0"));
    assert(gateway_seen(&dedup, "id-16"));
    puts("gateway protocol host tests passed");
}
