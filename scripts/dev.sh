#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$0")/.."
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
exec ./build/evida-server --port "${EVIDA_PORT:-18142}" --db "${EVIDA_DB_PATH:-var/evida.sqlite}"
