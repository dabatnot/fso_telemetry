#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID}" -ne 0 ]]; then
    echo "Cet installateur doit être lancé avec sudo." >&2
    exit 1
fi

set_hostname=false
if [[ $# -gt 0 ]]; then
    if [[ $# -eq 2 && "$1" == "--set-hostname" && "$2" == "av-core" ]]; then
        set_hostname=true
    else
        echo "Usage : sudo ./install.sh [--set-hostname av-core]" >&2
        exit 2
    fi
fi

bundle_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
for required in VERSION backend/av_core frontend/index.html fstl-client requirements.lock.txt av-core.service av-core-can.service av-core-can-up; do
    if [[ ! -e "$bundle_root/$required" ]]; then
        echo "Bundle incomplet : $required est absent. Construisez l'archive avec packaging/build_release.py." >&2
        exit 3
    fi
done

version="$(tr -d '\r\n' < "$bundle_root/VERSION")"
if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Version de bundle invalide : $version" >&2
    exit 3
fi

source /etc/os-release
architecture="$(dpkg --print-architecture 2>/dev/null || uname -m)"
if [[ "${ID:-}" != "raspbian" || "${VERSION_CODENAME:-}" != "bookworm" || "$architecture" != "arm64" ]]; then
    echo "Attention : la cible officielle est Raspberry Pi OS Bookworm 64 bits (détecté : ${PRETTY_NAME:-inconnu}, $architecture)." >&2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y python3 python3-venv iproute2 avahi-daemon ca-certificates

if ! getent group fsotelemetry >/dev/null; then
    groupadd --system fsotelemetry
fi
if ! id fsotelemetry >/dev/null 2>&1; then
    useradd --system --gid fsotelemetry --home-dir /var/lib/fsotelemetry --no-create-home --shell /usr/sbin/nologin fsotelemetry
fi
install -d -o fsotelemetry -g fsotelemetry -m 0750 /var/lib/fsotelemetry

install_base=/opt/fsotelemetry/av-core
releases_root="$install_base/releases"
install -d -m 0755 "$releases_root" /usr/local/libexec
release_path="$releases_root/$version"
stage_path="$(mktemp -d "$releases_root/.stage-$version-XXXXXX")"
trap 'if [[ -n "${stage_path:-}" && -d "$stage_path" ]]; then rm -rf -- "$stage_path"; fi' EXIT
chmod 0755 "$stage_path"

install -d -m 0755 "$stage_path/backend" "$stage_path/frontend" "$stage_path/fstl-client"
cp -a "$bundle_root/backend/av_core" "$stage_path/backend/"
cp -a "$bundle_root/frontend/." "$stage_path/frontend/"
cp -a "$bundle_root/fstl-client/." "$stage_path/fstl-client/"
install -m 0644 "$bundle_root/requirements.lock.txt" "$stage_path/requirements.lock.txt"
python3 -m venv "$stage_path/.venv"
"$stage_path/.venv/bin/python" -m pip install --disable-pip-version-check -r "$stage_path/requirements.lock.txt"
chown -R root:root "$stage_path"

systemctl stop av-core.service 2>/dev/null || true
if [[ -e "$release_path" ]]; then
    rm -rf -- "$release_path"
fi
mv "$stage_path" "$release_path"
stage_path=""
ln -sfn "releases/$version" "$install_base/current.new"
mv -Tf "$install_base/current.new" "$install_base/current"

install -m 0755 "$bundle_root/av-core-can-up" /usr/local/libexec/av-core-can-up
install -m 0644 "$bundle_root/av-core-can.service" /etc/systemd/system/av-core-can.service
install -m 0644 "$bundle_root/av-core.service" /etc/systemd/system/av-core.service

# Migre l'ancien layout plat. La configuration persistante reste sous /var/lib.
rm -rf -- "$install_base/backend" "$install_base/frontend" "$install_base/fstl-client" "$install_base/.venv"

systemctl daemon-reload
systemctl enable avahi-daemon.service av-core-can.service av-core.service
if "$set_hostname"; then
    hostnamectl set-hostname av-core
    systemctl restart avahi-daemon.service
else
    systemctl start avahi-daemon.service
fi
systemctl restart av-core-can.service || true
systemctl restart av-core.service

hostname_value="$(hostnamectl --static 2>/dev/null || hostname)"
echo "AV CORE $version est installé."
echo "Ouvrez http://${hostname_value}.local:8080 ou http://<ip-du-raspberry>:8080"
if [[ "$set_hostname" == true ]]; then
    echo "Le nom mDNS demandé est http://av-core.local:8080"
fi
