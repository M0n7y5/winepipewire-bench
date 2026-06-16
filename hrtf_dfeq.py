#!/usr/bin/env python3
"""Diffuse-field equalize a SOFA HRIR for the winepipewire.drv SOFA backend.

The driver's SOFA backend (dlls/mmdevapi/unix/spatial.c, WINE_SPATIAL_SOFA)
convolves raw HRIRs.  A raw / un-equalized HRTF set carries the measurement
rig's coloration as a direction-independent spectral tilt (the diffuse-field
response), which sounds tonally wrong.  Diffuse-field equalization removes ONLY
that direction-independent part, leaving every direction-dependent cue (ILD,
ITD, pinna notches) untouched, so localization is preserved while the timbre is
neutralized.

The correction is one global magnitude curve = 1 / (smoothed diffuse-field
average), realized as a minimum-phase filter and applied length-preserving (the
output keeps the input's tap count and SOFA dimensions), so the driver loads the
result with no code change.

Many published sets are ALREADY diffuse-field equalized (e.g. Valve's SADIE D1,
whose own History lists "Diffuse Field Equalization").  For those this tool
measures a flat diffuse field and reports the correction as a near-identity
no-op; it is meant for vetting and fixing the raw sets you might otherwise drop
into WINE_SPATIAL_SOFA.

Deps: numpy + h5py (SOFA is HDF5).  Run with:
    uv run --with h5py --with numpy python hrtf_dfeq.py in.sofa [--out out.sofa]

Usage:
    hrtf_dfeq.py in.sofa                 # measure + print the diffuse-field curve
    hrtf_dfeq.py in.sofa --out eq.sofa   # also write a DF-equalized copy
Options: --smooth-oct 0.333  --max-boost 12  --max-cut 12  --lo-hz 30  --hi-hz 18000
"""
import argparse, shutil, sys
import numpy as np
import h5py

GRID = [40,55,75,100,140,190,260,360,500,700,1000,1400,2000,2800,4000,5600,8000,11000,15000,20000]


def load(path):
    f = h5py.File(path, "r")
    ir = np.array(f["Data.IR"], dtype=np.float64)            # (M,R,N)
    rate = float(np.array(f["Data.SamplingRate"]).ravel()[0])
    sp = np.array(f["SourcePosition"], dtype=np.float64)     # (M,3)
    sp_type = f["SourcePosition"].attrs.get("Type", b"spherical")
    sp_type = sp_type.decode() if isinstance(sp_type, bytes) else sp_type
    f.close()
    return ir, rate, sp, sp_type


def area_weights(sp, sp_type):
    """Solid-angle weight per measurement so the average is a true spherical
    (diffuse-field) mean, not biased by denser sampling near the horizon."""
    if "spher" not in str(sp_type).lower():
        return np.ones(sp.shape[0])
    el = sp[:, 1]
    uel = np.unique(el)
    if uel.size < 2:
        return np.ones(sp.shape[0])
    mids = (uel[:-1] + uel[1:]) / 2.0
    lo = np.concatenate([[-90.0], mids])
    hi = np.concatenate([mids, [90.0]])
    ring_sa = 2 * np.pi * (np.sin(np.radians(hi)) - np.sin(np.radians(lo)))
    w = np.zeros(sp.shape[0])
    for i, e in enumerate(uel):
        idx = np.where(el == e)[0]
        w[idx] = ring_sa[i] / len(idx)
    return w / w.sum()


def df_average(ir, w):
    """Area-weighted RMS magnitude over all measurements and both ears -> (F,)."""
    mag2 = np.abs(np.fft.rfft(ir, axis=2)) ** 2          # (M,R,F)
    P2 = np.tensordot(w, mag2, axes=(0, 0)) / w.sum()    # (R,F)
    return np.sqrt(P2.mean(axis=0))                      # (F,)


def smooth_oct(mag, freqs, frac):
    """Fractional-octave geometric smoothing of a magnitude curve."""
    if frac <= 0:
        return mag.copy()
    r = 2.0 ** (frac / 2.0)
    out = np.empty_like(mag)
    for i, fc in enumerate(freqs):
        if fc <= 0:
            out[i] = mag[i]; continue
        sel = (freqs >= fc / r) & (freqs <= fc * r)
        out[i] = np.sqrt(np.mean(mag[sel] ** 2)) if sel.any() else mag[i]
    return out


