#!/bin/sh
# Upgrade test for the Linux packages: test-upgrade.sh <dist dir> <new oros-bridge binary>
# Installs the latest released .deb (here), .rpm (in a Fedora container)
# and runs the released AppImage, lets each keep settings like a person's
# (a paired browser, an extra plugin folder), then puts the new package
# from <dist dir> over it with the same home folder. Checks that the
# program is the new build, that it starts, and that the settings in
# ~/.config/oros-bridge/config.json are still there after it ran.
# Run from the repo root after package-all.sh. Needs sudo, curl and Docker.
set -eu
base=https://github.com/koulaxizis/oros-bridge/releases/latest/download

# A paired browser (the bridge keeps sha1("oros-bridge:" + token)) and an
# extra plugin folder: what a person would lose if an upgrade reset them.
token=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
hash=$(printf 'oros-bridge:%s' "$token" | sha1sum | cut -d' ' -f1)
folder=/opt/upgrade-test-plugins

settings() { # settings <home>
  mkdir -p "$1/.config/oros-bridge"
  printf '{"tokens":["%s"],"folders":["%s"],"upgradeMarker":"kept"}\n' "$hash" "$folder" > "$1/.config/oros-bridge/config.json"
}

check_settings() { # check_settings <home> <when>
  c="$1/.config/oros-bridge/config.json"
  for want in "$hash" "$folder" '"upgradeMarker"'; do
    grep -q "$want" "$c" 2>/dev/null || { echo "$2: config.json lost $want" >&2; cat "$c" >&2 || true; exit 1; }
  done
  if [ -e "$c.bad" ]; then echo "$2: config.json was set aside as damaged" >&2; exit 1; fi
}

run_once() { # run_once <home> <command...>: starts the bridge, waits until it is ready, stops it
  h=$1; shift
  HOME="$h" APPIMAGE_EXTRACT_AND_RUN=1 "$@" --console --port 0 > "$h/bridge.log" 2>&1 &
  pid=$!
  i=0
  while [ $i -lt 40 ] && ! grep -q "is ready on 127.0.0.1:" "$h/bridge.log"; do sleep 0.5; i=$((i + 1)); done
  # the AppImage runtime starts the bridge as its child
  pkill -P "$pid" 2>/dev/null || true
  kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
  grep -q "is ready on 127.0.0.1:" "$h/bridge.log" || { echo "$* did not start" >&2; cat "$h/bridge.log" >&2; exit 1; }
}

# Inside the Fedora container: the .rpm part.
if [ "${1:-}" = --inside-rpm ]; then
  dnf install -y -q --setopt=install_weak_deps=False /old/oros-bridge-linux-x64.rpm
  was=$(rpm -q --qf '%{VERSION}' oros-bridge)
  h=$(mktemp -d); settings "$h"
  run_once "$h" oros-bridge
  check_settings "$h" "rpm $was before the upgrade"
  rpm -Uvh --replacepkgs --oldpackage /new/oros-bridge-linux-x64.rpm
  [ "$(sha256sum < /usr/bin/oros-bridge)" = "$(sha256sum < /new-bin/oros-bridge)" ] || { echo "rpm: /usr/bin/oros-bridge is not the new build" >&2; exit 1; }
  [ "$(rpm -q oros-bridge | wc -l)" = 1 ] || { echo "rpm: two oros-bridge packages installed" >&2; exit 1; }
  check_settings "$h" "rpm upgrade from $was"
  run_once "$h" oros-bridge
  check_settings "$h" "rpm upgrade from $was, after a start"
  echo "ok: rpm $was -> $(rpm -q --qf '%{VERSION}' oros-bridge) keeps the settings and starts"
  exit 0
fi

dist=$(cd "$1" && pwd)
bin=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
here=$(cd "$(dirname "$0")" && pwd)/$(basename "$0")
old=$(mktemp -d)

# The latest release (what people have installed), checked against its SHA256SUMS.txt.
for f in SHA256SUMS.txt oros-bridge-linux-x64.deb oros-bridge-linux-x64.rpm oros-bridge-linux-x64.AppImage; do
  curl -fsSL --retry 3 -o "$old/$f" "$base/$f"
done
(cd "$old" && tr -d '\r' < SHA256SUMS.txt | sed 's#  \./#  #' | grep -E '  oros-bridge-linux-x64\.(deb|rpm|AppImage)$' > linux.sums &&
  [ "$(wc -l < linux.sums)" = 3 ] && sha256sum -c linux.sums)
chmod +x "$old/oros-bridge-linux-x64.AppImage" "$dist/oros-bridge-linux-x64.AppImage"

echo "== .deb"
sudo apt-get install -y -q "$old/oros-bridge-linux-x64.deb"
was=$(dpkg-query -W -f '${Version}' oros-bridge)
h=$(mktemp -d); settings "$h"
run_once "$h" oros-bridge
check_settings "$h" "deb $was before the upgrade"
# dpkg -i installs over it whether the version is newer or the same
sudo dpkg -i "$dist/oros-bridge-linux-x64.deb"
[ "$(sha256sum < /usr/bin/oros-bridge)" = "$(sha256sum < "$bin")" ] || { echo "deb: /usr/bin/oros-bridge is not the new build" >&2; exit 1; }
[ "$(dpkg-query -W -f '${Version}' oros-bridge)" = "$(dpkg-deb -f "$dist/oros-bridge-linux-x64.deb" Version)" ]
test -f /usr/share/applications/oros-bridge.desktop
check_settings "$h" "deb upgrade from $was"
run_once "$h" oros-bridge
check_settings "$h" "deb upgrade from $was, after a start"
echo "ok: deb $was -> $(dpkg-query -W -f '${Version}' oros-bridge) keeps the settings and starts"

echo "== .rpm (Fedora)"
docker run --rm -v "$old:/old:ro" -v "$dist:/new:ro" -v "$(dirname "$bin"):/new-bin:ro" -v "$here:/test-upgrade.sh:ro" \
  fedora:42 sh /test-upgrade.sh --inside-rpm

echo "== AppImage"
# An AppImage is replaced as a file; the home folder is all that carries over.
h=$(mktemp -d); settings "$h"
run_once "$h" "$old/oros-bridge-linux-x64.AppImage"
check_settings "$h" "the released AppImage"
run_once "$h" "$dist/oros-bridge-linux-x64.AppImage"
check_settings "$h" "the new AppImage over the released one"
grep -q "orOS Bridge $(sed -n 's/^project(oros_bridge VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt) is ready" "$h/bridge.log" ||
  { echo "the new AppImage is not this version" >&2; cat "$h/bridge.log" >&2; exit 1; }
echo "ok: the AppImage keeps the settings and starts"
