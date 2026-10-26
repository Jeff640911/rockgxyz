#!/bin/sh
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
CONSOLE_UID=$(stat -f %u /dev/console)
launchctl bootout "gui/$CONSOLE_UID/com.rockgxyz.mboxmidi" 2>/dev/null
launchctl bootout system/com.rockgxyz.mboxd 2>/dev/null
rm -f /Library/LaunchDaemons/com.rockgxyz.mboxd.plist /Library/LaunchAgents/com.rockgxyz.mboxmidi.plist
rm -f /usr/local/libexec/mboxd /usr/local/libexec/mboxmidi /usr/local/libexec/mboxctl
rm -rf /Library/Audio/Plug-Ins/HAL/Mbox2.driver
killall coreaudiod 2>/dev/null
echo "uninstalled"
