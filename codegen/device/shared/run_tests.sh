#!/usr/bin/env bash
# Host tests for the frame parser. No board, no display, no radio.
#
# Sanitizers are on by default and are not decoration: the first run caught a
# use-after-free, and the buffer-overrun cases below are exactly what a truncated
# frame arriving over a radio would exercise on a device with no memory protection
# and nobody watching.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIXTURES="${1:-$HERE/../../tests/fixtures/frames}"
BIN="$(mktemp -d)/frame_test"

CXX="${CXX:-c++}"
SANITIZE="${SANITIZE:-address,undefined}"

"$CXX" -std=c++17 -Wall -Wextra -Werror \
  ${SANITIZE:+-fsanitize="$SANITIZE"} -g \
  "$HERE/frame.cpp" "$HERE/frame_test.cpp" -o "$BIN"

"$BIN" "$FIXTURES"
