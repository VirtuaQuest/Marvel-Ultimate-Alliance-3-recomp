#!/bin/sh
# SteamOS / Linux: open the native GUI launcher from this source folder.
set -eu
cd "$(dirname "$0")/.."
exec ./run-linux.sh "$@"
