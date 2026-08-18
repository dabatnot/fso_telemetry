#!/usr/bin/env bash
set -euo pipefail

tool_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$tool_root/../.." && pwd)"
build_root="$repo_root/build/av-core"
backend_root="$tool_root/backend"
frontend_root="$tool_root/frontend"
config_path="${AV_CORE_CONFIG_PATH:-$build_root/av-core.json}"
host_address="${AV_CORE_HOST:-127.0.0.1}"

mkdir -p "$build_root"
if [[ ! -x "$build_root/.venv/bin/python" ]]; then
    python3 -m venv "$build_root/.venv"
fi
"$build_root/.venv/bin/python" -m pip install --disable-pip-version-check -r "$backend_root/requirements.lock.txt"

(
    cd "$frontend_root"
    npm ci
    npm run build
)

export PYTHONPATH="$backend_root"
exec "$build_root/.venv/bin/python" -m av_core \
    --host "$host_address" \
    --config "$config_path" \
    --frontend-dir "$build_root/frontend"
