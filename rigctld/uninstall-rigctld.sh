#!/usr/bin/env bash
#
# Remove the rigctld systemd service installed by setup-rigctld.sh.
#   Run with:  sudo ./uninstall-rigctld.sh
#
# By default this leaves the 'rigctld' system user and the libhamlib-utils
# package in place (they're harmless and you may want them again). Commands to
# remove those fully are printed at the end.
#
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Must run as root. Re-run:  sudo $0" >&2
    exit 1
fi

echo ">> Stopping + disabling rigctld.service..."
systemctl disable --now rigctld.service 2>/dev/null || true

echo ">> Removing unit file..."
rm -f /etc/systemd/system/rigctld.service
systemctl daemon-reload

echo
echo ">> rigctld service removed."
echo "   To also remove the service user:   sudo deluser --system rigctld"
echo "   To also remove the rigctld binary: sudo apt-get remove libhamlib-utils"
