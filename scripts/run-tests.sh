#!/usr/bin/env sh
set -eu

LEAK_CHECK=0
BUILD_DIR=build

while [ "$#" -gt 0 ]; do
    case "$1" in
        --leak-check)
            LEAK_CHECK=1
            shift
            ;;
        --help|-h)
            echo "usage: $0 [--leak-check] [build-dir]" >&2
            exit 0
            ;;
        -*)
            echo "unknown option: $1" >&2
            echo "usage: $0 [--leak-check] [build-dir]" >&2
            exit 2
            ;;
        *)
            BUILD_DIR=$1
            shift
            ;;
    esac
done

if [ "$LEAK_CHECK" = "1" ]; then
    if [ "$(uname -s)" = "Darwin" ]; then
        echo "warning: LeakSanitizer is not supported by Apple AddressSanitizer; running ASan/UBSan checks without leak detection" >&2
        export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0:detect_odr_violation=0:halt_on_error=1:abort_on_error=1:strict_init_order=1}"
    else
        export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=1:detect_odr_violation=0:halt_on_error=1:abort_on_error=1:strict_init_order=1}"
    fi
    export LSAN_OPTIONS="${LSAN_OPTIONS:-halt_on_error=1:print_suppressions=0}"
    export UBSAN_OPTIONS="${UBSAN_OPTIONS:-halt_on_error=1:print_stacktrace=1}"

    meson test -C "$BUILD_DIR" \
        dhcp-cb-cmds-tests \
        dhcp-cb-cmds-libload-tests \
        --print-errorlogs
else
    meson test -C "$BUILD_DIR" \
        dhcp-cb-cmds-tests \
        dhcp-cb-cmds-libload-tests \
        dhcp-cb-cmds-api-coverage \
        kea-config-backend-tests \
        --print-errorlogs
fi

if [ "${KEA_CB_CMDS_DB_TESTS:-0}" = "1" ]; then
    KEA_CB_CMDS_DB_TESTS=1 KEA_TEST_DB_WIPE_DATA_ONLY="${KEA_TEST_DB_WIPE_DATA_ONLY:-false}" \
        meson test -C "$BUILD_DIR" dhcp-cb-cmds-db-tests --print-errorlogs
fi

if [ "${KEA_CB_CMDS_ARM_CONFORMANCE:-0}" = "1" ] || \
    [ -n "${CB_PG_PASSWORD:-}" ] || \
    [ -n "${CB_MYSQL_PASSWORD:-}" ] || \
    [ -n "${CB_MY_PASSWORD:-}" ]; then
    python3 "$(dirname "$0")/arm_conformance.py"
fi
