#!/bin/sh
# orOS Bridge as an AppImage: make-appimage.sh <oros-bridge binary> <appimagetool> <out file>
# Run from the repo root. GTK 3 is not bundled: it comes from the system,
# and without it the AppImage runs as a terminal program.
set -eu
bin=$1 tool=$2 out=$3
dir=$(mktemp -d)/oros-bridge.AppDir
mkdir -p "$dir/usr/bin" "$dir/usr/share/doc/oros-bridge"
cp "$bin" "$dir/usr/bin/oros-bridge"
cp README.md LICENSE THIRD_PARTY.md "$dir/usr/share/doc/oros-bridge/"
cp packaging/linux/oros-bridge.desktop "$dir/oros-bridge.desktop"
cp res/oros-bridge.png "$dir/oros-bridge.png"
ln -s usr/bin/oros-bridge "$dir/AppRun"
ARCH=x86_64 "$tool" --no-appstream "$dir" "$out"
