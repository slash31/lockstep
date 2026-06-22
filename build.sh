#!/usr/bin/env bash
# Build the lockstep module against an SDR++ source tree.
#
# It will clone SDR++ next to this repo (if not already present), symlink the
# lockstep module into misc_modules/, register it in the root CMakeLists, and
# build just the lockstep target. The resulting .so is copied next to this
# script for easy install.
#
# Override the SDR++ location with:  SDRPP_DIR=/path/to/SDRPlusPlus ./build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDRPP_DIR="${SDRPP_DIR:-$(cd "$HERE/.." && pwd)/SDRPlusPlus}"
SDRPP_REPO="https://github.com/AlexandreRouma/SDRPlusPlus.git"

echo ">> SDR++ tree: $SDRPP_DIR"
if [ ! -d "$SDRPP_DIR/.git" ]; then
    echo ">> Cloning SDR++..."
    git clone --depth 1 "$SDRPP_REPO" "$SDRPP_DIR"
fi

# Symlink the module into the SDR++ source tree (kept in sync with this repo).
ln -sfn "$HERE" "$SDRPP_DIR/misc_modules/lockstep"

# Register the module in the root CMakeLists (idempotent).
ROOT_CMAKE="$SDRPP_DIR/CMakeLists.txt"
if ! grep -q "OPT_BUILD_LOCKSTEP" "$ROOT_CMAKE"; then
    echo ">> Registering lockstep in $ROOT_CMAKE"
    cat >> "$ROOT_CMAKE" <<'EOF'

# --- lockstep (bidirectional rigctl panadapter sync) ---
option(OPT_BUILD_LOCKSTEP "Build the lockstep rigctl sync module" ON)
if (OPT_BUILD_LOCKSTEP)
add_subdirectory("misc_modules/lockstep")
endif (OPT_BUILD_LOCKSTEP)
EOF
fi

# Configure + build only what's needed for our module. lockstep depends only on
# SDR++ core, so disable the source/sink modules that pull in vendor SDKs (so the
# build doesn't fail on a missing libairspy/libhackrf/rtaudio/etc.). The radio
# decoder is header-only for us (we just include radio_interface.h).
BUILD_DIR="$SDRPP_DIR/build"
cmake -B "$BUILD_DIR" -S "$SDRPP_DIR" -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_LOCKSTEP=ON \
    -DOPT_BUILD_AIRSPY_SOURCE=OFF \
    -DOPT_BUILD_AIRSPYHF_SOURCE=OFF \
    -DOPT_BUILD_AUDIO_SOURCE=OFF \
    -DOPT_BUILD_HACKRF_SOURCE=OFF \
    -DOPT_BUILD_PLUTOSDR_SOURCE=OFF \
    -DOPT_BUILD_RTL_SDR_SOURCE=OFF \
    -DOPT_BUILD_AUDIO_SINK=OFF
cmake --build "$BUILD_DIR" --target lockstep -j"$(nproc)"

# Locate and stage the built module.
SO="$(find "$BUILD_DIR" -name 'lockstep.so' -o -name 'lockstep.dll' -o -name 'liblockstep.so' 2>/dev/null | head -n1 || true)"
if [ -n "$SO" ]; then
    cp -f "$SO" "$HERE/"
    echo ">> Built: $SO"
    echo ">> Copied to: $HERE/$(basename "$SO")"
    echo ">> Install: copy it into your SDR++ modules dir, then add it via"
    echo "   Module Manager in SDR++ (type 'lockstep', module 'lockstep')."
else
    echo "!! Build finished but lockstep.so was not found under $BUILD_DIR" >&2
    exit 1
fi
