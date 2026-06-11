#!/usr/bin/env bash
# winepipewire.drv benchmark + functional harness.
#
# Selects the requested mmdevapi driver via the registry (saved and restored
# on exit), then runs:
#   1. (gate) the Wine mmdevapi conformance subtests (all 6, both arches)
#   2. wpw_phase   : inter-stream event-phase jitter + LWP    -> the sync metric
#   3. wpw_multi   : inter-stream IAudioClock drift + monotonicity
#   4. wpw_tone    : single-stream playback + monitor RMS/FFT -> signal fidelity
#   5. wpw_open    : stream-open / first-event / enumeration latency
#   6. wpw_stress churn     : open/close cycling + working-set leak check
#   7. wpw_stress many      : N concurrent streams + CPU/xrun sampling
#   8. wpw_stress ratechurn : IAudioClockAdjustment SetSampleRate toggling
#   9. wpw_loopcap : WASAPI loopback capture integrity (tone round-trip)
# and prints a one-page report with PASS/FAIL against BASELINES.md thresholds.
# Every measurement is also appended to results/<driver>-<timestamp>.tsv
# (copied to results/<driver>-latest.tsv) for compare.sh.
#
# Exit: 0 all PASS, 1 a check FAILED, 2 bad usage, 77 prerequisites missing.
#
# Env (all optional):
#   WINEPREFIX   wine prefix             (default: ~/.pwwow64)
#   WINE_BUILD   wine build dir          (default: ~/path/to/wine-cachyos/build)
#   DRIVER       pipewire | pulse        (default: pipewire)
#   PERF_GATE    1=enforce perf bounds   (default: 1 for pipewire, 0 for pulse)
#   N            streams for phase/multi (default: 4)
#   STRESS_N     streams for stress many (default: 24)
#   DUR          measurement seconds     (default: 15)
#   RUN_CONFORMANCE  1=run mmdevapi gate  (default: 1)
set -uo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$BENCH_DIR/bin"
WINEPREFIX="${WINEPREFIX:-$HOME/.pwwow64}"
WINE_BUILD="${WINE_BUILD:-$HOME/path/to/wine-cachyos/build}"
DRIVER="${DRIVER:-pipewire}"
case "$DRIVER" in
    pipewire|pulse) ;;
    *) echo "bad DRIVER '$DRIVER' (want pipewire or pulse)"; exit 2 ;;
esac
if [ "$DRIVER" = pipewire ]; then PERF_GATE="${PERF_GATE:-1}"; else PERF_GATE="${PERF_GATE:-0}"; fi
N="${N:-4}"
STRESS_N="${STRESS_N:-24}"
DUR="${DUR:-15}"
RUN_CONFORMANCE="${RUN_CONFORMANCE:-1}"

WINE="$WINE_BUILD/loader/wine"

# thresholds (see BASELINES.md)
PHASE_SD_MAX_MS=0.5      # post-sync ~0.004 ms; per-stream-timer regression ~1.8 ms
EXPECT_LWP=$((N + 3))    # 1 pw loop + 1 main + N workers + 1 shared timer
DRIFT_MAX_US=11000       # < ~1 period (10 ms)
RMS_MIN=0.10             # 0.25-amplitude sine -> 0.177 at the monitor
RMS_MAX=0.25
FREQ_LO=430
FREQ_HI=450
OPEN_P99_MAX_US=100000   # pathology nets only; fine-grained regression = compare.sh
FIRSTEVT_P99_MAX_US=50000
ENUM_P99_MAX_US=200000
WS_GROWTH_MAX_KB=4096    # churn leak detector: 4 MB slack over 300 cycles

export WINEPREFIX WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
FAILS=0
declare -a REPORT

