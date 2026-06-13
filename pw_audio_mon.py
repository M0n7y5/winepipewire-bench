#!/usr/bin/env python3
"""Host-side PipeWire audio collector feeding the in-game MangoHud audio overlay.

The patched MangoHud "audio" element runs inside the game process and renders the
HUD itself (DSP histogram, output L/R meters, per-channel bed bars). This daemon
only gathers the data the element cannot see from inside the container and writes
one machine-readable stats file the element reads:

    ~/.cache/pw-audio-hud/pw-stats.txt

That path is under $HOME on purpose: Steam's pressure-vessel gives the game a
private $XDG_RUNTIME_DIR, but $HOME is bind-mounted through, so a file there
bridges the host (this daemon) and the in-container overlay.

Data sources (all host-side):
  - `pw-top -b -n 2`                          -> DSP load, xruns, quantum, rate, stream count
  - capture of the default sink monitor       -> output L/R RMS levels (dBFS)
  - the mmdevapi spatial stats file           -> per-channel bed dBFS + HRTF flag

File format (one "key value..." line each; the element ignores unknown keys):
    dsp <percent>
    xrun <count>
    quantum <frames>
    rate <hz>
    streams <count>
    lvl <left_dbfs> <right_dbfs>
    hrtf <0|1>
    bed <0|1>
    ch <NAME> <dbfs>        (one per bed channel)

Run on the HOST before launching the game:
    python3 pw_audio_mon.py &

Launch the game with MangoHud (our patched build) + `audio=1` in the config, and
point the driver's WINE_SPATIAL_STATS at <dir>/pw-spatial.txt for the bed row.
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

RATE = 48000
CHANNELS = 2

stop = threading.Event()
shared = {"dsp": 0.0, "xrun": 0, "quant": 0, "rate": 0, "streams": 0}
shared_lock = threading.Lock()


def dbfs(rms):
    return -120.0 if rms <= 1e-9 else 20.0 * math.log10(rms)


def read_spatial(path):
    """Parse the mmdevapi spatial stats file -> (hrtf, bed, [(name, dbfs), ...]) or None."""
    try:
        if time.time() - os.path.getmtime(path) > 2.0:
            return None                      # stale: game gone / stats off
        with open(path) as f:
            txt = f.read().strip()
    except OSError:
        return None
    if not txt or txt.endswith("idle"):
        return None
    hrtf = bed = False
    chans = []
    for t in txt.split():
        if ":" not in t:
            continue
        k, v = t.split(":", 1)
        if k == "hrtf":
            hrtf = v == "1"
        elif k == "bed":
            bed = v == "1"
        elif k == "dyn":
            pass
        else:
            try:
                chans.append((k, float(v)))
            except ValueError:
                pass
    return hrtf, bed, chans


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
    ap = argparse.ArgumentParser(description="PipeWire audio monitor -> MangoHud audio overlay stats file")
    ap.add_argument("--source", default="@DEFAULT_MONITOR@", help="capture source (default: default sink monitor)")
    ap.add_argument("--dir", default=os.path.join(os.path.expanduser("~"), ".cache", "pw-audio-hud"),
                    help="output dir (default ~/.cache/pw-audio-hud, shared into the Steam container; $XDG_RUNTIME_DIR is NOT)")
    ap.add_argument("--interval", type=float, default=0.1, help="capture/update interval seconds (default 0.1)")
    ap.add_argument("--spatial", default=None, help="mmdevapi spatial stats file (default: <dir>/pw-spatial.txt)")
    args = ap.parse_args()
    os.makedirs(args.dir, exist_ok=True)

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
    stats_p = os.path.join(args.dir, "pw-stats.txt")
    spatial_path = args.spatial or os.path.join(args.dir, "pw-spatial.txt")

    while not stop.is_set():
        raw = rec.stdout.read(chunk_bytes)
        if not raw:
            break
        a = np.frombuffer(raw, dtype=np.float32)
        n = (a.size // CHANNELS) * CHANNELS
        a = a[:n].reshape(-1, CHANNELS)
        d_l = dbfs(float(np.sqrt(np.mean(a[:, 0] ** 2))))
        d_r = dbfs(float(np.sqrt(np.mean(a[:, 1] ** 2))))

        with shared_lock:
            s = dict(shared)

        lines = [
            f"dsp {s['dsp']:.1f}",
            f"xrun {s['xrun']}",
            f"quantum {s['quant']}",
            f"rate {s['rate']}",
            f"streams {s['streams']}",
            f"lvl {d_l:.1f} {d_r:.1f}",
        ]
        sp = read_spatial(spatial_path)
        if sp:
            hrtf, bed, chans = sp
            lines.append(f"hrtf {1 if hrtf else 0}")
            lines.append(f"bed {1 if bed else 0}")
            for nm, db in chans:
                lines.append(f"ch {nm} {db:.1f}")
        atomic_write(stats_p, "\n".join(lines) + "\n")

    stop.set()
    rec.terminate()
    try:
        rec.wait(timeout=2)
    except Exception:
        rec.kill()


if __name__ == "__main__":
    main()
