#!/bin/bash
# Build with retry: clang-scan-deps may segfault (known C++26 module issue).
# Clears module cache and retries up to 3 times.
# Usage: ./scripts/build-with-retry.sh <preset>

set -uo pipefail

PRESET="${1:?Usage: build-with-retry.sh <preset>}"

for attempt in 1 2 3; do
    echo "=== Build attempt $attempt ==="
    if cmake --build --preset "$PRESET"; then
        echo "Build succeeded on attempt $attempt"
        exit 0
    fi
    echo "Build failed on attempt $attempt, clearing caches..."
    # Clear .ddi scan-deps outputs so ninja re-scans, and the PCMs CMake built
    # next to them (build/*/CMakeFiles/**), so a stale BMI cannot survive a
    # failed attempt.
    find build/ -name "*.pcm" -delete 2>/dev/null || true
    find build/ -name "*.ddi" -delete 2>/dev/null || true
done
echo "Build failed after 3 attempts"
exit 1
