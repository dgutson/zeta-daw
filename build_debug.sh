#!/bin/bash
set -euo pipefail

cmake -S . -B build-debug \
    -DBUILD_TESTING=OFF \
    -DCMAKE_BUILD_TYPE=Debug \
    -DZETA_MIDI_TRACE=ON
cmake --build build-debug --parallel
