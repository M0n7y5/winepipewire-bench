# winepipewire-bench

Out-of-tree benchmark + functional harness for **`winepipewire.drv`**, Wine's
native PipeWire `mmdevapi` audio backend.

It deliberately lives outside the Wine tree. Wine's own test framework
(`dlls/mmdevapi/tests/`) is pure pass/fail API-contract conformance that must
run on real Windows too, so it cannot express the things this harness measures:
inter-stream timing jitter, thread counts, clock drift, open/enumeration
latency, stress behaviour, and output signal fidelity - all Linux/PipeWire-
specific quality-of-implementation metrics. Those belong here; correctness
regressions belong in the Wine suite. This harness also runs the Wine
conformance subtests as a gate, so one command covers both.

The same harness runs against **`winepulse.drv`** (`DRIVER=pulse`) for a
side-by-side driver comparison.

## Layout

```
probes/   wpw_phase.c    sub-period event-phase jitter + LWP count (the sync metric)
          wpw_multi.c    inter-stream IAudioClock GetPosition drift + monotonicity
          wpw_tone.c     single-stream 440 Hz playback (functional + signal fidelity)
          wpw_open.c     stream-open / first-event / enumeration latency percentiles
          wpw_stress.c   churn (open/close cycles + leak check), many (N concurrent
                         streams), ratechurn (IAudioClockAdjustment::SetSampleRate)
          wpw_loopcap.c  WASAPI loopback capture integrity (tone round-trip)
          wpw_caps.c     IAudioClient3 engine-period caps + 7.1 surround mix format
build.sh                 cross-compiles the probes to PE via the Wine tree's winegcc
run.sh                   selects the driver, runs everything, prints a PASS/FAIL
                         report, and appends every metric to results/<driver>-*.tsv
compare.sh               A/B markdown table from two TSVs + baseline regression gate
BASELINES.md             reference numbers + thresholds run.sh asserts
baselines/pipewire.tsv   machine baseline consumed by compare.sh --check
results/                 per-run metric TSVs (generated, gitignored)
bin/                     built probe .exe files (generated, gitignored)
```

The probes are plain WASAPI clients; they reach the driver at runtime through
the registry (`HKCU\Software\Wine\Drivers`, value `Audio`). `run.sh` saves the
pre-run value and restores it on exit (including Ctrl-C). Rebuild only when a
probe's own source changes, **not** when the driver changes.

## Prerequisites

- A Wine build tree with `tools/winegcc` and `clang` as the PE cross-compiler
  (the same tree that builds the driver).
- A running PipeWire daemon (`/run/user/$UID/pipewire-0`).
- A Wine prefix with `winepipewire.drv` (and `winepulse.drv` for comparisons).
- For the signal-fidelity checks: `parecord` (pipewire-pulse) and `python3` +
  `numpy`. Without them, `run.sh` still checks playback/loopback for zero
  WASAPI errors and skips only the monitor RMS/FFT analyses.
- Optional: `pw-top` for the xrun count during the many-streams stress.

## Usage

```sh
# 0. point the harness at your wine build tree once (local.env is gitignored)
echo 'WINE_BUILD=$HOME/path/to/wine-cachyos/build' > local.env
# 1. build the probes
./build.sh
# optional: WoW64 / 32-bit probes
ARCH=i386-windows ./build.sh

# 2. run the full harness against each driver
WINEPREFIX=~/.pwwow64 DRIVER=pipewire ./run.sh
WINEPREFIX=~/.pwwow64 DRIVER=pulse    ./run.sh

# 3. side-by-side comparison (the presentation deliverable)
./compare.sh results/pipewire-latest.tsv results/pulse-latest.tsv > report.md

# 4. regression gate against the checked-in baseline
./compare.sh --check baselines/pipewire.tsv results/pipewire-latest.tsv
# tighter/looser: --max-regress <pct>   (default 25)
```

`run.sh` tunables: `N=<streams>` `STRESS_N=<streams>` `DUR=<seconds>`
`RUN_CONFORMANCE=0` `PERF_GATE=0|1`. It exits `0` if every check passes, `1`
if any check fails, `2` on bad usage, and `77` (skip) if a prerequisite is
missing.

`DRIVER=pulse` runs are an informational reference: the perf thresholds
(phase sd, LWP, drift, open-latency bounds) print as INFO instead of gating
(winepulse has a different threading model) while every functional gate
(conformance, tone/loopback fidelity, churn/many/ratechurn, clock
monotonicity) is still enforced. Force gating with `PERF_GATE=1`.

The capture-based checks (tone, ratechurn, loopcap) play audible sound on the
default sink.

## Running a probe by hand

```sh
WINE=~/path/to/wine-cachyos/build/loader/wine
WINEPREFIX=~/.pwwow64 $WINE bin/wpw_phase.exe 4 20
WINEPREFIX=~/.pwwow64 $WINE bin/wpw_stress.exe ratechurn 8
WINEPREFIX=~/.pwwow64 $WINE bin/wpw_loopcap.exe 4 440
WINEPREFIX=~/.pwwow64 $WINE bin/wpw_caps.exe WPW71SURROUND   # name needs the 7.1 null sink loaded
```

## Latency knobs

libpipewire applies its environment knobs at `pw_stream_connect()` *after*
merging the stream's own properties, so they override the driver's
`node.latency` (one graph period per WASAPI period) per process:

```sh
PIPEWIRE_LATENCY=128/48000 wine game.exe   # force a 128-frame graph quantum
PIPEWIRE_QUANTUM=128/48000                 # same, plus pins the graph rate
PIPEWIRE_NODE=<object.serial|name>         # route to a specific sink
PIPEWIRE_PROPS='{ media.role = "Game" }'   # arbitrary stream properties
```

On the WASAPI side, winepipewire derives the IAudioClient3 shared-mode
minimum period from the graph's `clock.min-quantum` (clamped to 128 frames,
the typical Windows floor; never above the previous 3 ms value). Verified by
the `wpw_caps` gate: `engine_min_frames=128`, where winepulse reports
whatever the PA server's buffer attrs negotiate (256 under pipewire-pulse).

## License

LGPL-2.1-or-later, matching `winepipewire.drv`. The probes contain no PipeASIO
(GPL-3.0) code.
