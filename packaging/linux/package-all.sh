#!/bin/sh
# Every Linux download of orOS Bridge from one build: package-all.sh <out dir>
# Run from the repo root after `cmake --build build`. Makes
#   oros-bridge-linux-x64.tar.gz   the program, README, licences (as since 0.3.0)
#   oros-bridge-linux-x64.deb      Debian, Ubuntu, Mint… (nfpm)
#   oros-bridge-linux-x64.rpm      Fedora, openSUSE… (nfpm)
#   oros-bridge-linux-x64.AppImage any distribution, no install (appimagetool)
# Asset names stay the same between versions, so releases/latest/download/<name> always works.
# Needs Go (for nfpm) and curl. The tools are pinned and the AppImage tool is checked by its SHA-256.
set -eu
out=$1
NFPM=v2.43.0
APPIMAGETOOL=1.9.0
APPIMAGETOOL_SHA256=46fdd785094c7f6e545b61afcfb0f3d98d8eab243f644b4b17698c01d06083d1
v=$(sed -n 's/^project(oros_bridge VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
mkdir -p "$out"
tools=$(mktemp -d)

pkg=$(mktemp -d)
mkdir -p "$pkg/oros-bridge"
cp build/oros-bridge README.md LICENSE THIRD_PARTY.md "$pkg/oros-bridge/"
tar -C "$pkg" -czf "$out/oros-bridge-linux-x64.tar.gz" oros-bridge

GOBIN="$tools" go install "github.com/goreleaser/nfpm/v2/cmd/nfpm@$NFPM"
for f in deb rpm; do
  VERSION=$v "$tools/nfpm" package -f packaging/linux/nfpm.yaml -p $f -t "$tools/pkg.$f"
  mv "$tools/pkg.$f" "$out/oros-bridge-linux-x64.$f"
done

curl -fsSL -o "$tools/appimagetool" "https://github.com/AppImage/appimagetool/releases/download/$APPIMAGETOOL/appimagetool-x86_64.AppImage"
echo "$APPIMAGETOOL_SHA256  $tools/appimagetool" | sha256sum -c -
chmod +x "$tools/appimagetool"
APPIMAGE_EXTRACT_AND_RUN=1 sh packaging/linux/make-appimage.sh build/oros-bridge "$tools/appimagetool" "$out/oros-bridge-linux-x64.AppImage"
ls -l "$out"
