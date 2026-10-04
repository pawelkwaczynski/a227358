#!/bin/sh
# Build the ERD-241 C++ core.  Apple clang++ (no extra toolchain required).
#   sh computational/erd241/build.sh
set -e
cd "$(dirname "$0")"

CXX=${CXX:-clang++}
BASE="-std=c++17 -O3 -fno-exceptions -pthread"

if [ "${1:-}" = "portable" ]; then
    $CXX $BASE -o b3core-portable b3core.cpp
    echo "built b3core-portable with: $CXX $BASE"
    exit 0
fi

if [ -n "${1:-}" ]; then
    echo "usage: $0 [portable]" >&2
    exit 2
fi

# -march=native is not accepted by Apple clang on arm64; try the arch flags in
# order and keep the first one that compiles.
for ARCH in "-mcpu=native" "-mcpu=apple-m4" "-mcpu=apple-m1" "-march=native" ""; do
    if $CXX $BASE $ARCH -o b3core b3core.cpp 2>/dev/null; then
        echo "built b3core with: $CXX $BASE $ARCH"
        exit 0
    fi
done

# no arch flag worked silently -> rebuild loudly to show the real error
$CXX $BASE -o b3core b3core.cpp
echo "built b3core with: $CXX $BASE (no arch flag)"
