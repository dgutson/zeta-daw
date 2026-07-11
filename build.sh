#!/bin/bash
set -euo pipefail

cmake -S . -B build -DBUILD_TESTING=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
