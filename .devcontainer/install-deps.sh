#!/usr/bin/env bash
# .devcontainer/install-deps.sh
#
# Installs all build-time and runtime dependencies for the CrossOS SDK
# inside the dev-container (Ubuntu 24.04).
#
# Targets:
#   • Linux/X11 native build
#   • Windows cross-compile via mingw-w64
#   • Android cross-compile via Android NDK r26+

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "=== CrossOS dev-container setup ==="

# ── System packages ───────────────────────────────────────────────────────
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    clang \
    clang-format \
    clang-tidy \
    lld \
    ninja-build \
    pkg-config \
    git \
    curl \
    unzip \
    wget \
    ca-certificates \
    \
    `# X11 / XInput2 development libraries` \
    libx11-dev \
    libxext-dev \
    libxi-dev \
    libxrandr-dev \
    libxinerama-dev \
    libxcursor-dev \
    \
    `# Windows cross-compile toolchain` \
    mingw-w64 \
    \
    `# Static analysis / memory checking` \
    valgrind \
    cppcheck \
    \
    `# Java (required by Android SDK tools)` \
    openjdk-17-jdk-headless

# ── Android NDK ───────────────────────────────────────────────────────────
ANDROID_NDK_VERSION="r26d"
ANDROID_NDK_DIR="/opt/android-ndk-${ANDROID_NDK_VERSION}"

if [ ! -d "${ANDROID_NDK_DIR}" ]; then
    echo "--- Installing Android NDK ${ANDROID_NDK_VERSION} ---"
    NDK_ZIP="android-ndk-${ANDROID_NDK_VERSION}-linux.zip"
    wget -q "https://dl.google.com/android/repository/${NDK_ZIP}" -O /tmp/${NDK_ZIP}
    unzip -q /tmp/${NDK_ZIP} -d /opt/
    rm /tmp/${NDK_ZIP}
    echo "Android NDK installed at ${ANDROID_NDK_DIR}"
else
    echo "Android NDK already present at ${ANDROID_NDK_DIR}"
fi

# Symlink so CMake toolchain can use a stable path
ln -sfn "${ANDROID_NDK_DIR}" /opt/android-ndk

# Add NDK to PATH for all users
echo 'export ANDROID_NDK_HOME=/opt/android-ndk' >> /etc/environment
echo 'export PATH=$PATH:/opt/android-ndk/toolchains/llvm/prebuilt/linux-x86_64/bin' >> /etc/environment

echo ""
echo "=== Build targets available ==="
echo "  Linux/X11 native  :  cmake -B build && cmake --build build"
echo "  Windows (cross)   :  cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw64.cmake && cmake --build build-win"
echo "  Android (arm64)   :  cmake -B build-android -DCMAKE_TOOLCHAIN_FILE=/opt/android-ndk/build/cmake/android.toolchain.cmake \\"
echo "                              -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 && cmake --build build-android"
echo ""
echo "=== Setup complete ==="
