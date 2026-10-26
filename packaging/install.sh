#!/bin/sh
# Installs the driver from a CMake build directory (default: ./build).
# Run with sudo on macOS.
set -eu

BUILD=${1:-build}
HAL_DIR=/Library/Audio/Plug-Ins/HAL
LIBEXEC=/usr/local/libexec
HERE=$(cd "$(dirname "$0")" && pwd)

if [ "$(uname)" != Darwin ]; then
    echo "install.sh is for macOS" >&2
    exit 1
fi
if [ "$(id -u)" != 0 ]; then
    echo "run with sudo" >&2
    exit 1
fi

mkdir -p "$LIBEXEC" "$HAL_DIR"
install -m 755 "$BUILD/mboxd" "$BUILD/mboxmidi" "$BUILD/mboxctl" "$LIBEXEC/"
rm -rf "$HAL_DIR/Mbox2.driver"
cp -R "$BUILD/Mbox2.driver" "$HAL_DIR/"
# coreaudiod refuses unsigned plug-ins on Apple silicon; ad-hoc sign for
# local use. Distribution needs a Developer ID signature and notarization.
codesign --force --deep --sign - "$HAL_DIR/Mbox2.driver"
codesign --force --sign - "$LIBEXEC/mboxd" "$LIBEXEC/mboxmidi" "$LIBEXEC/mboxctl"

install -m 644 "$HERE/com.rockgxyz.mboxd.plist" /Library/LaunchDaemons/
install -m 644 "$HERE/com.rockgxyz.mboxmidi.plist" /Library/LaunchAgents/

launchctl bootout system/com.rockgxyz.mboxd 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/com.rockgxyz.mboxd.plist

CONSOLE_UID=$(stat -f %u /dev/console)
launchctl bootout "gui/$CONSOLE_UID/com.rockgxyz.mboxmidi" 2>/dev/null || true
launchctl bootstrap "gui/$CONSOLE_UID" /Library/LaunchAgents/com.rockgxyz.mboxmidi.plist || true

killall coreaudiod 2>/dev/null || true
echo "installed; logs: /var/log/mboxd.log"
