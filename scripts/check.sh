#!/usr/bin/env bash
# Full local check on Linux:
#   1. gcc, Release, -Werror: build and run every test
#   2. clang, Debug, AddressSanitizer + UndefinedBehaviorSanitizer: every test
#   3. clang, ThreadSanitizer: the threaded tests (test_concurrency shares one
#      mapping between a producer and a consumer thread, so TSan checks the
#      acquire/release pairing on head and tail)
# Usage: scripts/check.sh   (from the repository root)
set -euo pipefail
cd "$(dirname "$0")/.."
jobs=$(nproc)

echo "== gcc, Release, -Werror"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCORUNDUM_WERROR=ON >/dev/null
cmake --build build -j"$jobs"
ctest --test-dir build --output-on-failure

if command -v clang >/dev/null; then
  echo "== clang, Debug, AddressSanitizer + UndefinedBehaviorSanitizer"
  san="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
  CC=clang CXX=clang++ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DCORUNDUM_WERROR=ON \
    -DCMAKE_C_FLAGS="$san" -DCMAKE_CXX_FLAGS="$san" -DCORUNDUM_BUILD_BENCH=OFF >/dev/null
  cmake --build build-san -j"$jobs"
  ctest --test-dir build-san --output-on-failure

  echo "== clang, ThreadSanitizer"
  tsan="-fsanitize=thread -fno-omit-frame-pointer -O1 -g"
  CC=clang CXX=clang++ cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DCORUNDUM_WERROR=ON \
    -DCMAKE_C_FLAGS="$tsan" -DCMAKE_CXX_FLAGS="$tsan" -DCORUNDUM_BUILD_BENCH=OFF \
    -DCORUNDUM_BUILD_EXAMPLES=OFF >/dev/null
  cmake --build build-tsan -j"$jobs" --target test_concurrency test_cpp
  # TSan needs a memory layout that newer kernels' ASLR can break; run without ASLR.
  norand() { if setarch "$(uname -m)" -R true 2>/dev/null; then setarch "$(uname -m)" -R "$@"; else "$@"; fi; }
  export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"
  norand ./build-tsan/test_concurrency 1
  norand ./build-tsan/test_cpp
fi

leftover=$(ls /dev/shm | grep -c '^crd-' || true)
echo "== leftover segments in /dev/shm: $leftover"
[ "$leftover" = 0 ]
echo "== all checks passed"
