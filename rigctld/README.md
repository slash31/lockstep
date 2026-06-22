# rigctld setup (the pre-work lockstep needs)

lockstep doesn't talk to the radio directly — it talks to **`rigctld`**, Hamlib's
network daemon, over TCP (`localhost:4532`). rigctld owns the radio's single
serial/CAT port and multiplexes it, so lockstep *and* other CAT software (WSJT-X,
a logger) can all use the radio at the same time.

This folder makes that setup reproducible:

| File                   | What it is                                             |
|------------------------|--------------------------------------------------------|
| `rigctld.service`      | the systemd unit (rig model, serial device, port, baud)|
| `setup-rigctld.sh`     | idempotent installer — run with `sudo`                 |
| `uninstall-rigctld.sh` | removes the service                                    |

## Dependencies

- **`libhamlib-utils`** — the Debian/Ubuntu package that provides the `rigctld`
  and `rigctl` binaries. `setup-rigctld.sh` installs it automatically (via `apt`)
  if `rigctld` isn't already on the system.

(That's the only dependency for *running* the radio side. Building the lockstep
SDR++ module is separate — see the top-level `README.md`. lockstep itself does
**not** link Hamlib.)

## Quick install

```bash
cd rigctld
sudo ./setup-rigctld.sh
```

It installs `libhamlib-utils` if needed, creates a locked-down `rigctld` system
user with serial access, installs the unit, and enables + starts the service.
Re-run it any time after editing `rigctld.service`.

Verify the radio is reachable:

```bash
echo 'f' | nc -q1 localhost 4532     # prints the current dial frequency
```

## Your radio's settings

`rigctld.service` is configured for the setup it was captured from — an **Icom
IC-7100 at 19200 baud**:

```
ExecStart=/usr/bin/rigctld -m 3070 -r /dev/ttyUSB0 -t 4532 -s 19200
```

- `-m 3070` — Hamlib model (3070 = IC-7100; find others with `rigctl -l`).
- `-r /dev/ttyUSB0` — serial device.
- `-t 4532` — TCP port lockstep/WSJT-X connect to.
- `-s 19200` — serial speed; **must match the radio's CI-V baud rate** (Menu/SET).

To change any of these: edit `rigctld.service`, then `sudo ./setup-rigctld.sh`
again (or `sudo systemctl daemon-reload && sudo systemctl restart rigctld`).

## What the installer does (manual equivalent)

These are the exact privileged steps, if you'd rather do them by hand:

```bash
sudo cp rigctld.service /etc/systemd/system/rigctld.service
sudo adduser rigctld --system --group --home /var/lib/rigctld   # service account
sudo adduser rigctld dialout                                    # serial port access
sudo usermod rigctld --expiredate 1                             # lock interactive login
sudo systemctl daemon-reload
sudo systemctl enable rigctld.service
sudo systemctl start rigctld.service
sudo systemctl status rigctld.service
```

## Upgrading Hamlib (newer Icom VFO/memory handling)

Ubuntu ships an older Hamlib (e.g. 4.5.5). Newer releases have ongoing fixes to
how Icom rigs are handled on connect (the IC-7100 gets yanked from memory mode to
VFO at `rig_open` — a Hamlib-level behavior, not lockstep). To try a current
Hamlib:

```bash
cd rigctld
sudo ./update-hamlib.sh          # builds the default version into /usr/local
sudo ./update-hamlib.sh 4.7.2    # or pin a specific version
```

It downloads the release tarball, builds it (no autotools needed — the tarball
ships `./configure`), installs to `/usr/local`, runs `ldconfig`, and repoints the
rigctld service at `/usr/local/bin/rigctld`. The new `libhamlib` keeps soname
`.so.4`, so it's an ABI-compatible drop-in.

**Scope:** with `ldconfig`, `/usr/local/lib` precedes the distro path, so all
Hamlib apps use the new version. That's usually desirable. The distro package is
left in place, so reverting is easy:

```bash
# point the service back at the packaged binary:
sudo sed -i 's#/usr/local/bin/rigctld#/usr/bin/rigctld#' /etc/systemd/system/rigctld.service
sudo systemctl daemon-reload && sudo systemctl restart rigctld
# and (optional) remove the /usr/local copy:
sudo rm -f /usr/local/bin/rigctl* /usr/local/lib/libhamlib.* ; sudo ldconfig
```

Verify which is active: `rigctld --version` and `ldd $(which rigctld) | grep hamlib`.

## Troubleshooting

- **`nc` test errors / service won't start** — check `sudo systemctl status
  rigctld` and `journalctl -u rigctld`. Usual causes: radio off, wrong
  `-r` device, or `-s` baud not matching the radio's CI-V setting.
- **Permission denied on the serial port** — the `rigctld` user must be in
  `dialout` (the installer does this); a running service picks it up on restart.
- **Device name jumps around** (`ttyUSB0` vs `ttyUSB1`) — use a stable symlink:
  `ls -l /dev/serial/by-id/` and put that path in `-r`.

## Uninstall

```bash
sudo ./uninstall-rigctld.sh
```

Leaves the `rigctld` user and `libhamlib-utils` in place; the script prints how
to remove those too.
