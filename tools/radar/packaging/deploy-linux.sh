#!/usr/bin/env bash
set -euo pipefail

build_directory="${1:?build directory required}"
output_directory="${2:?output directory required}"
repository_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
executable="${build_directory}/bin/FsoSimpitRadar"
test -x "${executable}"

mkdir -p "${output_directory}/AppDir/usr/bin" "${output_directory}/AppDir/usr/share/applications"
mkdir -p "${output_directory}/AppDir/usr/share/icons/hicolor/16x16/apps"
install -m755 "${executable}" "${output_directory}/AppDir/usr/bin/FsoSimpitRadar"
install -m644 "${repository_root}/tools/radar/packaging/fso-simpit-radar.desktop" \
  "${output_directory}/AppDir/usr/share/applications/fso-simpit-radar.desktop"
install -m644 "${repository_root}/tools/radar/packaging/fso-simpit-radar.xpm" \
  "${output_directory}/AppDir/usr/share/icons/hicolor/16x16/apps/fso-simpit-radar.xpm"
mkdir -p "${output_directory}/AppDir/usr/share/licenses/FsoSimpitRadar"
install -m644 "${repository_root}/Copying.md" "${repository_root}/Unlicense.md" \
  "${output_directory}/AppDir/usr/share/licenses/FsoSimpitRadar/"
install -m644 "${repository_root}/tools/radar/THIRD_PARTY_NOTICES.md" \
  "${output_directory}/AppDir/usr/share/licenses/FsoSimpitRadar/"
curl -L --fail --silent --show-error https://www.gnu.org/licenses/lgpl-3.0.txt \
  -o "${output_directory}/AppDir/usr/share/licenses/FsoSimpitRadar/Qt-LGPL-3.0.txt"
for notice in /usr/share/doc/qt6-base-dev/copyright /usr/share/doc/qt6-wayland/copyright; do
  if [[ -f "${notice}" ]]; then
    install -m644 "${notice}" \
      "${output_directory}/AppDir/usr/share/licenses/FsoSimpitRadar/$(basename "$(dirname "${notice}")")-copyright"
  fi
done

: "${LINUXDEPLOY:?set LINUXDEPLOY to the linuxdeploy AppImage}"
: "${LINUXDEPLOY_PLUGIN_QT:?set LINUXDEPLOY_PLUGIN_QT to the Qt plugin AppImage}"
export QMAKE="${QMAKE:-qmake6}"
"${LINUXDEPLOY}" --appdir "${output_directory}/AppDir" \
  --desktop-file "${output_directory}/AppDir/usr/share/applications/fso-simpit-radar.desktop" \
  --executable "${output_directory}/AppDir/usr/bin/FsoSimpitRadar" --plugin qt

# linuxdeploy-plugin-qt deploys XCB by default. Keep the same AppImage native
# on Wayland by adding Qt's client-side platform/shell integrations and asking
# linuxdeploy to resolve their ELF dependencies and rpaths.
qt_plugins="$("${QMAKE}" -query QT_INSTALL_PLUGINS)"
for plugin_directory in wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
  if [[ -d "${qt_plugins}/${plugin_directory}" ]]; then
    mkdir -p "${output_directory}/AppDir/usr/plugins/${plugin_directory}"
    cp -a "${qt_plugins}/${plugin_directory}/." \
      "${output_directory}/AppDir/usr/plugins/${plugin_directory}/"
  fi
done
if compgen -G "${qt_plugins}/platforms/libqwayland*.so" >/dev/null; then
  cp -a "${qt_plugins}"/platforms/libqwayland*.so \
    "${output_directory}/AppDir/usr/plugins/platforms/"
  dependency_arguments=()
  while IFS= read -r -d '' plugin; do
    dependency_arguments+=(--deploy-deps-only "${plugin}")
  done < <(find "${output_directory}/AppDir/usr/plugins/platforms" \
                 "${output_directory}/AppDir/usr/plugins/wayland-decoration-client" \
                 "${output_directory}/AppDir/usr/plugins/wayland-graphics-integration-client" \
                 "${output_directory}/AppDir/usr/plugins/wayland-shell-integration" \
                 -type f -name '*.so' -print0)
  "${LINUXDEPLOY}" --appdir "${output_directory}/AppDir" "${dependency_arguments[@]}"
fi

"${LINUXDEPLOY}" --appdir "${output_directory}/AppDir" --output appimage
mv ./FSO_SimPit_Radar-x86_64.AppImage \
  "${output_directory}/FsoSimpitRadar-x86_64.AppImage"