note()  { REPORT+=("$1"); }
pass()  { echo "  PASS: $1"; REPORT+=("PASS  $1"); }
fail()  { echo "  FAIL: $1"; REPORT+=("FAIL  $1"); FAILS=$((FAILS + 1)); }
flt_lt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a+0 < b+0)}'; }
# perf-gated check: enforced only when PERF_GATE=1, otherwise informational
perf_check() { # $1=condition-result(0 ok) $2=pass text $3=fail/info text
    if [ "$PERF_GATE" = 1 ]; then
        if [ "$1" = 0 ]; then pass "$2"; else fail "$3"; fi
    else
        note "INFO  $3 (not gated, DRIVER=$DRIVER)"
    fi
}
# token extraction from "key=value" probe output: tok "<text>" <key>
tok() { echo "$1" | grep -oE "$2=-?[0-9.]+" | head -1 | cut -d= -f2; }

# ---- prerequisites -------------------------------------------------------
[ -x "$WINE" ]      || { echo "SKIP: wine loader not found at $WINE"; exit 77; }
for p in wpw_phase wpw_multi wpw_tone wpw_open wpw_stress wpw_loopcap; do
    [ -x "$BIN/$p.exe" ] || { echo "SKIP: $BIN/$p.exe missing; run build.sh first"; exit 77; }
done
SOCK="/run/user/$(id -u)/pipewire-0"
[ -S "$SOCK" ] || { echo "SKIP: no PipeWire daemon at $SOCK"; exit 77; }

echo "== wine mmdevapi driver benchmark =="
echo "prefix=$WINEPREFIX  driver=$DRIVER  perf_gate=$PERF_GATE  N=$N  stress_n=$STRESS_N  dur=${DUR}s"

# ---- driver selection (saved + restored on exit) -------------------------
# reg query emits CRLF; strip the CR or restore_audio writes "driver\r" back,
# which mmdevapi cannot match to any driver (kills audio in the prefix)
PREV_AUDIO=$("$WINE" reg query 'HKCU\Software\Wine\Drivers' /v Audio 2>/dev/null | awk '/REG_SZ/{print $NF}' | tr -d '\r')
restore_audio() {
    if [ -n "$PREV_AUDIO" ]; then
        "$WINE" reg add 'HKCU\Software\Wine\Drivers' /v Audio /d "$PREV_AUDIO" /f >/dev/null 2>&1
    else
        "$WINE" reg delete 'HKCU\Software\Wine\Drivers' /v Audio /f >/dev/null 2>&1
    fi
}
trap restore_audio EXIT
trap 'exit 130' INT TERM   # route signals through EXIT so restore_audio runs
"$WINE" reg add 'HKCU\Software\Wine\Drivers' /v Audio /d "$DRIVER" /f >/dev/null 2>&1
drv=$("$WINE" reg query 'HKCU\Software\Wine\Drivers' /v Audio 2>/dev/null | grep -oiE "$DRIVER" | head -1)
[ "$drv" = "$DRIVER" ] && echo "driver = $DRIVER" || { echo "SKIP: could not select $DRIVER driver"; exit 77; }

# ---- metrics TSV ----------------------------------------------------------
mkdir -p "$BENCH_DIR/results"
TSV="$BENCH_DIR/results/${DRIVER}-$(date +%Y%m%d-%H%M%S).tsv"
{ echo "# driver=$DRIVER date=$(date -Is) host=$(uname -r)"
  echo "# wine=$(git -C "$WINE_BUILD/.." rev-parse --short HEAD 2>/dev/null || echo unknown)"
  echo "# pipewire=$(pactl info 2>/dev/null | sed -n 's/.*on PipeWire \([0-9.]*\).*/\1/p')"
} > "$TSV"
metric() { printf '%s\t%s\n' "$1" "$2" >> "$TSV"; }

