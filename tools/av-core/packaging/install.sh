#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Cet installateur doit être lancé avec sudo." >&2
    exit 1
fi

tool_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
repo_root="$(cd "$tool_root/../.." && pwd)"
build_root="$repo_root/build/av-core"
install_root="/opt/fsotelemetry/av-core"

if ! grep -qE 'VERSION_CODENAME=(bookworm|trixie)' /etc/os-release; then
    echo "Attention : cet installateur cible Raspberry Pi OS Bookworm 64 bits." >&2
fi

apt-get update
apt-get install -y python3 python3-venv nodejs npm

(
    cd "$tool_root/frontend"
    npm ci
    npm run build
)

if ! getent group fsotelemetry >/dev/null; then
    groupadd --system fsotelemetry
fi
if ! id fsotelemetry >/dev/null 2>&1; then
    useradd --system --gid fsotelemetry --home-dir /var/lib/fsotelemetry --no-create-home --shell /usr/sbin/nologin fsotelemetry
fi

systemctl stop av-core.service 2>/dev/null || true
install -d -m 0755 "$install_root/backend" "$install_root/frontend"
rm -rf "$install_root/backend/av_core" "$install_root/frontend/assets"
cp -a "$tool_root/backend/av_core" "$install_root/backend/"
cp -a "$build_root/frontend/." "$install_root/frontend/"
python3 -m venv "$install_root/.venv"
"$install_root/.venv/bin/python" -m pip install --disable-pip-version-check -r "$tool_root/backend/requirements.lock.txt"
chown -R root:root "$install_root"

install -m 0644 "$tool_root/packaging/av-core.service" /etc/systemd/system/av-core.service
systemctl daemon-reload
systemctl enable --now av-core.service

echo "AV CORE est installé. Ouvrez http://<ip-du-raspberry>:8080"
