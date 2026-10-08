#!/bin/bash
# Used only by realm-lab CI/CD in its dedicated build volume.
set -euo pipefail
install_prefix="${1:-/build}"
cmake -S /workspace -B /build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$install_prefix" \
  -DTOOLS=1 -DBUILD_TESTING=ON \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build /build
ctest --test-dir /build --output-on-failure --no-tests=error
cmake --install /build
"$install_prefix/bin/worldserver" --version
# Extractors are required for authoring the new map, not just the servers.
for tool in mapextractor vmap4extractor vmap4assembler mmaps_generator; do
  test -x "$install_prefix/bin/$tool"
done