# ---- 1. conformance gate -------------------------------------------------
# Expected failure counts per driver.  On wine-cachyos BOTH winepipewire and
# winepulse report the probed device format (16-bit PCM, matching real
# Windows) for PKEY_AudioEngine_DeviceFormat, which flips upstream's
# todo_wine at dlls/mmdevapi/tests/propstore.c:85-86: exactly 6
# "Test succeeded inside todo block" failures (2 lines x 3 devices), by design.
expected_failures() { # $1=subtest
    if [ "$1" = propstore ]; then echo 6; else echo 0; fi
}
if [ "$RUN_CONFORMANCE" = 1 ]; then
    echo; echo "-- mmdevapi conformance gate --"
    for arch in x86_64 i386; do
        exe="$WINE_BUILD/dlls/mmdevapi/tests/$arch-windows/mmdevapi_test.exe"
        [ -x "$exe" ] || { note "INFO  conformance $arch skipped (no $exe)"; continue; }
        for t in mmdevenum render capture dependency propstore spatialaudio; do
            line=$("$WINE" "$exe" "$t" 2>&1 | grep -E "tests executed" | tail -1)
            nf=$(echo "$line" | grep -oE '[0-9]+ failures' | grep -oE '[0-9]+' | head -1)
            exp=$(expected_failures "$t")
            echo "  $arch/$t: ${line#*: }"
            metric "conf_${arch}_${t}_failures" "${nf:-999}"
            if [ "${nf:-999}" = "$exp" ]; then pass "conformance $arch/$t ($nf failures, expected $exp)"
            else fail "conformance $arch/$t (${nf:-?} failures, expected $exp)"; fi
        done
    done
else
    echo; echo "-- conformance gate skipped (RUN_CONFORMANCE=$RUN_CONFORMANCE) --"
fi

# ---- 2. phase + thread count --------------------------------------------
echo; echo "-- wpw_phase (event-phase jitter + LWP) --"
OUT="$(mktemp)"
"$WINE" "$BIN/wpw_phase.exe" "$N" "$DUR" >"$OUT" 2>/dev/null &
BG=$!
sleep $(( DUR / 2 + 1 ))
MAXLWP=0
for pid in $(pgrep -f 'wpw_phase.exe'); do
    n=$(ps -o nlwp= -p "$pid" 2>/dev/null | tr -d ' ')
    [ -n "$n" ] && [ "$n" -gt "$MAXLWP" ] && MAXLWP=$n
done
wait $BG
grep -E "vs stream0 phase" "$OUT" | sed 's/^/    /'
worst=0
while read -r sd; do flt_lt "$worst" "$sd" && worst=$sd; done < <(grep 'vs stream0 phase' "$OUT" | grep -oE 'sd=[0-9.]+' | cut -d= -f2)
echo "    worst sd = ${worst} ms ; LWP = ${MAXLWP} (expect ${EXPECT_LWP})"
metric phase_sd_ms "$worst"
metric lwp "$MAXLWP"
if [ -n "$worst" ] && flt_lt "$worst" "$PHASE_SD_MAX_MS"; then c=0; else c=1; fi
perf_check "$c" "phase sd ${worst}ms < ${PHASE_SD_MAX_MS}ms" "phase sd ${worst}ms vs bound ${PHASE_SD_MAX_MS}ms"
if [ "$MAXLWP" = "$EXPECT_LWP" ]; then c=0; else c=1; fi
perf_check "$c" "thread count $MAXLWP == $EXPECT_LWP (one shared timer)" "thread count $MAXLWP vs expected $EXPECT_LWP"
ei=$(grep 'evt_interval worst' "$OUT" || true)
if [ -n "$ei" ]; then
    echo "    ${ei}"
    metric evt_interval_sd_ms "$(tok "$ei" sd)"
    metric evt_interval_p99_ms "$(tok "$ei" p99)"
fi
rm -f "$OUT"

# ---- 3. drift + clock monotonicity ----------------------------------------
echo; echo "-- wpw_multi (inter-stream IAudioClock drift) --"
OUT="$(mktemp)"
"$WINE" "$BIN/wpw_multi.exe" "$N" "$DUR" >"$OUT" 2>/dev/null
drift=$(grep -oE 'MAX inter-stream drift over run = [0-9.]+' "$OUT" | grep -oE '[0-9.]+$')
nonmono=$(grep -oE 'pos_nonmono=[0-9]+' "$OUT" | cut -d= -f2)
echo "    MAX inter-stream drift = ${drift:-?} us (expect < ${DRIFT_MAX_US}) ; pos_nonmono = ${nonmono:-?}"
metric drift_us "${drift:-}"
metric pos_nonmono "${nonmono:-999}"
if [ -n "$drift" ] && flt_lt "$drift" "$DRIFT_MAX_US"; then c=0; else c=1; fi
perf_check "$c" "drift ${drift}us < ${DRIFT_MAX_US}us" "drift ${drift:-?}us vs bound ${DRIFT_MAX_US}us"
if [ "${nonmono:-999}" = 0 ]; then pass "IAudioClock positions monotonic (pos_nonmono=0)"
else fail "IAudioClock position went backwards (pos_nonmono=${nonmono:-?})"; fi
rm -f "$OUT"

