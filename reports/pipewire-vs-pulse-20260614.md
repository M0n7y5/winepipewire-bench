# winepipewire.drv vs winepulse.drv - benchmark comparison

Date: 2026-06-14
Harness: `run.sh` (full harness, both arches) + `compare.sh`, this repo.

## Environment

| | |
|---|---|
| Host kernel | 7.0.11-1-cachyos (x86_64) |
| CPU | AMD Ryzen 9 9900X3D (12c / 24t) |
| Audio server | PipeWire 1.6.6 (PulseAudio compat via pipewire-pulse) |
| Default sink | `alsa_output.pci-0000_12_00.6.analog-stereo` (Realtek ALC897, onboard analog) |
| Wine tree | wine-cachyos `cachyos_11.0_staging/_pipewire` @ `bcaa8344ae8` |
| Build trees | `build` (x86_64) + `build32` (i386) |
| 32-bit audio libs | `lib32-libpipewire` + `lib32-libpulse` (both present) |
| Prefix | `~/.pwwow64` |
| Harness params | N=4 streams, STRESS_N=24, DUR=15 s, conformance on |
| Driver selection | `WINE_AUDIO_DRIVER` (no prefix registry mutation) |

Both runs were taken back-to-back on an otherwise idle machine (load ~1.2, no
game running). Each driver ran the full mmdevapi conformance gate (both arches)
plus the ten functional/perf probes.

## Headline

- **pipewire wins the latency metrics decisively.** Stream-open p50 533 us vs
  4260 us (~8x faster), open p99 1037 us vs 6152 us, and far tighter event-loop
  wakeup jitter (`evt_interval_sd_ms` 0.008 vs 0.498).
- **pipewire uses less daemon CPU** under 24 concurrent streams: 2.4% vs 7.3%
  for the audio daemons (pulse pays the pipewire-pulse compat-layer overhead),
  and slightly less client CPU (3.7% vs 4.3%).
- **pipewire offers a lower minimum period:** `engine_min_frames` 128 vs 256.
- **pulse is marginally lighter:** working set 26572 vs 27868 kB (~1.3 MB lower)
  and one fewer thread (6 vs 7; winepipewire runs one extra shared timer thread
  by design).
- **Functionally identical on both arches:** both drivers pass every conformance
  subtest on x86_64 *and* i386 (propstore's 6 failures are the expected
  wine-cachyos device-format todo flip, by design for both drivers), zero xruns,
  zero stream errors, monotonic clocks, identical tone/loopback fidelity (RMS
  0.1768, 440.2 Hz, tone ratio 1.000).
- `phase_sd_ms` (0.003 vs 0.007) nominally favors pulse, but both are ~100x under
  the 0.5 ms bound: measurement noise, not a real gap.

## 32-bit coverage

Both drivers are exercised on i386 as well as x86_64. The host needs both 32-bit
client libraries for this: `lib32-libpipewire` (already present) and
`lib32-libpulse` (installed for this run). After installing the latter,
`build32` was reconfigured (dropping the cached "no pulse") and its
`winepulse.so` rebuilt, so i386 pulse selects with priority Preferred and passes
the full suite (mmdevenum 72/0, render 4284/0, ...) exactly like x86_64. Without
`lib32-libpulse` the i386 pulse column is invalid ("No sound card available"),
which is purely an environment gap, not a driver defect.

## Full comparison

Reading the table: *better* says which direction wins for that metric
(`lower`/`higher`), `~ N` means closer to N wins, and `gate` rows are pass/fail
criteria asserted by `run.sh` against an expected count (see `BASELINES.md`), not
a race between drivers. *winner* applies the *better* rule to the two values;
`tie` when they are equal.

