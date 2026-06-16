#!/usr/bin/env bash
# Build and run the host-side Kerfur nearby peer-state test.
# Requires a host C compiler (gcc/clang). No Zephyr toolchain needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-gcc}"
OUT="$HERE/nearby_test"

"$CC" -std=c11 -O1 -g \
	-Wall -Wextra -Wno-missing-field-initializers \
	-I"$ROOT/include" \
	-I"$HERE/shim" \
	-include "$HERE/nearby_test_config.h" \
	"$HERE/test_nearby.c" \
	"$ROOT/src/nearby/kerfur_nearby.c" \
	"$ROOT/src/nearby/encounter_log.c" \
	-o "$OUT"

"$OUT"
