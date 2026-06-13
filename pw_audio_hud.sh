#!/bin/sh
# One-line PipeWire audio status, for a terminal or a MangoHud `exec=` feed.
#
# Parses `pw-top -b` (stable left-anchored columns:
#   S ID QUANT RATE WAIT BUSY W/Q B/Q ERR FORMAT NAME).
# Reports, across all currently-running nodes (RATE>0):
#   - rate/quantum of the most loaded node (the audio period in frames),
#   - dsp:  max B/Q  = process time / quantum, i.e. how close to overrun (100% = no headroom),
#   - xrun: summed ERR = PipeWire-graph underruns since the node started,
#   - (N str) = number of active nodes.
#
# Usage:
#   ./pw_audio_hud.sh                 # one snapshot
#   watch -n0.5 ./pw_audio_hud.sh     # live terminal monitor
#   # in-game overlay (host poller + MangoHud cat, see README note below):
#   while :; do ./pw_audio_hud.sh; sleep 0.5; done > "$XDG_RUNTIME_DIR/pw-hud.txt"
#   #   then in ~/.config/MangoHud/MangoHud.conf:
#   #     custom_text=Audio
#   #     exec=cat "$XDG_RUNTIME_DIR/pw-hud.txt"
LC_ALL=C pw-top -b -n 2 2>/dev/null | awk '
/QUANT/ && /RATE/ { n=0; xr=0; maxbq=0; rr=0; q=0; next }   # header: new table, keep only the last (measured) one
{
  r = $4 + 0
  if (r <= 0) next                       # only running nodes
  bq = $8; gsub(/,/, ".", bq); gsub(/[^0-9.]/, "", bq); bq = bq + 0
  xr += ($9 + 0)
  if (bq >= maxbq) { maxbq = bq; q = $3 + 0; rr = r }
  n++
}
END {
  if (n == 0) { print "PW: idle"; exit }
  printf "PW: %gk q%d dsp:%d%% xrun:%d (%d str)\n", rr/1000, q, maxbq*100 + 0.5, xr, n
}'
