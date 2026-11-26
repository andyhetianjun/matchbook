#!/bin/sh
# Builds the command line tool and runs it on the example orders.
# Needs CMake 3.21+ and a C++20 compiler. Set CMAKE=... to use another cmake (e.g. CMAKE="uvx cmake").
set -eu
cd "$(dirname "$0")/.."
CMAKE=${CMAKE:-cmake}
$CMAKE --preset release >/dev/null
$CMAKE --build --preset release --target matchbook -j >/dev/null
./build/release/matchbook --book 3 data/example.csv
