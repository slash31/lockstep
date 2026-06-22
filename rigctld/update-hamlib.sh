#!/usr/bin/env bash
#
# Build a current Hamlib from source into /usr/local and point the rigctld
# service at it -- WITHOUT touching the distro's packaged Hamlib. Used to get
# newer Icom VFO/memory-mode handling than the Ubuntu package ships.
#
#   Run with:  sudo ./update-hamlib.sh [version]
#   e.g.       sudo ./update-hamlib.sh            # defaults to $HAMLIB_VER below
#              sudo ./update-hamlib.sh 4.7.2
#
# Safe + reversible:
#   * Installs to /usr/local (distro package in /usr stays intact).
#   * Only the rigctld.service ExecStart is repointed to /usr/local/bin/rigctld;
#     other apps (WSJT-X, etc.) keep using the packaged libhamlib.
#   * To revert: point ExecStart back to /usr/bin/rigctld + daemon-reload, or
#     `sudo rm -rf /usr/local/.../*hamlib*` (see the README).
#
set -euo pipefail

HAMLIB_VER="${1:-4.7.2}"
SRC_DIR="/usr/local/src"
TARBALL="hamlib-${HAMLIB_VER}.tar.gz"
URL="https://github.com/Hamlib/Hamlib/releases/download/${HAMLIB_VER}/${TARBALL}"
UNIT="/etc/systemd/system/rigctld.service"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root. Re-run:  sudo $0 ${HAMLIB_VER}" >&2
    exit 1
fi
for t in gcc make pkg-config curl tar; do
    command -v "$t" >/dev/null || { echo "Missing build tool: $t" >&2; exit 1; }
done

echo ">> Fetching Hamlib ${HAMLIB_VER} -> ${SRC_DIR}/${TARBALL}"
mkdir -p "$SRC_DIR"
cd "$SRC_DIR"
curl -fsSL -o "$TARBALL" "$URL"
tar xzf "$TARBALL"
cd "hamlib-${HAMLIB_VER}"

# Release tarballs ship a pre-generated ./configure, so no autotools/libtool
# are needed. --without-readline keeps it dependency-light (rigctld doesn't
# need line editing); drop it if you want interactive rigctl history.
echo ">> Configuring (prefix=/usr/local)..."
./configure --prefix=/usr/local --disable-static --without-readline

echo ">> Building..."
make -j"$(nproc)"

echo ">> Installing to /usr/local + refreshing linker cache..."
make install
ldconfig

echo ">> Repointing rigctld service at /usr/local/bin/rigctld (if installed)..."
if [ -f "$UNIT" ]; then
    sed -i 's#/usr/bin/rigctld#/usr/local/bin/rigctld#' "$UNIT"
    systemctl daemon-reload
    systemctl restart rigctld.service || true
fi

echo
echo ">> Done. Versions:"
echo "   packaged : $(/usr/bin/rigctld --version 2>&1 | head -1)"
echo "   new      : $(/usr/local/bin/rigctld --version 2>&1 | head -1)"
echo "   service  : $(grep ExecStart "$UNIT" 2>/dev/null || echo '(no rigctld.service)')"
