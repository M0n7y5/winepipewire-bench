# Baselines

Reference numbers for `winepipewire.drv`, measured on a live PipeWire daemon,
default render endpoint, shared-mode float32 / 2ch / 48000 Hz, default period
(10 ms). `run.sh` asserts the post-sync column.

Hardware/context of record: AMD Ryzen 9 9900X3D, PipeWire 1.6.6 over ALSA
`analog-stereo`, **wine-cachyos** build (`--enable-archs=i386,x86_64
--with-pipewire`), clang PE cross-compiler.

## The change being guarded

Commit *"winepipewire.drv: Synchronize streams sharing a device and period."*
replaced the per-stream Wine timer threads with **one timer thread per
`(device, period)` group**, servicing and signalling every stream in the group
in a single tick. The probes below quantify that.

| Metric (N=4 streams) | Pre-sync (per-stream timers) | Post-sync (shared group timer) | `run.sh` threshold |
|---|---|---|---|
| `wpw_phase` inter-stream event-phase **sd** | 1.7-2.1 ms | **0.003-0.009 ms** | sd < 0.5 ms |
| `wpw_phase` event-phase **range** | +/-5 ms (half period) | +/-0.05 ms | (covered by sd) |
| process **LWP count** | 10 (4 per-stream timers) | **7** (1 shared timer) | LWP == N+3 |
| `wpw_multi` MAX inter-stream **drift** | bounded <= 1 period (rate-locked) | well under 1 period | drift < 11000 us |
| `wpw_tone` WASAPI errors | 0 | **0** | == 0 |
| `wpw_tone` monitor **RMS** (0.25-amp sine) | 0.177 | **0.177** (0.25/sqrt(2)) | 0.10 <= RMS <= 0.25 |
| `wpw_tone` dominant **freq** | 440 Hz | **440 Hz** (FFT bin) | 430 <= f <= 450 |

LWP count = 1 PipeWire global loop + 1 process main + N probe workers + **1**
shared timer = `N + 3`. A regression to per-stream timers makes it
`2N + 2` (= 10 at N=4), which fails the `== N+3` check.

## Review-fix guards (functional gates, enforced for both drivers)

These sections of `run.sh` exercise the driver paths fixed in the
winepipewire review series end-to-end:

- **`wpw_loopcap`** guards *"Use the loopback flag to request sink
  capture."*: a loopback stream that records the microphone instead of the
  sink monitor fails the `tone_ratio >= 0.5` Goertzel gate (reference run:
  `tone_ratio = 1.000`, `discont = 0`).
- **`wpw_stress ratechurn`** guards *"Check the rate control result and fix
  repeated SetSampleRate calls."*: after 12 alternating
  `SetSampleRate(rate/2)` / `SetSampleRate(rate)` calls the captured tail must
  sit at ~440 Hz. With the old compounding-ratio bug the ratio is computed
  against the mutated rate, so the tail reads ~880 Hz (or sticks at 220 Hz).
  `E_NOTIMPL` (PipeWire < 1.2.6 / no adaptive resampler) downgrades the check
  to an INFO skip (probe exit 3).
- **`wpw_stress churn`**: 300 open/close cycles; `errors == 0` and working
  set growth <= 4096 kB (leak net; reference: ~100 kB growth).
- **`wpw_stress many`**: `STRESS_N` (default 24) concurrent event-driven
  streams for 10 s; `timeouts == 0 && errors == 0`. CPU% (client + PipeWire
  daemons) and pw-top xruns are recorded as informational metrics.
- **`wpw_multi` `pos_nonmono`**: IAudioClock::GetPosition must never go
  backwards (`== 0`, both drivers; a backwards clock is a correctness bug).
- **`evt_interval_sd_ms` / `evt_interval_p99_ms`** guards *"Extrapolate the
  graph clock when timing the period grid."*: the period timer compares its
  grid against an extrapolated graph clock; regressing to the raw
  `pw_time.now` cycle-start staircase smears wakeup intervals across
  period +/- period/2 whenever the graph quantum does not divide the period
  (sd jumps from the 0.01-0.13 ms idle noise band to ~2.1 ms at quantum
  256, caught by `compare.sh --check`).

## Conformance (Wine `mmdevapi` suite, the correctness net)