# ---- 4. tone + monitor signal -------------------------------------------
echo; echo "-- wpw_tone (playback + monitor RMS/FFT) --"
CAP="$(mktemp --suffix=.raw)"
PAREC=""
if command -v parecord >/dev/null 2>&1; then
    parecord -d @DEFAULT_MONITOR@ --format=float32le --rate=48000 --channels=2 --latency-msec=100 --raw "$CAP" >/dev/null 2>&1 &
    PAREC=$!
    sleep 0.6
fi
tone_out=$("$WINE" "$BIN/wpw_tone.exe" 3 440 2>/dev/null)
tone_rc=$?
echo "    ${tone_out}"
[ -n "$PAREC" ] && { sleep 0.3; kill "$PAREC" 2>/dev/null; wait "$PAREC" 2>/dev/null; }
metric tone_errors "$(tok "$tone_out" errors)"
if [ "$tone_rc" = 0 ]; then pass "tone playback (0 WASAPI errors)"; else fail "tone playback (rc=$tone_rc, WASAPI errors)"; fi

if [ -n "$PAREC" ] && [ -s "$CAP" ] && python3 -c 'import numpy' >/dev/null 2>&1; then
    read -r rms freq < <(python3 - "$CAP" <<'PY'
import sys, numpy as np
x = np.fromfile(sys.argv[1], dtype='<f4')
if x.size < 16: print("0 0"); sys.exit()
mono = x.reshape(-1,2).mean(axis=1)
win = 2400
e = np.array([np.sqrt(np.mean(mono[i:i+win]**2)) for i in range(0, len(mono)-win, win)])
onset = int(np.argmax(e > 0.02)) * win if (e > 0.02).any() else 0
seg = mono[onset+4800: onset+4800+65536]
if seg.size < 8192: seg = mono
rms = float(np.sqrt(np.mean(seg**2)))
s = seg - seg.mean()
sp = np.abs(np.fft.rfft(s*np.hanning(len(s)))); fr = np.fft.rfftfreq(len(s), 1/48000)
print(f"{rms:.4f} {fr[int(np.argmax(sp))]:.1f}")
PY
)
    echo "    monitor RMS = ${rms} ; dominant freq = ${freq} Hz"
    metric tone_rms "$rms"
    metric tone_freq_hz "$freq"
    if awk -v r="$rms" -v lo="$RMS_MIN" -v hi="$RMS_MAX" 'BEGIN{exit !(r+0>=lo && r+0<=hi)}'; then
        pass "monitor RMS ${rms} in [${RMS_MIN},${RMS_MAX}]"; else fail "monitor RMS ${rms} outside [${RMS_MIN},${RMS_MAX}]"; fi
    if awk -v f="$freq" -v lo="$FREQ_LO" -v hi="$FREQ_HI" 'BEGIN{exit !(f+0>=lo && f+0<=hi)}'; then
        pass "dominant freq ${freq}Hz in [${FREQ_LO},${FREQ_HI}]"; else fail "dominant freq ${freq}Hz outside [${FREQ_LO},${FREQ_HI}]"; fi
else
    note "INFO  monitor signal analysis skipped (need parecord + python3-numpy)"
    echo "    (signal analysis skipped: parecord and/or python3-numpy unavailable)"
fi
rm -f "$CAP"

