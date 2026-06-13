#!/usr/bin/env bash
# Build the winepipewire benchmark probes as PE (x86_64-windows) executables.
#
# The probes are ordinary WASAPI clients; they reach winepipewire.drv at
# runtime via the registry (see run.sh), so they do NOT need rebuilding when
# the driver changes; rebuild only when a probe's own source changes.
#
# Env:
#   WINE_BUILD  wine build directory holding tools/winegcc (required; export it or set it in local.env)
#   ARCH        winegcc target arch (default: x86_64-windows; use i386-windows for the WoW64 path)
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -z "${WINE_BUILD:-}" ] && [ -f "$BENCH_DIR/local.env" ]; then
    . "$BENCH_DIR/local.env"
fi
WINE_BUILD="${WINE_BUILD:-}"
ARCH="${ARCH:-x86_64-windows}"
OUT="$BENCH_DIR/bin"

if [ -z "$WINE_BUILD" ]; then
    echo "error: WINE_BUILD is not set; export it or put it in local.env (see README)" >&2
    exit 2
fi
if [ ! -x "$WINE_BUILD/tools/winegcc/winegcc" ]; then
    echo "error: winegcc not found at $WINE_BUILD/tools/winegcc/winegcc" >&2
    echo "       set WINE_BUILD to your wine build directory (got: $WINE_BUILD)" >&2
    exit 2
fi

mkdir -p "$OUT"
cd "$WINE_BUILD"
for probe in wpw_phase wpw_multi wpw_tone wpw_open wpw_stress wpw_loopcap wpw_miccap wpw_spatial wpw_spatial_bed wpw_caps; do
    echo "building $probe ($ARCH) ..."
    tools/winegcc/winegcc -o "$OUT/$probe.exe" --wine-objdir . \
        -b "$ARCH" -I../include -Iinclude -I../include/msvcrt -D_MSVCR_VER=0 \
        "$BENCH_DIR/probes/$probe.c" -lole32 -luuid -lkernel32 -lntdll -lmsvcrt -lpsapi
done
echo "done -> $OUT"
