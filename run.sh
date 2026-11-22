#!/bin/sh
set -eu
cd "$(dirname "$0")"
exec /bin/bash ./run-linux.sh "$@"