`dlls/mmdevapi/tests/` on wine-cachyos, both arches. Only **failure counts**
are asserted (executed-test counts in capture/propstore are dynamic; they
depend on enumerated devices/formats):

| subtest | x86_64 executed | i386 executed | expected failures |
|---|---|---|---|
| mmdevenum | 72 | 72 | 0 |
| render | 4284 (1 skipped) | 4284 (1 skipped) | 0 |
| capture | ~6045-6052 (dynamic) | ~6045-6052 (dynamic) | 0 |
| dependency | 11 | 11 | 0 |
| propstore | ~111-114 (dynamic) | ~297-300 (dynamic) | **6** |
| spatialaudio | 443 | 443 | 0 |

**propstore = 6 expected failures, by design, for both drivers**: on
wine-cachyos both `winepipewire.drv` and `winepulse.drv` report the probed
device format (16-bit PCM, matching real Windows) for
`PKEY_AudioEngine_DeviceFormat`, which flips upstream's `todo_wine` at
`dlls/mmdevapi/tests/propstore.c:85-86` into "Test succeeded inside todo
block": 2 lines x 3 devices = 6. Removing the upstream `todo_wine` is a
fork-policy decision tracked separately; until then the harness expects
exactly 6.

`render 4284/0` is the load-bearing case: it exercises GetPosition / padding
timing and confirms dropping the old `just_started`/`just_underran` resync in
favour of the group timer's grid-(re)acquire deferral is behaviourally exact.

## Machine baseline (`baselines/pipewire.tsv`)

Every `run.sh` invocation appends all metrics to
`results/<driver>-<timestamp>.tsv` (copied to `results/<driver>-latest.tsv`).
`baselines/pipewire.tsv` is the checked-in reference consumed by:

```sh
./compare.sh --check baselines/pipewire.tsv results/pipewire-latest.tsv
```

which fails (exit 1) when a lower-is-better metric (`phase_sd_ms`,
`evt_interval_*`, `drift_us`, `open_*`/`firstevt_*`/`enum_*` latencies,
`cpu_*_pct`, `xruns`, `ws_final_kb`) regresses by more than `--max-regress`
(default 25 %) beyond a per-metric absolute slack. The static thresholds in
`run.sh` are deliberately generous pathology nets; fine-grained regression
detection is `compare.sh --check`'s job. If latencies drift close to the
static bounds, raise the bounds, not the baseline, and investigate.

### Re-baselining

After a deliberate, understood performance change:

```sh
DRIVER=pipewire ./run.sh                      # must exit 0
cp results/pipewire-latest.tsv baselines/pipewire.tsv
git add baselines/pipewire.tsv && git commit  # together with the explanation
```

### Reference comparison (winepipewire vs winepulse, this machine)

From `compare.sh` on the recorded baselines, the headline numbers:

| metric | pipewire | pulse |
|---|---:|---:|
| stream open p50 | 722 us | 4679 us |
| stream open p99 | 2021 us | 6074 us |
| CPU, PipeWire daemons (24 streams) | 1.4 % | 6.3 % |
| CPU, client process (24 streams) | 4.2 % | 5.0 % |
| event-interval jitter sd (worst stream) | 0.068 ms | 0.51 ms |
| event-interval p99 | 10.05 ms | 10.72 ms |
| working set after churn | ~27.8 MB | ~26.5 MB |
| client threads (LWP, N=4 streams) | 7 | 6 |
| conformance | identical | identical |

Known, explained deltas where winepulse measures better:

- **Working set (+~1.3 MB)**: resident library footprint, not a leak;
  libpipewire-0.3 + SPA plugins map ~2.6 MB RSS in the client (format
  conversion/resampling run in-process), libpulse only ~0.8 MB because
  pipewire-pulse does that work in the daemon. The flip side is the daemon
  CPU row above (1.4 % vs 6.3 %): total CPU for 24 streams is ~5.6 % vs
  ~11.3 % in winepipewire's favour.
- **LWP (+1 thread)**: winepipewire runs its per-(device, period) group timer
  on a dedicated thread, while winepulse schedules the equivalent group timer
  as a pa_time_event on its existing mainloop thread. One thread per period
  group (not per stream) is the documented design; folding it onto the
  PipeWire main loop via pw_loop_add_timer would save it, at the cost of
  sharing the control loop with every other stream's callbacks.
