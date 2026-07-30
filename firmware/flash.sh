#!/usr/bin/env bash
# Build and flash the board without touching the BOOT button.
#
#   ./flash.sh                build + flash + run
#   ./flash.sh --monitor      ... and stream the USB console
#   ./flash.sh --build-only   compile, do not touch the board
#   ./flash.sh --restore      flash the saved vendor firmware back
#
# How the button-free path works: picotool talks to the running firmware over
# its USB CDC/reset interface and asks it to reboot into BOOTSEL (-f), loads,
# then reboots back into the application (-x). This only holds while the
# firmware on the board is alive and has USB up -- which is why main() idles a
# few seconds before touching hardware. If a build hangs before USB enumerates,
# BOOT + RESET is the only way back.

set -euo pipefail
cd "$(dirname "$0")"

: "${PICO_SDK_PATH:=$HOME/pico-sdk}"
export PICO_SDK_PATH

BUILD_DIR=build
UF2="$BUILD_DIR/h0urg1ass.uf2"

# Where --restore looks for the factory image saved off the board before it was
# first overwritten. Capture one with:
#     picotool save -p -f vendor-backup/vendor-firmware.uf2
# Override with H0URG1ASS_BACKUP=/path/to/image.uf2 if you keep it elsewhere.
BACKUP="${H0URG1ASS_BACKUP:-vendor-backup/vendor-firmware.uf2}"

mode="${1:-}"

port() { ls /dev/cu.usbmodem* 2>/dev/null | head -1; }

flash() {
    local f="$1"
    if ! command -v picotool >/dev/null 2>&1; then
        echo "ERROR: picotool not found. brew install picotool" >&2
        exit 1
    fi
    # -f forces the reboot into BOOTSEL over the running firmware's USB
    # interface; -x runs the new image once loaded. No BOOT button, no
    # dependency on the BOOTSEL mass-storage volume mounting (macOS often
    # will not mount it after a software reset).
    if picotool load -f -x "$f"; then
        echo "flashed $(basename "$f")"
        return 0
    fi
    echo "" >&2
    echo "picotool could not reach the board. Recovery:" >&2
    echo "  hold BOOT, tap RESET, release BOOT, then re-run" >&2
    exit 1
}

if [ "$mode" = "--restore" ]; then
    [ -f "$BACKUP" ] || { echo "ERROR: no backup at $BACKUP" >&2; exit 1; }
    flash "$BACKUP"
    exit 0
fi

# Configure output is noise on a warm build, but a FAILURE must be visible --
# swallowing it turns a one-line compile error into a mystery.
if ! cmake -S . -B "$BUILD_DIR" -G Ninja >/tmp/h0urg1ass-cmake.log 2>&1; then
    cat /tmp/h0urg1ass-cmake.log >&2
    echo "ERROR: cmake configure failed" >&2
    exit 1
fi
if ! cmake --build "$BUILD_DIR" -j"$(sysctl -n hw.ncpu)" >/tmp/h0urg1ass-build.log 2>&1; then
    cat /tmp/h0urg1ass-build.log >&2
    echo "ERROR: build failed" >&2
    exit 1
fi
echo "built $(du -h "$UF2" | cut -f1) -> $UF2"

[ "$mode" = "--build-only" ] && exit 0

flash "$UF2"

if [ "$mode" = "--monitor" ]; then
    # Give the freshly-booted firmware time to re-enumerate its CDC port.
    for _ in $(seq 1 30); do
        P=$(port) && [ -n "$P" ] && break
        sleep 0.5
    done
    P=$(port)
    [ -n "$P" ] || { echo "ERROR: no USB serial port appeared" >&2; exit 1; }
    echo "monitoring $P (ctrl-c to stop)"
    exec cat "$P"
fi