# ---- 5. open / first-event / enumeration latency --------------------------
echo; echo "-- wpw_open (open/first-event/enum latency, 50 iters) --"
open_out=$("$WINE" "$BIN/wpw_open.exe" 50 2>/dev/null)
open_rc=$?
oline=$(echo "$open_out" | grep 'open_p50_us=' || true)
echo "    ${oline:-$open_out}"
if [ "$open_rc" != 0 ] || [ -z "$oline" ]; then
    fail "wpw_open (rc=$open_rc)"
else
    for k in open_p50_us open_p99_us open_max_us firstevt_p50_us firstevt_p99_us enum_p50_us enum_p99_us; do
        metric "$k" "$(tok "$oline" "$k")"
    done
    op99=$(tok "$oline" open_p99_us); fp99=$(tok "$oline" firstevt_p99_us); ep99=$(tok "$oline" enum_p99_us)
    if flt_lt "$op99" "$OPEN_P99_MAX_US"; then c=0; else c=1; fi
    perf_check "$c" "open p99 ${op99}us < ${OPEN_P99_MAX_US}us" "open p99 ${op99}us vs bound ${OPEN_P99_MAX_US}us"
    if flt_lt "$fp99" "$FIRSTEVT_P99_MAX_US"; then c=0; else c=1; fi
    perf_check "$c" "first-event p99 ${fp99}us < ${FIRSTEVT_P99_MAX_US}us" "first-event p99 ${fp99}us vs bound ${FIRSTEVT_P99_MAX_US}us"
    if flt_lt "$ep99" "$ENUM_P99_MAX_US"; then c=0; else c=1; fi
    perf_check "$c" "enum p99 ${ep99}us < ${ENUM_P99_MAX_US}us" "enum p99 ${ep99}us vs bound ${ENUM_P99_MAX_US}us"
fi

# ---- 6. open/close churn + leak check --------------------------------------
echo; echo "-- wpw_stress churn (300 open/close cycles) --"
churn_out=$("$WINE" "$BIN/wpw_stress.exe" churn 300 2>/dev/null)
churn_rc=$?
cline=$(echo "$churn_out" | grep 'cycles=' || true)
echo "    ${cline:-$churn_out}"
ch_errs=$(tok "$cline" errors); ws_w=$(tok "$cline" ws_warmup_kb); ws_f=$(tok "$cline" ws_final_kb)
metric churn_errors "${ch_errs:-999}"
metric ws_warmup_kb "${ws_w:-}"
metric ws_final_kb "${ws_f:-}"
if [ "${ch_errs:-999}" = 0 ]; then pass "churn errors 0"; else fail "churn errors ${ch_errs:-?} (rc=$churn_rc)"; fi
if [ -n "$ws_w" ] && [ -n "$ws_f" ] && awk -v a="$ws_f" -v b="$ws_w" -v s="$WS_GROWTH_MAX_KB" 'BEGIN{exit !(a+0 <= b+0+s)}'; then
    pass "working set ${ws_w}kB -> ${ws_f}kB (growth <= ${WS_GROWTH_MAX_KB}kB)"
else
    fail "working set ${ws_w:-?}kB -> ${ws_f:-?}kB (growth > ${WS_GROWTH_MAX_KB}kB, leak?)"
fi

# ---- 7. many concurrent streams + CPU/xrun sampling -------------------------
echo; echo "-- wpw_stress many ($STRESS_N streams, 10 s) --"
OUT="$(mktemp)"
CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)
pw_pids=$( { pgrep -x pipewire; pgrep -x pipewire-pulse; pgrep -x wireplumber; } 2>/dev/null )
jif() { local s=0 p v; for p in "$@"; do v=$(awk '{print $14+$15}' "/proc/$p/stat" 2>/dev/null); s=$((s + ${v:-0})); done; echo "$s"; }
pw0=$(jif $pw_pids); t0=$(date +%s.%N)
"$WINE" "$BIN/wpw_stress.exe" many "$STRESS_N" 10 >"$OUT" 2>/dev/null &
BG=$!
sleep 2
cl_pids=$(pgrep -f 'wpw_stress\.exe' || true)
cl0=$(jif $cl_pids); ct0=$(date +%s.%N)
sleep 6
cl1=$(jif $cl_pids); ct1=$(date +%s.%N)
XRUNS=-1
if command -v pw-top >/dev/null 2>&1; then
    # pw-top's first batch iteration prints zeroed counters; sample two
    # iterations and parse only the second block (header rows match QUANT)
    XRUNS=$(pw-top -b -n 2 2>/dev/null | awk '/QUANT/{blk++} blk==2 && /wpw_stress/ {f=1; s+=$9} END{print f ? s+0 : -1}')
