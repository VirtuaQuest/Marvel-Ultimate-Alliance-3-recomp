#!/bin/sh
# Use the generator's tested Python version even on newer Linux distributions.
set -eu
cd "$(dirname "$0")/.."
if [ -x .venv-linux/bin/python ] && .venv-linux/bin/python -c 'import sys; assert sys.version_info[:2] == (3, 12)' 2>/dev/null; then
    exit 0
fi
export UV_CACHE_DIR="$PWD/.uv-cache"
export UV_PYTHON_INSTALL_DIR="$PWD/.python"
export UV_LINK_MODE=copy
if command -v uv >/dev/null 2>&1; then
    setup_uv=$(command -v uv)
else
    python3 -m venv .venv-bootstrap
    .venv-bootstrap/bin/python -m pip install --disable-pip-version-check --progress-bar off uv
    setup_uv="$PWD/.venv-bootstrap/bin/uv"
fi
"$setup_uv" venv --python 3.12 --seed .venv-linux
.venv-linux/bin/python -c 'import sys; assert sys.version_info[:2] == (3, 12); print(sys.version)'
