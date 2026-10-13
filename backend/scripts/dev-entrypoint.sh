#!/usr/bin/env bash
# Development entrypoint: configure, build and run the hub from the
# bind-mounted sources. The build directory lives in a named volume so the
# CMake cache and the fetched dependencies survive container restarts.
set -euo pipefail

BUILD_DIR="${HOMESTR_BUILD_DIR:-/app/build}"
BUILD_TYPE="${HOMESTR_BUILD_TYPE:-Debug}"

echo "[entrypoint] configuring ${BUILD_TYPE} build in ${BUILD_DIR}"
cmake -S /app -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DHOMESTR_BUILD_TESTS=ON

echo "[entrypoint] building"
cmake --build "${BUILD_DIR}"

if [[ "${HOMESTR_RUN_TESTS:-0}" == "1" ]]; then
  ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

echo "[entrypoint] starting homestr_server"
exec "${BUILD_DIR}/homestr_server" "$@"
