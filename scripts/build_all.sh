#!/usr/bin/env bash
set -u

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BUILD_TYPE="${BUILD_TYPE:-Release}"
ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
ANDROID_PLATFORM="${ANDROID_PLATFORM:-android-26}"
ANDROID_NDK="${ANDROID_NDK:-/opt/android-ndk}"
ARTIFACTS_DIR="$ROOT_DIR/artifacts"

RUN_LINUX=1
RUN_WINDOWS=1
RUN_ANDROID_NDK=1
RUN_ANDROID_APK=1
CLEAN=0
KEEP_GOING=1

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Builds CrossOS SDK targets/apps across all supported platforms in this repo.

Options:
  --clean                 Remove build folders before configuring
  --stop-on-error         Stop at first failed stage
  --linux-only            Run only Linux CMake build
  --windows-only          Run only Windows (mingw) CMake build
  --android-ndk-only      Run only Android NDK CMake build
  --android-apk-only      Run only Android Gradle APK build
  --no-linux              Skip Linux CMake build
  --no-windows            Skip Windows CMake build
  --no-android-ndk        Skip Android NDK CMake build
  --no-android-apk        Skip Android Gradle APK build
  -h, --help              Show this help

Environment overrides:
  BUILD_TYPE      (default: Release)
  ANDROID_ABI     (default: arm64-v8a)
  ANDROID_PLATFORM(default: android-26)
  ANDROID_NDK     (default: /opt/android-ndk)
EOF
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "[ERROR] Missing required command: $1"
        return 1
    fi
    return 0
}

log_stage() {
    printf "\n========== %s ==========%s" "$1" "\n"
}

run_cmd() {
    echo "+ $*"
    "$@"
}

mark_result() {
    local stage="$1"
    local code="$2"
    if [ "$code" -eq 0 ]; then
        PASSED_STAGES+=("$stage")
    else
        FAILED_STAGES+=("$stage")
        if [ "$KEEP_GOING" -eq 0 ]; then
            summarize_and_exit 1
        fi
    fi
}

run_linux() {
    local stage="linux"
    local build_dir="$ROOT_DIR/build"
    local artifacts_dir="$ARTIFACTS_DIR/linux"

    log_stage "Linux build"
    require_cmd cmake || return 1

    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$build_dir"
    fi

    run_cmd cmake -S "$ROOT_DIR" -B "$build_dir" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DCROSSOS_BUILD_TESTS=ON || return 1
    run_cmd cmake --build "$build_dir" || return 1
    run_cmd ctest --test-dir "$build_dir" --output-on-failure || return 1

    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$artifacts_dir"
    fi

    run_cmd mkdir -p "$artifacts_dir" || return 1

    local found=0
    local bin

    # Top-level executables (if any)
    for bin in "$build_dir"/*; do
        if [ -f "$bin" ] && [ -x "$bin" ]; then
            run_cmd cp "$bin" "$artifacts_dir/" || return 1
            found=1
        fi
    done

    # App executables are emitted under build/applications/<app>/<app>
    for bin in "$build_dir"/applications/*/*; do
        if [ -f "$bin" ] && [ -x "$bin" ]; then
            run_cmd cp "$bin" "$artifacts_dir/" || return 1
            found=1
        fi
    done

    if [ "$found" -eq 0 ]; then
        echo "[ERROR] No Linux executables found in $build_dir"
        return 1
    fi

    echo "Linux artifacts: $artifacts_dir"
    return 0
}