def minphase_response(target_mag, n):
    """Min-phase complex response (rfft grid, n//2+1 bins) with |H| = target_mag.
    target_mag is given on the rfft grid; mirrored to full n for the cepstrum."""
    full = np.concatenate([target_mag, target_mag[-2:0:-1]])     # length n, even symmetric
    logm = np.log(np.maximum(full, 1e-9))
    cep = np.fft.ifft(logm).real
    win = np.zeros(n)
    win[0] = 1.0
    win[1:n // 2] = 2.0
    win[n // 2] = 1.0
    Hmp = np.exp(np.fft.fft(cep * win))                          # |Hmp| ~= full, min-phase
    return Hmp[:n // 2 + 1]


def db(x):
    return 20 * np.log10(np.maximum(x, 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("in_sofa")
    ap.add_argument("--out")
    ap.add_argument("--smooth-oct", type=float, default=1.0 / 3.0)
    ap.add_argument("--max-boost", type=float, default=12.0)
    ap.add_argument("--max-cut", type=float, default=12.0)
    ap.add_argument("--lo-hz", type=float, default=30.0)
    ap.add_argument("--hi-hz", type=float, default=18000.0)
    ap.add_argument("--flat-thresh", type=float, default=1.0,
                    help="warn that correction is a near no-op below this dB span")
    a = ap.parse_args()

    ir, rate, sp, sp_type = load(a.in_sofa)
    M, R, N = ir.shape
    freqs = np.fft.rfftfreq(N, 1.0 / rate)
    w = area_weights(sp, sp_type)
    P = df_average(ir, w)
    k1k = int(np.argmin(np.abs(freqs - 1000.0)))
    Pdb = db(P) - db(P)[k1k]

    print(f"# {a.in_sofa}")
    print(f"# M={M} R={R} N={N} rate={rate:.0f}  weighting={'area' if not np.allclose(w, w[0]) else 'uniform'}")
    print("# diffuse-field average (dB rel 1 kHz):")
    print("#  freq      dB")
    for g in GRID:
        if g > freqs[-1]:
            continue
        k = int(np.argmin(np.abs(freqs - g)))
        print(f"  {g:6d}  {Pdb[k]:+7.2f}")
    band = (freqs >= 100) & (freqs <= 16000)
    span = Pdb[band].max() - Pdb[band].min()
    print(f"# DF 100Hz-16kHz: mean {Pdb[band].mean():+.2f}  span {span:.2f} dB")

    if not a.out:
        if span < a.flat_thresh:
            print(f"# VERDICT: already diffuse-field flat (span {span:.2f} < {a.flat_thresh} dB); no DF-EQ needed.")
        else:
            print(f"# VERDICT: non-flat diffuse field (span {span:.2f} dB); re-run with --out to correct.")
        return

    # correction magnitude on the rfft grid: target(flat) / smoothed DF average
    Psm = smooth_oct(P, freqs, a.smooth_oct)
    C = (Psm[k1k] / np.maximum(Psm, 1e-9))               # flat target, normalized at 1 kHz
    C = np.clip(C, 10 ** (-a.max_cut / 20), 10 ** (a.max_boost / 20))
    # full correction across [lo_hz, hi_hz]; roll the correction off to unity only
    # OUTSIDE that band (over a narrow ~1/3-octave skirt) so band edges and the
    # noisy top octave are not boosted, but the audible band is fully corrected.
    lo2 = a.lo_hz / (2.0 ** (1.0 / 3.0))
    hi2 = a.hi_hz * (2.0 ** (1.0 / 3.0))
    taper = np.ones_like(C)
    taper[freqs <= lo2] = 0.0
    taper[freqs >= hi2] = 0.0
    lo_ramp = (freqs > lo2) & (freqs < a.lo_hz)
    hi_ramp = (freqs > a.hi_hz) & (freqs < hi2)
    taper[lo_ramp] = np.clip(np.log2(freqs[lo_ramp] / lo2) / np.log2(a.lo_hz / lo2), 0, 1)
    taper[hi_ramp] = np.clip(np.log2(hi2 / freqs[hi_ramp]) / np.log2(hi2 / a.hi_hz), 0, 1)
    Cdb = db(C) * taper
    C = 10 ** (Cdb / 20)

    if span < a.flat_thresh:
        print(f"# NOTE: input is already DF-flat (span {span:.2f} dB); the correction below is a near-identity.")
    print(f"# correction applied (dB): min {Cdb.min():+.2f}  max {Cdb.max():+.2f}")

    Hc = minphase_response(C, N)                         # (F,) complex, length-preserving
    Hir = np.fft.rfft(ir, axis=2)                        # (M,R,F)
    corr = np.fft.irfft(Hir * Hc[None, None, :], n=N, axis=2)

    # verify the corrected diffuse field is flat
    Pc = df_average(corr, w)
    Pcdb = db(Pc) - db(Pc)[k1k]
    print(f"# corrected DF 100Hz-16kHz: mean {Pcdb[band].mean():+.2f}  span {Pcdb[band].max()-Pcdb[band].min():.2f} dB")

    shutil.copyfile(a.in_sofa, a.out)
    g = h5py.File(a.out, "r+")
    # Overwrite ONLY Data.IR. Do NOT touch any attribute: this is a netCDF4 file
    # (creation-order-tracked attributes), and h5py rewrites an attribute by
    # delete+recreate, which breaks that ordering so libmysofa rejects the file
    # with "unsupported format" (10001). Provenance lives in the output filename.
    g["Data.IR"][...] = corr.astype(g["Data.IR"].dtype)
    g.close()
    print(f"# wrote {a.out}")


if __name__ == "__main__":
    sys.exit(main())
