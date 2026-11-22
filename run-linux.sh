#!/usr/bin/env bash
# Desktop entry point: normal launches show only the application window.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
root=$PWD
# Desktop entry points always open the GUI, even when Steam variables were
# inherited from the desktop session. Only an explicit flag selects direct play.
if [[ ${1:-} == --game-mode ]]; then
    shift
    exec bash "$root/play-steam.sh" "$@"
fi
if [[ ${1:-} == --launcher ]]; then shift; fi
# Keep terminal output when explicitly started from a shell. Desktop launches
# write directly to a log without creating a terminal window. RecompLauncher's
# setup step (--prepare) reads the "progress" lines on stdout, so it keeps them too.
prepare=0
for arg in "$@"; do [[ $arg == --prepare ]] && prepare=1; done
if [[ -t 1 ]] || ((prepare)); then
    exec > >(tee -a "$root/launcher-start.log") 2>&1
else
    exec >> "$root/launcher-start.log" 2>&1
fi
printf '\nStarting MUA3 Recomp from: %s\n' "$root"
result=0
if [[ ! -f "$root/tools/launch_linux.sh" || ! -f "$root/tools/steamdeck_setup.sh" ]]; then
    echo 'Incomplete launcher update: extract the complete startup repair ZIP into this folder, including its tools directory.'
    result=1
else
    bash "$root/tools/launch_linux.sh" "$@" || result=$?
fi
if ((result)); then
    message="MUA3 Recomp could not start (exit $result).
Details: $root/launcher-start.log"
    echo "$message"
    if ((prepare)); then
        :  # (RecompLauncher shows the error itself)
    elif command -v kdialog >/dev/null 2>&1; then
        kdialog --title 'MUA3 Recomp' --error "$message" || true
    elif command -v zenity >/dev/null 2>&1; then
        zenity --error --title='MUA3 Recomp' --text="$message" || true
    fi
fi
exit "$result"
