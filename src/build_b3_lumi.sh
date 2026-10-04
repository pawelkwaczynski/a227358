#!/bin/bash -l
# build_b3_lumi.sh: clean build of b3core on LUMI with a manifest tying every result to its binary.
#   cd /project/project_465003389/green27/b3 && bash build_b3_lumi.sh
set -euo pipefail
cd "$(dirname "$0")"
module load gcc-native/14.2 >/dev/null 2>&1
FLAGS="-std=c++17 -O3 -fno-exceptions -pthread"      # same as build.sh portable: no host-specific flags
rm -f b3core-portable
g++ $FLAGS -o b3core-portable b3core.cpp
{
    echo "built $(date -u +%FT%TZ) on $(hostname)"
    echo "compiler: $(g++ --version | head -1)"
    echo "flags: $FLAGS"
    sha256sum b3core.cpp merge_b3_shards.py b3core-portable
} > MANIFEST.txt
cat MANIFEST.txt
