#!/usr/bin/env bash
# Full local check: gcc release build, clang ASan+UBSan debug build, all tests.
# Usage: scripts/check.sh   (from the repository root, on Linux)
set -euo pipefail
cd "$(dirname "$0")/.."
jobs=$(nproc)

echo "== gcc, Release, -Werror"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCORUNDUM_WERROR=ON >/dev/null
cmake --build build -j"$jobs"
for t in test_ring test_ipc test_cpp; do ./build/$t; done

if command -v clang >/dev/null; then
  echo "== clang, Debug, AddressSanitizer + UndefinedBehaviorSanitizer"
  san="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
  CC=clang CXX=clang++ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCORUNDUM_WERROR=ON \
    -DCMAKE_C_FLAGS="$san" -DCMAKE_CXX_FLAGS="$san" -DCORUNDUM_BUILD_BENCH=OFF >/dev/null
  cmake --build build-san -j"$jobs"
  for t in test_ring test_ipc test_cpp; do ./build-san/$t; done
fi

leftover=$(ls /dev/shm | grep -c '^crd-' || true)
echo "== leftover segments in /dev/shm: $leftover"
[ "$leftover" = 0 ]
echo "== all checks passed"
