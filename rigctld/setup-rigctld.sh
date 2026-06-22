#!/usr/bin/env bash
#
# Install rigctld as a systemd service, so lockstep (and WSJT-X, loggers, ...)
# can share the radio's single CAT port through it.
#
#   Run with:  sudo ./setup-rigctld.sh
#
# This is intentionally a script you invoke under sudo yourself (not a Makefile
# that calls sudo for you), so every privileged step is visible. It is
# idempotent: safe to re-run after editing rigctld.service.
#
# Mirrors these manual steps:
#   sudo cp rigctld.service /etc/systemd/system/
#   sudo adduser rigctld --system --group --home /var/lib/rigctld
#   sudo adduser rigctld dialout
#   sudo usermod rigctld --expiredate 1
#   sudo systemctl daemon-reload
#   sudo systemctl enable --now rigctld.service
#
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UNIT_SRC="$HERE/rigctld.service"
UNIT_DST="/etc/systemd/system/rigctld.service"
SVC_USER="rigctld"
SVC_HOME="/var/lib/rigctld"

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root. Re-run:  sudo $0" >&2
    exit 1
fi
[ -f "$UNIT_SRC" ] || { echo "Missing unit file: $UNIT_SRC" >&2; exit 1; }

echo ">> Ensuring rigctld is installed (package: libhamlib-utils)..."
if ! command -v rigctld >/dev/null 2>&1; then
    apt-get update
    apt-get install -y libhamlib-utils
else
    echo "   rigctld already present: $(command -v rigctld)"
fi

echo ">> Creating system user '$SVC_USER' (if missing)..."
if ! getent passwd "$SVC_USER" >/dev/null; then
    adduser "$SVC_USER" --system --group --home "$SVC_HOME"
else
    echo "   user '$SVC_USER' already exists"
fi

echo ">> Granting serial access (dialout group) + locking interactive login..."
usermod -aG dialout "$SVC_USER"
usermod "$SVC_USER" --expiredate 1     # account can't be logged into; service still runs

echo ">> Installing unit -> $UNIT_DST ..."
install -m 0644 "$UNIT_SRC" "$UNIT_DST"

echo ">> Enabling + (re)starting service..."
systemctl daemon-reload
systemctl enable rigctld.service
systemctl restart rigctld.service
sleep 1
systemctl --no-pager status rigctld.service || true

echo
echo ">> Done. Verify CAT control reaches the radio:"
echo "     echo 'f' | nc -q1 localhost 4532    # should print the dial frequency"
echo "   If that errors, check: the radio is on, the serial device/baud in"
echo "   rigctld.service are correct, then: sudo systemctl restart rigctld"
