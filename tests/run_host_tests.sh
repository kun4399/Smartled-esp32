#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=$(mktemp "${TMPDIR:-/tmp}/smartled-protocol.XXXXXX")
trap 'rm -f "$binary"' EXIT
cc -std=c11 -Wall -Wextra -Werror \
  -I"$root/main" -I"$root/managed_components/espressif__cjson/cJSON" \
  "$root/tests/test_gateway_protocol.c" \
  "$root/main/gateway_protocol.c" \
  "$root/managed_components/espressif__cjson/cJSON/cJSON.c" \
  -lm -o "$binary"
"$binary"