| metric | better | pipewire | pulse | delta% | winner |
|---|:---:|---:|---:|---:|:---:|
| conf_x86_64_mmdevenum_failures | gate | 0 | 0 | - | - |
| conf_x86_64_render_failures | gate | 0 | 0 | - | - |
| conf_x86_64_capture_failures | gate | 0 | 0 | - | - |
| conf_x86_64_dependency_failures | gate | 0 | 0 | - | - |
| conf_x86_64_propstore_failures | gate | 6 | 6 | +0.0% | - |
| conf_x86_64_spatialaudio_failures | gate | 0 | 0 | - | - |
| conf_i386_mmdevenum_failures | gate | 0 | 0 | - | - |
| conf_i386_render_failures | gate | 0 | 0 | - | - |
| conf_i386_capture_failures | gate | 0 | 0 | - | - |
| conf_i386_dependency_failures | gate | 0 | 0 | - | - |
| conf_i386_propstore_failures | gate | 6 | 6 | +0.0% | - |
| conf_i386_spatialaudio_failures | gate | 0 | 0 | - | - |
| phase_sd_ms | lower | 0.007 | 0.003 | -57.1% | pulse |
| lwp | lower | 7 | 6 | -14.3% | pulse |
| evt_interval_sd_ms | lower | 0.008 | 0.498 | +6125.0% | pipewire |
| evt_interval_p99_ms | lower | 10.015 | 10.702 | +6.9% | pipewire |
| drift_us | lower | 0.0 | 0.0 | - | tie |
| pos_nonmono | lower | 0 | 0 | - | tie |
| tone_errors | lower | 0 | 0 | - | tie |
| tone_rms | ~ 0.177 | 0.1768 | 0.1768 | +0.0% | tie |
| tone_freq_hz | ~ 440 | 440.2 | 440.2 | +0.0% | tie |
| open_p50_us | lower | 533 | 4260 | +699.2% | pipewire |
| open_p99_us | lower | 1037 | 6152 | +493.2% | pipewire |
| open_max_us | lower | 1579 | 6292 | +298.5% | pipewire |
| firstevt_p50_us | lower | 10064 | 10481 | +4.1% | pipewire |
| firstevt_p99_us | lower | 10096 | 11031 | +9.3% | pipewire |
| enum_p50_us | lower | 0 | 0 | - | tie |
| enum_p99_us | lower | 0 | 0 | - | tie |
| churn_errors | lower | 0 | 0 | - | tie |
| ws_warmup_kb | lower | 27776 | 26572 | -4.3% | pulse |
| ws_final_kb | lower | 27868 | 26572 | -4.7% | pulse |
| many_timeouts | lower | 0 | 0 | - | tie |
| many_errors | lower | 0 | 0 | - | tie |
| cpu_client_pct | lower | 3.7 | 4.3 | +16.2% | pipewire |
| cpu_pwdaemon_pct | lower | 2.4 | 7.3 | +204.2% | pipewire |
| xruns | lower | 0 | 0 | - | tie |
| ratechurn_errors | lower | 0 | 0 | - | tie |
| ratechurn_tail_freq_hz | ~ 440 | 440.0 | 440.0 | +0.0% | tie |
| loop_rms | ~ 0.177 | 0.1768 | 0.1768 | +0.0% | tie |
| loop_tone_ratio | higher | 1.000 | 1.000 | +0.0% | tie |
| loop_discont | lower | 0 | 0 | - | tie |
| loop_errors | lower | 0 | 0 | - | tie |
| engine_def_frames | - | 480 | 480 | +0.0% | - |
| engine_min_frames | - | 128 | 256 | +100.0% | - |
| surround_mix_channels | - | 8 | 8 | +0.0% | - |
| surround_mix_mask | - | 1599 | 1599 | +0.0% | - |
| surround_fmt51_supported | - | 1 | 1 | +0.0% | - |
| surround_render_errors | - | 0 | 0 | - | - |

## Reproduce

```sh
WINEPREFIX=~/.pwwow64 DRIVER=pipewire ./run.sh
WINEPREFIX=~/.pwwow64 DRIVER=pulse    ./run.sh
./compare.sh results/pipewire-latest.tsv results/pulse-latest.tsv
```