fi
wait $BG
many_rc=$?
pw1=$(jif $pw_pids); t1=$(date +%s.%N)
mline=$(grep 'streams=' "$OUT" || true)
echo "    ${mline:-$(cat "$OUT")}"
m_to=$(tok "$mline" timeouts); m_err=$(tok "$mline" errors)
cpu_client=$(awk -v d="$((cl1 - cl0))" -v hz="$CLK_TCK" -v a="$ct0" -v b="$ct1" 'BEGIN{w=b-a; printf "%.1f", (w>0 ? d/hz/w*100 : 0)}')
cpu_pw=$(awk -v d="$((pw1 - pw0))" -v hz="$CLK_TCK" -v a="$t0" -v b="$t1" 'BEGIN{w=b-a; printf "%.1f", (w>0 ? d/hz/w*100 : 0)}')
echo "    cpu: client=${cpu_client}% pipewire-daemons=${cpu_pw}% ; xruns=${XRUNS}"
metric many_timeouts "${m_to:-999}"
metric many_errors "${m_err:-999}"
metric cpu_client_pct "$cpu_client"
metric cpu_pwdaemon_pct "$cpu_pw"
metric xruns "$XRUNS"
if [ "${m_to:-999}" = 0 ] && [ "${m_err:-999}" = 0 ]; then
    pass "many $STRESS_N streams (0 timeouts, 0 errors)"
else
    fail "many $STRESS_N streams (timeouts=${m_to:-?} errors=${m_err:-?} rc=$many_rc)"
fi
rm -f "$OUT"

# ---- 8. SetSampleRate churn (IAudioClockAdjustment) -------------------------
echo; echo "-- wpw_stress ratechurn (8 s, toggling every 500 ms) --"
CAP="$(mktemp --suffix=.raw)"
PAREC=""
if command -v parecord >/dev/null 2>&1; then
    parecord -d @DEFAULT_MONITOR@ --format=float32le --rate=48000 --channels=2 --latency-msec=100 --raw "$CAP" >/dev/null 2>&1 &
    PAREC=$!
    sleep 0.6
fi
rate_out=$("$WINE" "$BIN/wpw_stress.exe" ratechurn 8 2>/dev/null)
rate_rc=$?
[ -n "$PAREC" ] && { sleep 0.5; kill "$PAREC" 2>/dev/null; wait "$PAREC" 2>/dev/null; }
rline=$(echo "$rate_out" | grep 'setrate_calls=' || true)
echo "    ${rline:-$rate_out}"
if [ "$rate_rc" = 3 ]; then
    note "INFO  ratechurn skipped (SetSampleRate unsupported on this backend)"
    echo "    (skipped: SetSampleRate unsupported)"
