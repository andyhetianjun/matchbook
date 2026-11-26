#!/bin/sh
# Compiles the exchange to WebAssembly and assembles the static demo in site/.
# Needs em++ (brew install emscripten, or the emsdk). Usage: scripts/build_wasm.sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p site
em++ -O2 -std=c++20 -Iinclude -Wall -Wextra -Werror \
  wasm/api.cpp src/order_book.cpp src/feed.cpp src/ledger.cpp src/exchange.cpp \
  src/traffic.cpp src/wire.cpp \
  -o site/matchbook.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createMatchbook -sENVIRONMENT=web \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=33554432 \
  -sEXPORTED_FUNCTIONS=_mb_init,_mb_command,_mb_traffic,_mb_state \
  -sEXPORTED_RUNTIME_METHODS=cwrap
cp web/index.html site/index.html
ls -l site
