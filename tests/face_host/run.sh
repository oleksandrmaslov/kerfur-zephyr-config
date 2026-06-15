#!/usr/bin/env bash
# Build and run the host-side face_runtime render-path test.
# Requires a host C compiler (gcc/clang). No Zephyr toolchain needed.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
CC="${CC:-gcc}"
OUT="$HERE/face_test"

"$CC" -std=c11 -O1 -g \
	-Wall -Wextra -Wno-missing-field-initializers \
	-I"$ROOT/include" \
	-I"$HERE/shim" \
	"$HERE/test_face_runtime.c" \
	"$ROOT/src/ui/face_runtime.c" \
	"$ROOT/src/ui/generated/kerfur_face_recipes.c" \
	"$ROOT/src/ui/generated/kerfur_face_assets.c" \
	-o "$OUT"

"$OUT"
