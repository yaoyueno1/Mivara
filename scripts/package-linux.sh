#!/usr/bin/env bash
# Portable Linux tarball of sender, receiver and app.
# An AppImage is attempted only when appimagetool is already on PATH.
# This script does not download that tool. dist/ is gitignored.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ver=$(sed -n 's/^#define MIVARA_VERSION "\([^"]*\)".*/\1/p' src/version.h | head -n 1)
if [ -z "$ver" ]; then
    echo "package-linux: could not read MIVARA_VERSION from src/version.h" >&2
    exit 1
fi

make OPT_FLAGS="-O3 -march=x86-64 -flto" sender receiver app

name="Mivara-${ver}-linux-x86_64"
stage="dist/${name}"
rm -rf "$stage"
mkdir -p "$stage/bin"
cp -a bin/sender bin/receiver bin/app "$stage/bin/"
chmod 755 "$stage/bin/sender" "$stage/bin/receiver" "$stage/bin/app"

cat > "$stage/mivara.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Mivara
Comment=LAN screen mirroring. After moving this folder, set Exec to the absolute path of bin/app.
Exec=bin/app
Terminal=false
Categories=Network;Utility;
EOF

cat > "$stage/README" <<EOF
Mivara ${ver} for Linux x86_64.

bin/sender, bin/receiver and bin/app were built with
-O3 -march=x86-64 -flto. They still link against the distro's
libstdc++, SDL2, FFmpeg, X11, and (when those headers were present
at build time) PipeWire and libdbus.

Wayland capture needs xdg-desktop-portal and PipeWire at runtime.
Extend mode enables a disconnected VIRTUAL* output, or a Sway or
Hyprland headless output. It does not load a kernel module.

The .desktop file in this folder uses a relative Exec. Edit it after
you move the folder. The in-app autostart toggle writes
~/.config/autostart/mivara.desktop with the absolute path of the
running app.
EOF

tar -C dist -czf "dist/${name}.tar.gz" "$name"
echo "package-linux: dist/${name}.tar.gz"

if ! command -v appimagetool >/dev/null 2>&1; then
    echo "package-linux: appimagetool is not on PATH; the tarball is the package"
    exit 0
fi

appdir="dist/${name}.AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/bin"
cp -a "$stage/bin/." "$appdir/usr/bin/"
cat > "$appdir/mivara.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Mivara
Exec=app
Icon=mivara
Categories=Network;Utility;
EOF
printf '%s\n' '<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64"><rect width="64" height="64" fill="#2f2f30"/></svg>' > "$appdir/mivara.svg"
cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
exec "$here/usr/bin/app" "$@"
EOF
chmod 755 "$appdir/AppRun"

export ARCH="${ARCH:-x86_64}"
if appimagetool "$appdir" "dist/${name}.AppImage"; then
    echo "package-linux: dist/${name}.AppImage (still needs the distro libraries)"
else
    echo "package-linux: appimagetool failed; the tarball is the package" >&2
fi
