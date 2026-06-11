#!/usr/bin/env bash
# Compare two run.sh metric TSVs.
#
#   ./compare.sh A.tsv B.tsv
#       Side-by-side markdown table (union of metrics, delta% where numeric)
#       to stdout: the driver-vs-driver presentation deliverable.
#
#   ./compare.sh --check [--max-regress PCT] baseline.tsv new.tsv
#       Regression gate: for every lower-is-better metric present in both
#       files, FAIL when new > base * (1 + PCT/100) AND new - base > epsilon
#       (per-metric absolute slack, so noise on tiny values never trips).
#       Prints REGRESSION/ok/SKIP lines; exit 1 on any regression, else 0.
#
# TSV format (produced by run.sh): leading '# key=value ...' header lines,
# then one 'metric<TAB>value' row per measurement.
set -uo pipefail

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
}

MODE=table
MAXR=25
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --check) MODE=check ;;
        --max-regress) shift; MAXR="${1:-25}" ;;
        -h|--help) usage; exit 0 ;;
        *) ARGS+=("$1") ;;
    esac
    shift
done
if [ "${#ARGS[@]}" != 2 ]; then usage; exit 2; fi
A="${ARGS[0]}"; B="${ARGS[1]}"
[ -f "$A" ] || { echo "error: no such file: $A" >&2; exit 2; }
[ -f "$B" ] || { echo "error: no such file: $B" >&2; exit 2; }

if [ "$MODE" = table ]; then
    awk -F'\t' -v na="$(basename "$A" .tsv)" -v nb="$(basename "$B" .tsv)" '
        function isnum(s) { return s ~ /^-?[0-9]+([.][0-9]+)?$/ }
        BEGIN {
            # direction of improvement per metric
            dir["phase_sd_ms"] = "lower";  dir["evt_interval_sd_ms"] = "lower"
            dir["evt_interval_p99_ms"] = "lower"; dir["drift_us"] = "lower"
            dir["pos_nonmono"] = "lower"; dir["lwp"] = "lower"
            dir["open_p50_us"] = "lower"; dir["open_p99_us"] = "lower"
            dir["open_max_us"] = "lower"; dir["firstevt_p50_us"] = "lower"
            dir["firstevt_p99_us"] = "lower"; dir["enum_p50_us"] = "lower"
            dir["enum_p99_us"] = "lower"; dir["churn_errors"] = "lower"
            dir["ws_warmup_kb"] = "lower"; dir["ws_final_kb"] = "lower"
            dir["many_timeouts"] = "lower"; dir["many_errors"] = "lower"
            dir["cpu_client_pct"] = "lower"; dir["cpu_pwdaemon_pct"] = "lower"
            dir["xruns"] = "lower"; dir["tone_errors"] = "lower"
            dir["ratechurn_errors"] = "lower"; dir["loop_errors"] = "lower"
            dir["loop_discont"] = "lower"
            dir["loop_tone_ratio"] = "higher"
            # closer-to-target metrics
            tgt["tone_freq_hz"] = 440; tgt["ratechurn_tail_freq_hz"] = 440
            tgt["tone_rms"] = 0.177; tgt["loop_rms"] = 0.177
        }
        function abs(x) { return x < 0 ? -x : x }
        FNR == 1 { fi++ }
        /^#/ {
            hdr[fi] = hdr[fi] "> " substr($0, 3) "\n"
            if (match($0, /driver=[a-zA-Z0-9_-]+/))
                drv[fi] = substr($0, RSTART + 7, RLENGTH - 7)
            next
        }
        NF >= 2 {
            if (!($1 in seen)) { seen[$1] = 1; order[++nk] = $1 }
            val[fi, $1] = $2
        }
        END {
            if (drv[1] == "") drv[1] = na
            if (drv[2] == "") drv[2] = nb
            printf "# Driver comparison: %s vs %s\n\n", na, nb
            printf "**%s**\n%s\n**%s**\n%s\n", na, hdr[1], nb, hdr[2]
            print "Reading the table: *better* says which direction wins for that metric"
            print "(`lower`/`higher`), `~ N` means closer to N wins, and `gate` rows are"
            print "pass/fail criteria asserted by run.sh against an expected count (see"
            print "BASELINES.md), not a race between drivers.  *winner* applies the"
            print "*better* rule to the two values; `tie` when they are equal."
            print ""
            printf "| metric | better | %s | %s | delta%% | winner |\n", na, nb
            print  "|---|:---:|---:|---:|---:|:---:|"
            for (k = 1; k <= nk; k++) {
                m = order[k]
                a = ((1, m) in val) ? val[1, m] : "-"
                b = ((2, m) in val) ? val[2, m] : "-"
                d = "-"
                if (isnum(a) && isnum(b) && a + 0 != 0)
                    d = sprintf("%+.1f%%", (b - a) / a * 100)
                if (m ~ /^conf_/)        better = "gate"
                else if (m in tgt)       better = sprintf("~ %g", tgt[m])
                else if (m in dir)       better = dir[m]
                else                     better = "-"
                win = "-"
                if (better != "gate" && better != "-" && isnum(a) && isnum(b) \
                    && a + 0 >= 0 && b + 0 >= 0) {   # negative = not measured
                    if (m in tgt)            { da = abs(a - tgt[m]); db = abs(b - tgt[m]) }
                    else if (dir[m] == "lower")  { da = a + 0; db = b + 0 }
                    else                         { da = -(a + 0); db = -(b + 0) }
                    win = (da < db) ? drv[1] : (db < da) ? drv[2] : "tie"
                }
                printf "| %s | %s | %s | %s | %s | %s |\n", m, better, a, b, d, win
            }
        }
    ' "$A" "$B"
    exit 0