run_windows() {
    local stage="windows"
    local build_dir="$ROOT_DIR/build-win"
    local artifacts_dir="$ARTIFACTS_DIR/windows"

    log_stage "Windows (mingw) build"
    require_cmd cmake || return 1

    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$build_dir"
    fi

    run_cmd cmake -S "$ROOT_DIR" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/cmake/toolchain-mingw64.cmake" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCROSSOS_BUILD_TESTS=OFF || return 1

    local build_code=0
    run_cmd cmake --build "$build_dir" || build_code=$?

    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$artifacts_dir"
    fi

    run_cmd mkdir -p "$artifacts_dir" || return 1

    local found=0
    local bin

    # Top-level Windows executables (if any)
    for bin in "$build_dir"/*.exe; do
        if [ -f "$bin" ]; then
            run_cmd cp "$bin" "$artifacts_dir/" || return 1
            found=1
        fi
    done

    # App executables under build-win/applications/<app>/<app>.exe
    for bin in "$build_dir"/applications/*/*.exe; do
        if [ -f "$bin" ]; then
            run_cmd cp "$bin" "$artifacts_dir/" || return 1
            found=1
        fi
    done

    if [ "$found" -eq 0 ]; then
        echo "[ERROR] No Windows executables found in $build_dir"
        if [ "$build_code" -ne 0 ]; then
            return "$build_code"
        fi
        return 1
    fi

    echo "Windows artifacts: $artifacts_dir"

    if [ "$build_code" -ne 0 ]; then
        echo "[WARN] Windows build had failures; copied available executables."
        return "$build_code"
    fi

    return 0
}

run_android_ndk() {
    local stage="android-ndk"
    local build_dir="$ROOT_DIR/build-android"
    local toolchain_file="$ANDROID_NDK/build/cmake/android.toolchain.cmake"

    log_stage "Android NDK build"
    require_cmd cmake || return 1

    if [ ! -f "$toolchain_file" ]; then
        echo "[ERROR] Android NDK toolchain not found at: $toolchain_file"
        echo "        Set ANDROID_NDK=/path/to/ndk to override."
        return 1
    fi

    if [ "$CLEAN" -eq 1 ]; then
        rm -rf "$build_dir"
    fi

    run_cmd cmake -S "$ROOT_DIR" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
        -DANDROID_ABI="$ANDROID_ABI" \
        -DANDROID_PLATFORM="$ANDROID_PLATFORM" \
        -DCROSSOS_BUILD_EXAMPLES=OFF \
        -DCROSSOS_BUILD_TESTS=OFF || return 1
    run_cmd cmake --build "$build_dir" || return 1
    return 0
}

run_android_apk() {
    local stage="android-apk"
    local android_dir="$ROOT_DIR/android"
    local apk_artifacts_dir="$ROOT_DIR/artifacts/android-apk"
    local disc_apk_source="$android_dir/app/build/outputs/apk/discBurner/debug/app-discBurner-debug.apk"
    local hello_apk_source="$android_dir/app/build/outputs/apk/helloWorld/debug/app-helloWorld-debug.apk"
    local scanner_apk_source="$android_dir/app/build/outputs/apk/filmScanner/debug/app-filmScanner-debug.apk"
    local disc_apk_artifact="$apk_artifacts_dir/disc_burner.apk"
    local hello_apk_artifact="$apk_artifacts_dir/hello_world.apk"
    local scanner_apk_artifact="$apk_artifacts_dir/film_scanner.apk"

    log_stage "Android APK build"
    require_cmd bash || return 1

    if [ ! -f "$android_dir/gradlew" ]; then
        echo "[ERROR] Missing Gradle wrapper: $android_dir/gradlew"
        return 1
    fi

    if [ "$CLEAN" -eq 1 ]; then
        run_cmd bash "$android_dir/gradlew" -p "$android_dir" clean || return 1
    fi

    run_cmd bash "$android_dir/gradlew" -p "$android_dir" assembleDiscBurnerDebug || return 1
    run_cmd bash "$android_dir/gradlew" -p "$android_dir" assembleHelloWorldDebug || return 1
    run_cmd bash "$android_dir/gradlew" -p "$android_dir" assembleFilmScannerDebug || return 1

    if [ ! -f "$disc_apk_source" ]; then
        echo "[ERROR] Disc burner APK not found after build: $disc_apk_source"
        return 1
    fi

    if [ ! -f "$hello_apk_source" ]; then
        echo "[ERROR] Hello world APK not found after build: $hello_apk_source"
        return 1
    fi

    if [ ! -f "$scanner_apk_source" ]; then
        echo "[ERROR] Film scanner APK not found after build: $scanner_apk_source"
        return 1
    fi

    run_cmd mkdir -p "$apk_artifacts_dir" || return 1
    run_cmd rm -f "$apk_artifacts_dir/app-debug.apk" || return 1
    run_cmd cp "$disc_apk_source" "$disc_apk_artifact" || return 1
    run_cmd cp "$hello_apk_source" "$hello_apk_artifact" || return 1
    run_cmd cp "$scanner_apk_source" "$scanner_apk_artifact" || return 1
    echo "APK artifacts: $disc_apk_artifact, $hello_apk_artifact, $scanner_apk_artifact"
    return 0
}

