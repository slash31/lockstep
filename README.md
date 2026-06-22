# lockstep

A native [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus) module that keeps
SDR++ and a radio's VFO **bidirectionally locked** through `rigctld` (Hamlib) —
a self-contained replacement for the `rigctld` ⇄ `rigsync` ⇄ SDR++ chain.

Radio-agnostic (rigctld drives any of Hamlib's ~250 rigs) and, because it speaks
to rigctld rather than the serial port directly, it **coexists with other CAT
software** (WSJT-X, a logger) sharing the one CI-V port — rigctld multiplexes.

## What it does

The SDR (fed by the buffer board's RF tap) is a second receiver on the antenna.
lockstep keeps it tuned identically to the radio, both ways:

```
radio ──RF tap──► buffer board ──► SDR ──► SDR++   (spectrum)
  ▲                                          │
  └── serial ─► rigctld ◄─TCP─ lockstep (inside SDR++)
                    ▲
                    └─TCP─ WSJT-X, logger, ...  (rigctld multiplexes the port)
```

| Direction      | Mechanism                                                          |
|----------------|-------------------------------------------------------------------|
| radio → SDR++  | `getFreq`/`getMode`; `tuner::tune(NORMAL, vfo, f)` + set radio module mode |
| SDR++ → radio  | poll the VFO's absolute freq/mode; `setFreq`/`setMode`             |
| anti-feedback  | pure dual-poll: shared last-known state, 1 Hz epsilon, no events   |

Both directions are **polled** on one worker thread and reconciled against a
shared last-known state, so whichever side moved since the last tick wins and no
feedback loop forms. The synced quantity is the **selected VFO's absolute
frequency** (`waterfall center + VFO offset`) — so clicking/dragging any signal
in the waterfall QSYs the radio there, not just moving the whole center.

**Mode sync** maps SDR++ radio modes ⇄ rigctl modes (NFM↔FM, WFM, AM, DSB, USB,
CW, LSB). It only runs when the selected VFO is a `radio` demodulator; toggle it
with the "Sync mode" checkbox. Passband/bandwidth is not synced.

## Why rigctld (not libhamlib-in-process)

lockstep talks to `rigctld` over TCP with its **own tiny rigctl client** (raw
POSIX sockets, in `src/main.cpp`). It deliberately does *not* use SDR++'s built-in
`net::rigctl::Client`: that class only gained `getMode`/`setMode` in recent master,
so a module linked against them fails to load (silently!) on any older installed
SDR++ — which is why the module wouldn't appear in the list at first. Speaking the
rigctl wire protocol ourselves makes lockstep loadable across SDR++ versions and
keeps full mode sync regardless.

## Build

Needs the SDR++ build deps (CMake ≥ 3.13, C++17, and core SDR++ deps —
Debian/Ubuntu: `build-essential cmake libfftw3-dev libglfw3-dev libvolk-dev
libzstd-dev`; see the SDR++ README for the rest). No libhamlib link needed for
the module itself; you only need Hamlib's `rigctld` binary at runtime.

```bash
./build.sh
# or point at an existing checkout:
SDRPP_DIR=/path/to/SDRPlusPlus ./build.sh
```

The script clones SDR++ (shallow) beside this repo if needed, symlinks this module
into `misc_modules/lockstep`, registers it in the root `CMakeLists.txt`, builds the
`lockstep` target (vendor-SDK source modules disabled so it builds without
libairspy/hackrf/etc.), and drops `lockstep.so` next to this README.

> **ABI note:** SDR++ has no stable module ABI. Loading requires the core to
> provide every symbol the module references — verify with:
> ```
> nm -D -u lockstep.so | awk '{print $2}' > need.txt
> nm -D --defined-only /usr/lib/libsdrpp_core.so | awk '{print $3}' > have.txt
> comm -23 <(sort -u need.txt) <(sort -u have.txt)   # must be empty
> ```
> A clean symbol check means it will *load*. There's still a smaller residual
> risk: lockstep reads `gui::waterfall.selectedVFO` as a direct struct member, so
> if the `WaterFall` layout changed between your installed build and the headers
> this was compiled against, that field could read wrong. If it loads but
> frequency/VFO tracking misbehaves, rebuild against your install's matching
> SDR++ commit (`SDRPP_DIR=...`).

## Use

1. Have `rigctld` running against the radio. Easiest is the systemd service in
   [`rigctld/`](rigctld/) — `cd rigctld && sudo ./setup-rigctld.sh` (installs it,
   survives reboots, and lets WSJT-X share the port). Or run it ad-hoc:
   ```bash
   rigctld -m 3070 -r /dev/ttyUSB0 -s 19200   # -m 3070 = IC-7100; find others: rigctl -l
   ```
   Verify: `echo "f" | nc -q1 localhost 4532` prints the dial frequency.
2. (Optional) Point WSJT-X / your logger at the same rigctld — Rig =
   "Hamlib NET rigctl", address `localhost:4532`. They share the port.
3. In SDR++: select the SDR fed by the buffer board, start it, and add + select a
   **radio** demodulator VFO (that's the VFO lockstep tracks and mode-syncs).
4. Add **lockstep** from the Module Manager and set Host `127.0.0.1`, Port `4532`,
   Poll `250` → **Start**. Status reads **Locked**. Options:
   - **Tune radio from SDR++** (default on) — bidirectional. Turn it **off** for
     *follow-only*: lockstep only reads the radio and never writes to it, so a rig
     parked on a **memory channel stays in memory mode** (any freq/mode write
     would force an Icom into VFO mode). Use follow-only when you just want the
     panadapter to track a memory channel.
   - **Sync mode** — also keep USB/LSB/CW/FM in step (needs a `radio` VFO).
5. Turn the radio → the SDR++ VFO follows; with "Tune radio from SDR++" on, click a
   signal → the radio QSYs there, and changing mode either end syncs the other.

At startup lockstep always adopts the **radio's** current freq/mode (the radio
wins), and it never pushes a frequency back to the rig in the same poll as it
pulled one — so it won't bump the rig out of memory mode on connect.

## Status / scope of v1

- Frequency **and** mode, both directions, following the selected VFO.
- Mode **passband/bandwidth is not synced**, only the mode itself.
- No TX guard yet; lockstep will follow/retune while you're keyed.
- Single VFO that tracks `gui::waterfall.selectedVFO`; multi-VFO isn't handled.

## Files

- `src/main.cpp` — the module (rigctl client, dual-poll sync).
- `CMakeLists.txt` — SDR++ module build.
- `build.sh` — clone/symlink/register/build helper.
- `rigctld/` — systemd service + installer for the rigctld pre-work (run first).
- `reference/` — upstream SDR++ / gerner source pulled for API reference (not built).
