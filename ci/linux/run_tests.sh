#!/usr/bin/env bash

SCRIPT=$(readlink -f "$0")
HERE=$(dirname "$SCRIPT")

export LD_LIBRARY_PATH=$(pwd)/bin/lib:$LD_LIBRARY_PATH

# if on mac and building for architecture that doesn't match host
# then unittests will fail so we just need to skip it (for now)
if [ "$RUNNER_OS" = "macOS" ] && [ "$ARCH" != "$(uname -m)" ]; then
    echo "Skipping tests due to architecture mismatch!"
    exit 0
fi

if [ "$CONFIGURATION" = "Debug" ] && [[ "$RUNNER_OS" != "macOS" ]] ; then
    valgrind --leak-check=full --error-exitcode=1 --gen-suppressions=all \
        --suppressions="$HERE/valgrind.supp" ./bin/unittests --gtest_shuffle
else
    ./bin/unittests --gtest_shuffle
fi

if [ "$CONFIGURATION" = "Release" ]; then
    started=$SECONDS
    ctest -C Release -L telemetry-short --output-on-failure --timeout 60
    elapsed=$((SECONDS - started))
    if [ "$elapsed" -ge 300 ]; then
        echo "telemetry-short exceeded five minutes: ${elapsed}s" >&2
        exit 1
    fi
fi