summarize_and_exit() {
    local code="$1"
    printf "\n===== Build summary =====\n"

    if [ "${#PASSED_STAGES[@]}" -gt 0 ]; then
        printf "Passed: %s\n" "${PASSED_STAGES[*]}"
    else
        printf "Passed: (none)\n"
    fi

    if [ "${#FAILED_STAGES[@]}" -gt 0 ]; then
        printf "Failed: %s\n" "${FAILED_STAGES[*]}"
    else
        printf "Failed: (none)\n"
    fi

    exit "$code"
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --clean)
                CLEAN=1
                ;;
            --stop-on-error)
                KEEP_GOING=0
                ;;
            --linux-only)
                RUN_LINUX=1
                RUN_WINDOWS=0
                RUN_ANDROID_NDK=0
                RUN_ANDROID_APK=0
                ;;
            --windows-only)
                RUN_LINUX=0
                RUN_WINDOWS=1
                RUN_ANDROID_NDK=0
                RUN_ANDROID_APK=0
                ;;
            --android-ndk-only)
                RUN_LINUX=0
                RUN_WINDOWS=0
                RUN_ANDROID_NDK=1
                RUN_ANDROID_APK=0
                ;;
            --android-apk-only)
                RUN_LINUX=0
                RUN_WINDOWS=0
                RUN_ANDROID_NDK=0
                RUN_ANDROID_APK=1
                ;;
            --no-linux)
                RUN_LINUX=0
                ;;
            --no-windows)
                RUN_WINDOWS=0
                ;;
            --no-android-ndk)
                RUN_ANDROID_NDK=0
                ;;
            --no-android-apk)
                RUN_ANDROID_APK=0
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1"
                usage
                exit 2
                ;;
        esac
        shift
    done
}

PASSED_STAGES=()
FAILED_STAGES=()

main() {
    parse_args "$@"

    if [ "$RUN_LINUX" -eq 0 ] && [ "$RUN_WINDOWS" -eq 0 ] && [ "$RUN_ANDROID_NDK" -eq 0 ] && [ "$RUN_ANDROID_APK" -eq 0 ]; then
        echo "Nothing selected to build."
        usage
        exit 2
    fi

    if [ "$RUN_LINUX" -eq 1 ]; then
        run_linux
        mark_result "linux" "$?"
    fi

    if [ "$RUN_WINDOWS" -eq 1 ]; then
        run_windows
        mark_result "windows" "$?"
    fi

    if [ "$RUN_ANDROID_NDK" -eq 1 ]; then
        run_android_ndk
        mark_result "android-ndk" "$?"
    fi

    if [ "$RUN_ANDROID_APK" -eq 1 ]; then
        run_android_apk
        mark_result "android-apk" "$?"
    fi

    if [ "${#FAILED_STAGES[@]}" -gt 0 ]; then
        summarize_and_exit 1
    fi

    summarize_and_exit 0
}

main "$@"