else
    rt_errs=$(tok "$rline" errors)
    metric ratechurn_errors "${rt_errs:-999}"
    if [ "$rate_rc" = 0 ] && [ "${rt_errs:-999}" = 0 ]; then pass "ratechurn errors 0"
    else fail "ratechurn (rc=$rate_rc errors=${rt_errs:-?})"; fi
    # tail of the capture must be back at the original rate: ~440 Hz, not ~880
    if [ -n "$PAREC" ] && [ -s "$CAP" ] && python3 -c 'import numpy' >/dev/null 2>&1; then
        tail_freq=$(python3 - "$CAP" <<'PY'
import sys, numpy as np
x = np.fromfile(sys.argv[1], dtype='<f4')
if x.size < 2*48000*2: print("0"); sys.exit()
mono = x.reshape(-1,2).mean(axis=1)
# last second of ACTIVE audio (the >=1.5 s tail plays at the restored rate);
# blind tail indexing would race parecord's kill point
win = 2400
e = np.array([np.sqrt(np.mean(mono[i:i+win]**2)) for i in range(0, len(mono)-win, win)])
act = np.nonzero(e > 0.02)[0]
if act.size == 0: print("0"); sys.exit()
end = (act[-1] + 1) * win
seg = mono[max(0, end-48000):end]
s = seg - seg.mean()
sp = np.abs(np.fft.rfft(s*np.hanning(len(s)))); fr = np.fft.rfftfreq(len(s), 1/48000)
print(f"{fr[int(np.argmax(sp))]:.1f}")
PY
)
        echo "    post-churn tail dominant freq = ${tail_freq} Hz"
        metric ratechurn_tail_freq_hz "$tail_freq"
        if awk -v f="$tail_freq" -v lo="$FREQ_LO" -v hi="$FREQ_HI" 'BEGIN{exit !(f+0>=lo && f+0<=hi)}'; then
            pass "ratechurn tail freq ${tail_freq}Hz in [${FREQ_LO},${FREQ_HI}] (rate restored)"
        else
            fail "ratechurn tail freq ${tail_freq}Hz outside [${FREQ_LO},${FREQ_HI}] (compounding ratio?)"
        fi
    else
        note "INFO  ratechurn tail analysis skipped (need parecord + python3-numpy)"
    fi
fi
rm -f "$CAP"

# ---- 9. loopback capture integrity ------------------------------------------
echo; echo "-- wpw_loopcap (render + WASAPI loopback round-trip) --"
loop_out=$("$WINE" "$BIN/wpw_loopcap.exe" 4 440 2>/dev/null)
loop_rc=$?
lline=$(echo "$loop_out" | grep 'cap_samples=' || true)
echo "    ${lline:-$loop_out}"
l_rms=$(tok "$lline" cap_rms); l_ratio=$(tok "$lline" tone_ratio); l_disc=$(tok "$lline" discont); l_errs=$(tok "$lline" errors)
metric loop_rms "${l_rms:-0}"
metric loop_tone_ratio "${l_ratio:-0}"
metric loop_discont "${l_disc:-999}"
metric loop_errors "${l_errs:-999}"
if [ "${l_errs:-999}" = 0 ]; then pass "loopcap errors 0"; else fail "loopcap errors ${l_errs:-?} (rc=$loop_rc)"; fi
if awk -v r="${l_rms:-0}" -v lo="$RMS_MIN" -v hi="$RMS_MAX" 'BEGIN{exit !(r+0>=lo && r+0<=hi)}'; then
    pass "loopcap RMS ${l_rms} in [${RMS_MIN},${RMS_MAX}]"; else fail "loopcap RMS ${l_rms:-?} outside [${RMS_MIN},${RMS_MAX}]"; fi
if awk -v r="${l_ratio:-0}" 'BEGIN{exit !(r+0 >= 0.5)}'; then
    pass "loopcap tone ratio ${l_ratio} >= 0.5 (sink capture, not mic)"; else fail "loopcap tone ratio ${l_ratio:-?} < 0.5 (wrong source?)"; fi
if [ -n "${l_disc:-}" ] && [ "${l_disc:-999}" -le 1 ]; then
    pass "loopcap discontinuities ${l_disc} <= 1"; else fail "loopcap discontinuities ${l_disc:-?} > 1"; fi

# ---- report --------------------------------------------------------------
cp -f "$TSV" "$BENCH_DIR/results/${DRIVER}-latest.tsv"
echo; echo "===================== REPORT ====================="
for line in "${REPORT[@]}"; do echo "  $line"; done
echo "=================================================="
echo "metrics: $TSV"
if [ "$FAILS" = 0 ]; then echo "ALL CHECKS PASSED"; exit 0; else echo "$FAILS CHECK(S) FAILED"; exit 1; fi
