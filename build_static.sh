#!/bin/sh
#
# build_static.sh
#
# Builds a fully static, dependency-free rmq_bridge binary on Alpine Linux.
# Fetches cJSON and rabbitmq-c from source, compiles them as static libraries,
# then links everything into a single portable binary.
#
# Prerequisites (Alpine):
#   apk add build-base cmake linux-headers git
#
# Usage:
#   chmod +x build_static.sh
#   ./build_static.sh
#
# Output:
#   ./rmq_bridge  (static binary)
#

set -e

# ---- Configuration ------------------------------------------------

CJSON_VERSION="v1.7.19"
RABBITMQ_C_VERSION="v0.15.0"

BUILD_DIR="$(pwd)/_static_build"
PREFIX="${BUILD_DIR}/install"

NPROC=$(nproc 2>/dev/null || echo 4)

# ---- Prepare build directory --------------------------------------

echo "==> Preparing build directory: ${BUILD_DIR}"
mkdir -p "${BUILD_DIR}" "${PREFIX}"

# ---- Build cJSON (static) ----------------------------------------

echo "==> Fetching cJSON ${CJSON_VERSION}"
cd "${BUILD_DIR}"
if [ -d "cJSON" ]; then
    echo "    cJSON directory already exists, skipping clone"
else
    git clone --depth 1 --branch "${CJSON_VERSION}" https://github.com/DaveGamble/cJSON.git
fi

echo "==> Building cJSON (static)"
mkdir -p cJSON/build && cd cJSON/build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_C_FLAGS="-fPIC" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_SHARED_AND_STATIC_LIBS=OFF \
    -DENABLE_CJSON_TEST=OFF \
    -DENABLE_CJSON_UTILS=OFF
make -j"${NPROC}"
make install

# ---- Build rabbitmq-c (static) -----------------------------------

echo "==> Fetching rabbitmq-c ${RABBITMQ_C_VERSION}"
cd "${BUILD_DIR}"
if [ -d "rabbitmq-c" ]; then
    echo "    rabbitmq-c directory already exists, skipping clone"
else
    git clone --depth 1 --branch "${RABBITMQ_C_VERSION}" https://github.com/alanxz/rabbitmq-c.git
fi

echo "==> Building rabbitmq-c (static, no SSL)"
mkdir -p rabbitmq-c/build && cd rabbitmq-c/build
cmake .. \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_C_FLAGS="-fPIC" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_STATIC_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_TOOLS=OFF \
    -DENABLE_SSL_SUPPORT=OFF
make -j"${NPROC}"
make install

# ---- Build rmq_bridge (fully static) -----------------------------

echo "==> Building rmq_bridge (static)"
cd "${BUILD_DIR}/.."

gcc -Wall -Wextra -O2 -std=c11 -static -no-pie \
    -I"${PREFIX}/include" \
    -o rmq_bridge \
    rmq_bridge.c \
    -L"${PREFIX}/lib" -L"${PREFIX}/lib64" \
    -lrabbitmq -lcjson -lpthread

# ---- Verify -------------------------------------------------------

echo ""
echo "==> Build complete."
echo ""

file rmq_bridge
ldd rmq_bridge 2>&1 || true

SIZE=$(du -h rmq_bridge | cut -f1)
echo ""
echo "==> Static binary: ./rmq_bridge (${SIZE})"
echo "==> This binary has no shared library dependencies and is fully portable."

# ---- Optional: strip for smaller binary ---------------------------

echo "==> Stripping binary for size"
strip rmq_bridge

SIZE_STRIPPED=$(du -h rmq_bridge | cut -f1)
echo "==> Stripped binary: ./rmq_bridge (${SIZE_STRIPPED})"
