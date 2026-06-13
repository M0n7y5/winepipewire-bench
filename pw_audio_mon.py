#!/usr/bin/env python3
"""Host-side PipeWire audio monitor for an in-game MangoHud overlay.

Captures the default sink monitor (per-channel RMS/peak via numpy) and polls
`pw-top` (DSP load / xruns / quantum), then renders compact Unicode-sparkline
HUD rows to files in $XDG_RUNTIME_DIR.  MangoHud `exec=cat`s those files, so the
overlay updates live with one monitor and no game-side dependencies.

Rows written (one line each, no label - MangoHud's custom_text supplies labels):
  pw-row-info.txt : q<quantum> <latency>ms <rate>k str<N> xrun:<N>
  pw-row-dsp.txt  : <sparkline> <pct>%        (DSP load = pw-top B/Q, history)
  pw-row-lvl.txt  : L<spark><dB> R<spark><dB>  (output level, history + dBFS, !=clip)
  pw-hud.txt      : all of the above on one line (for a single-row config)

Run on the HOST before launching the game:
  ./pw_audio_mon.py &
Stop with Ctrl-C or `kill`.  See --help for options.

MangoHud (~/.config/MangoHud/MangoHud.conf), needs legacy_layout for exec=,
and `env -u LD_PRELOAD` to dodge the Steam-container exec bug (#1339):
  legacy_layout=1
  custom_text=PW
  exec=env -u LD_PRELOAD cat $XDG_RUNTIME_DIR/pw-row-info.txt
  custom_text=DSP
  exec=env -u LD_PRELOAD cat $XDG_RUNTIME_DIR/pw-row-dsp.txt
  custom_text=Lvl
  exec=env -u LD_PRELOAD cat $XDG_RUNTIME_DIR/pw-row-lvl.txt
Launch the game with MangoHud enabled (Steam per-game toggle or `mangohud %command%`).
"""
import argparse
import math
import os
import shutil
import signal
import subprocess
import sys
import threading
import time

import numpy as np

BLOCKS = " ▁▂▃▄▅▆▇█"            # 0..8, index 0 = space (silence/idle)
RATE = 48000
CHANNELS = 2

stop = threading.Event()
shared = {"dsp": 0.0, "xrun": 0, "quant": 0, "rate": 0, "streams": 0}
shared_lock = threading.Lock()


def spark(values, lo, hi):
    """Render a list of values to a Unicode sparkline, each clamped to [lo, hi]."""
    out = []
    span = hi - lo if hi > lo else 1.0
    for v in values:
        i = int(round((v - lo) / span * (len(BLOCKS) - 1)))
        out.append(BLOCKS[min(len(BLOCKS) - 1, max(0, i))])
    return "".join(out)


def dbfs(rms):
    return -120.0 if rms <= 1e-9 else 20.0 * math.log10(rms)

def block(v, lo, hi):
    span = hi - lo if hi > lo else 1.0
    i = int(round((v - lo) / span * (len(BLOCKS) - 1)))
    return BLOCKS[min(len(BLOCKS) - 1, max(0, i))]


def read_spatial(path, floor):
    """Parse the mmdevapi spatial stats file into a bed-meter row, or None."""
    try:
        if time.time() - os.path.getmtime(path) > 2.0:
            return None                      # stale: game gone / stats off
        with open(path) as f:
            txt = f.read().strip()
    except OSError:
        return None
    if not txt or txt.endswith("idle"):
        return None
    meta, chans = {}, []
    for t in txt.split():
        if ":" not in t:
            continue
        k, v = t.split(":", 1)
        if k in ("hrtf", "bed", "dyn"):
            meta[k] = v
        else:
            try:
                chans.append((k, float(v)))
            except ValueError:
                pass
    tag = "HRTF" if meta.get("hrtf") == "1" else "pan"
    if not chans:
        return f"{tag} (no bed)"
    return tag + " " + " ".join(f"{n}{block(db, floor, 0)}" for n, db in chans)


def atomic_write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(text)
    os.replace(tmp, path)


def pwtop_poll(interval):
    """Background: parse `pw-top -b -n 2`, keep the last (measured) table."""
    while not stop.is_set():
        dsp = xrun = quant = rate = streams = 0
        try:
            out = subprocess.run(["pw-top", "-b", "-n", "2"], capture_output=True,
                                 text=True, timeout=interval + 4,
                                 env={**os.environ, "LC_ALL": "C"}).stdout
            maxbq = 0.0
            for line in out.splitlines():
                f = line.split()
                if len(f) < 9 or "QUANT" in line:
                    if "QUANT" in line:        # new table -> reset accumulators
                        dsp = xrun = quant = rate = streams = 0
                        maxbq = 0.0
                    continue
                try:
                    r = int(f[3])
                except ValueError:
                    continue
                if r <= 0:
                    continue
                bq = float(f[7].replace(",", "."))
                err = int(f[8])
                xrun += err
                streams += 1
                if bq >= maxbq:
                    maxbq, quant, rate = bq, int(f[2]), r
            dsp = maxbq * 100.0
        except Exception:
            pass
        with shared_lock:
            shared.update(dsp=dsp, xrun=xrun, quant=quant, rate=rate, streams=streams)
        stop.wait(interval)