fi

# --check mode: A = baseline, B = new
awk -F'\t' -v maxr="$MAXR" '
    BEGIN {
        # lower-is-better metrics and their absolute-slack epsilons
        eps["phase_sd_ms"]        = 0.05
        eps["evt_interval_sd_ms"] = 0.05
        eps["evt_interval_p99_ms"]= 0.5
        eps["drift_us"]           = 200
        eps["open_p50_us"]        = 200
        eps["open_p99_us"]        = 500
        eps["firstevt_p99_us"]    = 500
        eps["enum_p50_us"]        = 200
        eps["enum_p99_us"]        = 500
        eps["cpu_client_pct"]     = 1.0
        eps["cpu_pwdaemon_pct"]   = 1.0
        eps["xruns"]              = 1
        eps["ws_final_kb"]        = 1024
        bad = 0
    }
    function isnum(s) { return s ~ /^-?[0-9]+([.][0-9]+)?$/ }
    FNR == 1 { fi++ }
    /^#/ { next }
    NF >= 2 { val[fi, $1] = $2 }
    END {
        for (m in eps) {
            if (!((1, m) in val) || !((2, m) in val)) { printf "SKIP %s (missing)\n", m; continue }
            base = val[1, m]; new = val[2, m]
            if (!isnum(base) || !isnum(new) || base + 0 < 0 || new + 0 < 0) {
                printf "SKIP %s (non-numeric or negative)\n", m; continue
            }
            base += 0; new += 0
            if (new > base * (1 + maxr / 100) && new - base > eps[m]) {
                pct = base != 0 ? (new - base) / base * 100 : 999
                printf "REGRESSION %s %g -> %g (+%.1f%%)\n", m, base, new, pct
                bad++
            } else {
                pct = base != 0 ? (new - base) / base * 100 : 0
                printf "ok %s %g -> %g (%+.1f%%)\n", m, base, new, pct
            }
        }
        if (bad) { printf "%d REGRESSION(S) (threshold +%s%%)\n", bad, maxr; exit 1 }
        print "no regressions"
    }
' "$A" "$B"