def main():
    ap = argparse.ArgumentParser(description="PipeWire audio monitor -> MangoHud overlay files")
    ap.add_argument("--source", default="@DEFAULT_MONITOR@", help="capture source (default: default sink monitor)")
    ap.add_argument("--dir", default=os.environ.get("XDG_RUNTIME_DIR", "/tmp"), help="output dir for HUD files")
    ap.add_argument("--interval", type=float, default=0.1, help="render interval seconds (default 0.1)")
    ap.add_argument("--width", type=int, default=20, help="sparkline history width")
    ap.add_argument("--floor", type=float, default=-54.0, help="level sparkline floor dBFS")
    ap.add_argument("--spatial", default=None, help="mmdevapi spatial stats file (default: <dir>/pw-spatial.txt)")
    args = ap.parse_args()

    if not shutil.which("parecord"):
        sys.exit("parecord not found (install pipewire-pulse / pulseaudio-utils)")

    rec = subprocess.Popen(
        ["parecord", "-d", args.source, "--format=float32le",
         f"--rate={RATE}", f"--channels={CHANNELS}", "--raw", "--latency-msec=20"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    poller = threading.Thread(target=pwtop_poll, args=(1.0,), daemon=True)
    poller.start()

    def shutdown(*_):
        stop.set()
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)

    chunk_bytes = int(args.interval * RATE) * CHANNELS * 4
    hist_dsp, hist_l, hist_r = [], [], []
    last_xrun, xrun_hot = 0, 0.0

    info_p = os.path.join(args.dir, "pw-row-info.txt")
    dsp_p = os.path.join(args.dir, "pw-row-dsp.txt")
    lvl_p = os.path.join(args.dir, "pw-row-lvl.txt")
    all_p = os.path.join(args.dir, "pw-hud.txt")
    bed_p = os.path.join(args.dir, "pw-row-bed.txt")
    spatial_path = args.spatial or os.path.join(args.dir, "pw-spatial.txt")

    while not stop.is_set():
        raw = rec.stdout.read(chunk_bytes)
        if not raw:
            break
        a = np.frombuffer(raw, dtype=np.float32)
        n = (a.size // CHANNELS) * CHANNELS
        a = a[:n].reshape(-1, CHANNELS)
        rms_l, rms_r = float(np.sqrt(np.mean(a[:, 0] ** 2))), float(np.sqrt(np.mean(a[:, 1] ** 2)))
        pk_l, pk_r = float(np.max(np.abs(a[:, 0]))), float(np.max(np.abs(a[:, 1])))
        d_l, d_r = dbfs(rms_l), dbfs(rms_r)

        for h, v in ((hist_l, d_l), (hist_r, d_r)):
            h.append(v)
            del h[:-args.width]
        with shared_lock:
            s = dict(shared)
        hist_dsp.append(s["dsp"])
        del hist_dsp[:-args.width]

        clip = " CLIP" if max(pk_l, pk_r) >= 0.999 else ""
        if s["xrun"] > last_xrun:
            xrun_hot = time.monotonic()
        last_xrun = s["xrun"]
        xr_mark = "!" if time.monotonic() - xrun_hot < 2.0 else ""

        lat = (s["quant"] / s["rate"] * 1000.0) if s["rate"] else 0.0
        info = f"q{s['quant']} {lat:.1f}ms {s['rate'] // 1000}k str{s['streams']} xrun:{s['xrun']}{xr_mark}"
        dsp = f"{spark(hist_dsp, 0, 100)} {s['dsp']:.0f}%"
        lvl = (f"L{spark(hist_l, args.floor, 0)}{d_l:+.0f} "
               f"R{spark(hist_r, args.floor, 0)}{d_r:+.0f}{clip}")
        bed = read_spatial(spatial_path, args.floor)

        atomic_write(info_p, info + "\n")
        atomic_write(dsp_p, dsp + "\n")
        atomic_write(lvl_p, lvl + "\n")
        atomic_write(bed_p, (bed or "-") + "\n")
        atomic_write(all_p, f"{info}  DSP {dsp}  {lvl}" + (f"  {bed}" if bed else "") + "\n")

    stop.set()
    rec.terminate()
    try:
        rec.wait(timeout=2)
    except Exception:
        rec.kill()


if __name__ == "__main__":
    main()
