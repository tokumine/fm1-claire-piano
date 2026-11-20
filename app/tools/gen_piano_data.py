#!/usr/bin/env python3
"""Generate piano_data.h with calibrated FM piano tables and a full LZ4-compressed
library of strictly public-domain classical, impressionist, baroque, and ragtime masterworks.
"""
import argparse
import json
import math
import os
import re
import struct
import sys
from pathlib import Path

SRC_PIANO = Path("/home/simon/fm/piano/clair-de-lune-fm")
MUTOPIA_ROOT = Path("/tmp/pianooo/public/Katalog")
QUELLEN_MD = MUTOPIA_ROOT / "QUELLEN.md"

# ---- Exact calibration tables from fm_piano.py ----
BAND_COEF = [
    [-17.874,   7.415,  -0.307, -11.864,   5.267,   3.442,   2.601,  -0.309],
    [-21.784, -14.463, -12.074,  -2.592,   7.359,   7.334,   4.130,  -0.785],
    [-30.330, -16.578, -22.342, -18.923,   9.941,  10.927,   4.623,  -1.504],
    [-41.618, -35.578, -33.636, -16.148,  15.597,  12.929,   2.646,  -2.421],
    [-73.258, -69.961,  -2.387,  24.287,  24.078,  24.448,   8.369,  -1.187],
    [ -3.437, 443.346, 823.950, 419.625,  13.986,  -2.467,   2.149,  -1.403],
]
BAND_MMAX = [99, 99, 93, 75, 63, 51]
BAND_COUNT = [1, 2, 4, 8, 16, 32]
BAND_CENTER = [math.sqrt(a * b) for a, b in ((1, 1), (2, 3), (4, 7), (8, 15), (16, 31), (32, 63))]
STRETCH_M = [21, 27, 33, 39, 45, 57, 69, 75, 81, 87, 93, 99, 108]
STRETCH_C = [-24, -17, -6.5, -4.5, -1.5, -0.6, 0.5, 1.5, 5.0, 7.8, 8.5, 11, 18]
STRIKE_M = [21, 57, 63, 69, 75, 108]
STRIKE_P = [8.4, 8.4, 9.0, 11.5, 14.0, 16.0]
SOFT_DB = [0.0, -1.5, -4.5, -7.5, -9.0, -9.0]

def interp1(x, xs, ys):
    if x <= xs[0]: return float(ys[0])
    if x >= xs[-1]: return float(ys[-1])
    for i in range(len(xs) - 1):
        if xs[i] <= x <= xs[i + 1]:
            t = (x - xs[i]) / (xs[i + 1] - xs[i])
            return float(ys[i]) * (1.0 - t) + float(ys[i + 1]) * t
    return float(ys[-1])

def bessel_j(k, x):
    N = 80
    s = 0.0
    for i in range(N):
        tau = (i + 0.5) * math.pi / N
        s += math.cos(k * tau - x * math.sin(tau))
    return s / N

def inharmonicity(m):
    return 2.2e-4 * (2.85 ** ((m - 57) / 12.0)) + 1.75e-4 * (2.0 ** (-(m - 27) / 4.5))

def fundamental(m):
    return 440.0 * (2.0 ** ((m - 69) / 12.0 + interp1(m, STRETCH_M, STRETCH_C) / 1200.0))

def strike_period(m):
    return interp1(m, STRIKE_M, STRIKE_P)

def decay_rates(m, band):
    r1 = 14.0 * (2.0 ** ((m - 45) / 20.0)) * (1.0 + 0.1 * band)
    r2 = (1.6 * (2.0 ** ((m - 40) / 26.0)) + 0.5 * (2.0 ** ((m - 70) / 9.0))) * (1.0 + 0.12 * band)
    s = max(0.0, min(1.0, (m - 40) / 30.0))
    after = -10.0 - 14.0 * (s * s * (3.0 - 2.0 * s))
    return r1, r2, after

def release_tau(m):
    return 0.14 * (2.0 ** (-(m - 28) / 36.0))

def feat(x, y):
    return [1.0, x, x * x, x ** 3, y, x * y, x * x * y, y * y]

def band_levels_at(b, mm, y):
    if mm <= BAND_MMAX[b]:
        f = feat((mm - 63.0) / 36.0, y)
        return sum(a * c for a, c in zip(f, BAND_COEF[b]))
    mx = BAND_MMAX[b]
    f_mx = feat((mx - 63.0) / 36.0, y)
    v_mx = sum(a * c for a, c in zip(f_mx, BAND_COEF[b]))
    return v_mx + band_levels_at(b - 1, mm, y) - band_levels_at(b - 1, mx, y) - 0.5 * (mm - mx)

def band_levels(m, layer):
    y = max(-1.3, min(1.17, (layer - 9.0) / 6.0))
    mc = max(27.0, min(99.0, float(m)))
    return [band_levels_at(b, mc, y) for b in range(6)]

def partial_levels_v5(m, layer, f1, B, nmax, dev_m, dev):
    """Exact fm_piano_v5.py design5() partial spectrum: band_levels + 7k/9k roll-offs + SOFT_DB + fine_structure."""
    Lb = band_levels(m, layer)
    Lb[0] += -28.0 / (1.0 + (f1 / 62.0) ** 12)
    per = [Lb[b] - 10.0 * math.log10(BAND_COUNT[b]) for b in range(6)]
    xc = [math.log2(c) for c in BAND_CENTER]
    last = min((per[5] - per[4]) / (xc[5] - xc[4]), -14.0)
    j = 0
    while j + 1 < len(dev_m) - 1 and dev_m[j + 1] < m:
        j += 1
    w = max(0.0, min(1.0, (m - dev_m[j]) / (dev_m[j + 1] - dev_m[j])))
    fine = [(1.0 - w) * dev[j][k] + w * dev[j + 1][k] if k < len(dev[j]) else 0.0 for k in range(nmax)]
    # Deterministic per-key personality matching fm_piano_v5.py seed 1905
    es, fs = [], []
    for idx in range(nmax):
        n = idx + 1.0
        x = math.log2(n)
        ev = interp1(x, xc, per) if x <= xc[5] else per[5] + last * (x - xc[5])
        fn = n * f1 * math.sqrt((1.0 + B * n * n) / (1.0 + B))
        if fn > 9000.0: ev -= 30.0 * math.log2(fn / 9000.0)
        if fn > 7000.0: ev -= 36.0 * math.log2(fn / 7000.0)
        soft_k = max(0.0, (6.0 - layer) / 3.0)
        if soft_k > 0.0:
            ev += interp1(x, xc, SOFT_DB) * soft_k * interp1(m, [45.0, 57.0], [0.2, 1.0])
        # Deterministic key personality (zero-mean, ~0.8 dB std)
        h = ((m * 1103515245 + idx * 12345 + 1905) & 0x7FFFFFFF) / 2147483647.0
        ev += fine[idx] + (h - 0.5) * 1.4
        es.append(ev)
        fs.append(fn)
    return es, fs

NPART = 16

def build_v5_reference_models():
    import numpy as np
    sys.path.insert(0, str(SRC_PIANO))
    import fm_piano_v5 as v5, dsp, dsp_v4, render_v5 as rv5
    piano = v5.LivingPiano()
    bir = dsp_v4.Structure().ir(None)
    nfft = 1 << 17
    pad_l = np.zeros(nfft, dtype=np.complex128); pad_l[:len(bir)] = bir[:, 0]
    pad_r = np.zeros(nfft, dtype=np.complex128); pad_r[:len(bir)] = bir[:, 1]
    Hl = np.fft.rfft(pad_l.real)
    Hr = np.fft.rfft(pad_r.real)
    f_grid = np.fft.rfftfreq(nfft, 1.0 / v5.FS)
    Hpow = np.abs(Hl) ** 2
    g_body = 10.0 ** (rv5.BODY_DB / 20.0) / float(np.sqrt(np.mean(Hpow[(f_grid > 100) & (f_grid < 4000)])))
    # Smooth soundboard power response over ~6 Hz (16 bins) matching real decaying partial linewidth
    raw_pow = 0.5 * (np.abs(1.0 + g_body * Hl) ** 2 + np.abs(1.0 + g_body * Hr) ** 2)
    kern = np.ones(17, dtype=np.float64) / 17.0
    H_body_mag = np.sqrt(np.convolve(raw_pow, kern, mode="same"))
    tilt_pts = rv5.r4.TILT
    tilt_mag = 10.0 ** (np.interp(np.log2(np.maximum(f_grid, 20.0)),
                                  np.log2([p[0] for p in tilt_pts]),
                                  [p[1] for p in tilt_pts]) / 20.0)
    H_tot = H_body_mag * tilt_mag
    return piano, f_grid, H_tot

def design_v5_key(m, piano, f_grid, H_tot, knock_p, knock_db, stoch_p, stoch_db):
    import numpy as np
    d_ref = piano.design5(m, 9.0, False)
    f1 = float(d_ref["f1"])
    B = inharmonicity(m)
    tau_a = interp1(m, [21, 45, 66, 84, 96, 108], [0.010, 0.009, 0.007, 0.003, 0.0018, 0.0012])
    tau_r = release_tau(m)
    spread = interp1(m, [21, 60, 96], [0.6, 0.8, 1.3])
    pan0 = float(d_ref["pan0"])

    pratios, pr1, pr2, pw2 = [], [], [], []
    P_ref = d_ref["p"]
    for k in range(NPART):
        if k < len(P_ref["f"]):
            pratios.append(float(P_ref["f"][k]) / f1)
            pr1.append(float(P_ref["r1"][k]))
            pr2.append(float(P_ref["r2"][k]))
            pw2.append(float(P_ref["w2"][k]))
        else:
            nn = k + 1.0
            fk = nn * f1 * math.sqrt((1.0 + B * nn * nn) / (1.0 + B))
            pratios.append(fk / f1)
            pr1.append(60.0)
            pr2.append(12.0)
            pw2.append(0.03)

    layers_amp, disp_amps, knock_amps, stoch_amps = [], [], [], []
    for ly in [3.0, 6.0, 9.0, 12.0, 15.0]:
        dl = piano.design5(m, ly, False)
        P = dl["p"]
        amp_map = {}
        up_pow = 0.0
        for j in range(len(P["n"])):
            pnum = int(round(float(P["n"][j]))) - 1
            fk = float(P["f"][j])
            if fk >= 19800.0:
                continue
            # Exact eigenfunction gain through 250-mode soundboard + listening-distance tilt!
            gain_tf = float(np.interp(fk, f_grid, H_tot))
            aj = float(P["amp"][j]) * gain_tf
            if pnum < NPART:
                amp_map[pnum] = aj
            else:
                up_pow += aj * aj
        layers_amp.append([amp_map.get(k, 0.0) for k in range(NPART)])
        disp_amps.append(math.sqrt(up_pow))
        tone = float(dl["tone"])
        kdb = interp1(m, knock_p, knock_db) + interp1(ly, [3.0, 9.0, 15.0], [3.0, 0.0, -3.0])
        knock_amps.append(tone * (10.0 ** (kdb / 20.0)))
        sdb = interp1(m, stoch_p, stoch_db)
        stoch_amps.append(tone * (10.0 ** (sdb / 20.0)))

    return dict(f1=f1, B=B, tau_a=tau_a, tau_r=tau_r, det_cents=0.65 * spread, pan0=pan0,
                pratios=pratios, pr1=pr1, pr2=pr2, pw2=pw2,
                layers_amp=layers_amp, disp_amps=disp_amps, knock_amps=knock_amps, stoch_amps=stoch_amps)

def lz4_compress_raw(data: bytes) -> bytes:
    n = len(data)
    if n == 0:
        return b""
    out = bytearray()
    table = {}
    anchor = 0
    pos = 0
    limit = n - 5
    while pos < limit:
        h = (data[pos] | (data[pos+1] << 8) | (data[pos+2] << 16) | (data[pos+3] << 24))
        ref = table.get(h, -1)
        table[h] = pos
        if ref >= 0 and pos - ref <= 65535 and data[ref:ref+4] == data[pos:pos+4]:
            # Extend match forward
            ml = 4
            while pos + ml < n and data[ref + ml] == data[pos + ml]:
                ml += 1
            lit_len = pos - anchor
            m_tok = min(15, ml - 4)
            l_tok = min(15, lit_len)
            out.append((l_tok << 4) | m_tok)
            if lit_len >= 15:
                rem = lit_len - 15
                while rem >= 255:
                    out.append(255); rem -= 255
                out.append(rem)
            out += data[anchor:pos]
            out += struct.pack("<H", pos - ref)
            if ml - 4 >= 15:
                rem = (ml - 4) - 15
                while rem >= 255:
                    out.append(255); rem -= 255
                out.append(rem)
            pos += ml
            anchor = pos
        else:
            pos += 1
    # Final literal run
    lit_len = n - anchor
    out.append(min(15, lit_len) << 4)
    if lit_len >= 15:
        rem = lit_len - 15
        while rem >= 255:
            out.append(255); rem -= 255
        out.append(rem)
    out += data[anchor:n]
    return bytes(out)

def lz4_decompress_verify(src: bytes, expected_len: int) -> bytes:
    dst = bytearray()
    i = 0
    n = len(src)
    while i < n:
        tok = src[i]; i += 1
        ll = tok >> 4
        if ll == 15:
            while True:
                b = src[i]; i += 1; ll += b
                if b != 255: break
        dst += src[i:i+ll]; i += ll
        if i >= n: break
        off = src[i] | (src[i+1] << 8); i += 2
        ml = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                b = src[i]; i += 1; ml += b
                if b != 255: break
        start = len(dst) - off
        for k in range(ml):
            dst.append(dst[start + k])
    assert len(dst) == expected_len, f"lz4 mismatch {len(dst)} != {expected_len}"
    return bytes(dst)

# ---- Key detection (Krumhansl-Schmuckler) ----
MAJ_PROF = [6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88]
MIN_PROF = [6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17]

def detect_key(notes):
    pc = [0.0] * 12
    for t_on, dur, p, vel, role, bar in notes:
        if 21 <= p <= 108:
            pc[p % 12] += max(0.1, dur) * (2.0 if role == 3 else 1.0)
    best_sc, best_root, best_min = -1e9, 0, 0
    for r in range(12):
        sm = sum(pc[(r + k) % 12] * MAJ_PROF[k] for k in range(12))
        if sm > best_sc: best_sc, best_root, best_min = sm, r, 0
        sn = sum(pc[(r + k) % 12] * MIN_PROF[k] for k in range(12))
        if sn > best_sc: best_sc, best_root, best_min = sn, r, 1
    return best_root, best_min

def pack_piece_blob(notes, pedal):
    """Expand dt > 2550ms into rest events, clamp to <= 7600 notes, transpose & LZ4-compress."""
    expanded = []
    prev_t = 0.0
    for t_on, dur, p, vel, role, bar in notes:
        if len(expanded) >= 7550:
            break
        if not (21 <= p <= 108):
            continue
        dt_10 = max(0, round((t_on - prev_t) * 100.0))
        prev_t = t_on
        while dt_10 > 255 and len(expanded) < 7540:
            expanded.append((255, 0, 0, 0)) # silent rest
            dt_10 -= 255
        dur_20 = max(1, min(255, round(dur * 50.0)))
        soft = 1 if (role == 5 and vel < 48) else 0
        p_byte = (p & 127) | (soft << 7)
        v_q = max(4, min(31, vel >> 2))
        vr_byte = (role & 7) | (v_q << 3)
        expanded.append((dt_10, dur_20, p_byte, vr_byte))

    n = len(expanded)
    dts = bytes(x[0] for x in expanded)
    durs = bytes(x[1] for x in expanded)
    ps = bytes(x[2] for x in expanded)
    vrs = bytes(x[3] for x in expanded)

    # Pedal spans (cap at 240)
    ped_clamped = pedal[:240]
    pbytes = bytearray()
    for a, b in ped_clamped:
        da = max(0, min(65535, round(a * 100.0)))
        db = max(da + 1, min(65535, round(b * 100.0)))
        pbytes += struct.pack("<HH", da, db)

    raw = dts + durs + ps + vrs + bytes(pbytes)
    comp = lz4_compress_raw(raw)
    assert lz4_decompress_verify(comp, len(raw)) == raw
    dur_10ms = min(65535, round((notes[-1][0] + notes[-1][1]) * 100.0)) if notes else 0
    return n, len(ped_clamped), dur_10ms, comp

def clean_title(s):
    s = re.sub(r"^\d+\s+", "", s)
    s = s.replace("_", " ").replace(".mid", "").replace("–", "-").replace("—", "-")
    # ASCII transliteration for German/French titles
    rep = {"ä":"a","ö":"o","ü":"u","Ä":"A","Ö":"O","Ü":"U","ß":"ss","é":"e","è":"e","ê":"e","à":"a","â":"a","ç":"c","Å":"A","å":"a","æ":"ae","ø":"o","–":"-"}
    for k, v in rep.items(): s = s.replace(k, v)
    s = re.sub(r"\s+", " ", s).strip().upper()
    return s[:22]

def load_midi_notes(mid_path, bpm_scale=1.0, max_notes=7500, quant_ms=0):
    sys.path.insert(0, str(SRC_PIANO / "pipeline"))
    import midifile
    m = midifile.read(str(mid_path))
    D = m["division"] or 480
    tempi = m["tempi"] or [(0, 120.0)]
    def tick_to_s(tk):
        s = 0.0; pt = 0; pb = tempi[0][1] * bpm_scale
        for te, be in tempi:
            if tk <= te: break
            s += (te - pt) / D * (60.0 / pb)
            pt = te; pb = be * bpm_scale
        return s + (tk - pt) / D * (60.0 / pb)

    raw_notes = sorted(m["notes"], key=lambda x: (x["on"], x["p"]))[:max_notes]
    if not raw_notes:
        return [], []
    by_tick = {}
    for n in raw_notes:
        t0 = tick_to_s(n["on"])
        tk = round(t0 / 0.035)
        by_tick.setdefault(tk, []).append(n)
    out = []
    t_shift = 0.5 - tick_to_s(raw_notes[0]["on"])
    for n in raw_notes:
        t_on = tick_to_s(n["on"]) + t_shift
        t_off = tick_to_s(n["off"]) + t_shift
        if quant_ms > 0:
            t_on = round(t_on * 1000.0 / quant_ms) * (quant_ms / 1000.0)
        dur = max(0.10, min(5.0, t_off - t_on))
        tk = round((tick_to_s(n["on"])) / 0.035)
        grp = by_tick[tk]
        p = n["p"]
        if not (21 <= p <= 108): continue
        max_p = max(o["p"] for o in grp)
        min_p = min(o["p"] for o in grp)
        if p == max_p and p >= 58: role = 0
        elif p == min_p and p < 60: role = 3
        else: role = 5
        vel = max(24, min(118, int(n["vel"])))
        bar = 1 + int(t_on / 2.2)
        out.append([round(t_on, 3), round(dur, 3), p, vel, role, bar])
    out.sort(key=lambda x: (x[0], x[2]))
    # Auto-generate clean harmonic pedal spans (every ~2.2s, lift 140ms)
    total_s = out[-1][0] + out[-1][1]
    ped = []
    t = 0.45
    while t < total_s and len(ped) < 220:
        ped.append((t, t + 2.05))
        t += 2.20
    return out, ped

def load_mutopia_public_domain_set():
    pd = set()
    if not QUELLEN_MD.exists():
        return pd
    for line in QUELLEN_MD.read_text(encoding="utf-8").splitlines():
        if line.startswith("|") and ".mid |" in line:
            parts = [p.strip() for p in line.split("|")[1:-1]]
            if len(parts) >= 5 and parts[2] == "Public Domain":
                folder, fname = parts[0].split(" / ", 1)
                pd.add((folder, fname))
    return pd

def classify_composer(comp, title):
    u = (comp + " " + title).upper()
    if "DEBUSSY" in u or "SATIE" in u: return ("DEBUSSY", 1)
    if "CHOPIN" in u: return ("CHOPIN", 2)
    if "BEETHOVEN" in u: return ("BEETHOVEN", 3)
    if "BACH" in u or "PETZOLD" in u or "HANDEL" in u or "CORELLI" in u or "VIVALDI" in u or "RAMEAU" in u: return ("BAROQUE", 4)
    if "MOZART" in u or "HAYDN" in u or "CLEMENTI" in u or "KUHLAU" in u or "CZERNY" in u: return ("CLASSICAL", 5)
    if "JOPLIN" in u or "TURPIN" in u or "STRAUSS" in u or "BIZET" in u: return ("RAGTIME", 7)
    return ("ROMANTIC", 6)
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    partials = json.load(open(SRC_PIANO / "piano_partials.json"))
    knock = json.load(open(SRC_PIANO / "knock_fit.json"))
    stoch = json.load(open(SRC_PIANO / "stoch_fit.json"))
    struct_fit = json.load(open(SRC_PIANO / "structure_fit.json"))
    dev_m, dev = partials["pitches"], partials["deviation_db"]
    knock_p, knock_db = knock["pitches"], knock["dB"]
    stoch_p, stoch_db = stoch["pitches"], stoch["dB"]

    # Extract top 16 measured soundboard/bridge modes >= 65 Hz from structure_fit.json
    speaks = [(f, db) for f, db in struct_fit["peaks"] if 65.0 <= f <= 2600.0]
    speaks.sort(key=lambda x: x[1], reverse=True)
    speaks = sorted(speaks[:16], key=lambda x: x[0])

    lines = [
        "/* Generated by tools/gen_piano_data.py */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        "#define PIANO_KEYS 88",
        "#define PIANO_M_MIN 21",
        "#define PIANO_M_MAX 108",
        "#define PIANO_NLAYERS 5",
        "#define PIANO_NPART 16",
        "#define PIANO_NSTRUCT 16",
        "",
        "typedef struct {",
        "    float f1, B, tau_a, tau_r, det_ratio, pan;",
        "    float pratio[PIANO_NPART];",
        "    float pr1[PIANO_NPART];",
        "    float pr2[PIANO_NPART];",
        "    float pw2[PIANO_NPART];",
        "} piano_key_t;",
        "",
        "typedef struct {",
        "    float amp[PIANO_NPART];",
        "    float disp_amp, knock_amp, stoch_amp;",
        "} piano_layer_t;",
        "",
        "typedef struct { float freq, t60, weight, cycles, theta; } struct_mode_t;",
        "",
    ]

    s_rows = []
    for idx, (sf, sdb) in enumerate(speaks):
        t60 = min(110.0 / sf, 0.35)
        wgt = 0.7 * (10.0 ** ((min(sdb, 0.0) + 10.0) / 20.0)) * min(1.0, (700.0 / sf) ** 1.5)
        cyc = 0.21 * math.sqrt(sf) * (0.85 + 0.30 * (((idx * 37) % 11) / 10.0))
        th = ((idx * 97 + 19) % 100) * (2.0 * math.pi / 100.0)
        s_rows.append(f"    {{{sf:.2f}f,{t60:.4f}f,{wgt:.5f}f,{cyc:.3f}f,{th:.4f}f}}")
    lines.append("static const struct_mode_t STRUCT_MODES[PIANO_NSTRUCT] = {\n" + ",\n".join(s_rows) + "\n};")
    lines.append("")

    piano_ref, f_grid, H_tot = build_v5_reference_models()
    key_rows, layer_rows = [], []
    for m in range(21, 109):
        d = design_v5_key(m, piano_ref, f_grid, H_tot, knock_p, knock_db, stoch_p, stoch_db)
        det_ratio = 2.0 ** (d["det_cents"] / 1200.0)
        prat = ",".join(f"{x:.5f}f" for x in d["pratios"])
        pr1s = ",".join(f"{x:.3f}f" for x in d["pr1"])
        pr2s = ",".join(f"{x:.3f}f" for x in d["pr2"])
        pw2s = ",".join(f"{x:.4f}f" for x in d["pw2"])
        key_rows.append(f"    {{{d['f1']:.4f}f,{d['B']:.7f}f,{d['tau_a']:.5f}f,{d['tau_r']:.4f}f,{det_ratio:.7f}f,{d['pan0']:.4f}f,{{{prat}}},{{{pr1s}}},{{{pr2s}}},{{{pw2s}}}}}")
        lr = []
        for li in range(5):
            amps = ",".join(f"{x:.6f}f" for x in d["layers_amp"][li])
            lr.append(f"{{{{{amps}}},{d['disp_amps'][li]:.6f}f,{d['knock_amps'][li]:.6f}f,{d['stoch_amps'][li]:.6f}f}}")
        layer_rows.append("    {" + ",".join(lr) + "}")

    lines.append("static const piano_key_t PIANO_KEY[PIANO_KEYS] = {\n" + ",\n".join(key_rows) + "\n};")
    lines.append("static const piano_layer_t PIANO_LAYER[PIANO_KEYS][PIANO_NLAYERS] = {\n" + ",\n".join(layer_rows) + "\n};")
    lines.append("")
    # Build catalog of strictly Public Domain pieces within a 338 KB compressed budget
    pieces = [] # (title, composer, cat_id, notes, pedal, soft_mode)

    # 0. Clair de lune (Debussy) - exact calibrated v5 (Mutopia public-domain edition)
    cdl_ev = json.load(open(SRC_PIANO / "events_v5.json"))
    pieces.append(("CLAIR DE LUNE", "DEBUSSY", 1, cdl_ev["notes"], cdl_ev["pedal"], 1))

    # 1. Original & public-domain scores from clair-de-lune-fm/pipeline/pieces
    sys.path.insert(0, str(SRC_PIANO / "pipeline"))
    sys.path.insert(0, str(SRC_PIANO / "pipeline/pieces"))
    import pianist as cdl_pianist
    role_map = {"mel": 0, "mel2": 1, "cm": 2, "bass": 3, "oct": 4, "inner": 5}
    for mod_name, title, comp, cid, soft in [
        ("op109_1", "SONATA OP 109 I", "BEETHOVEN", 3, 0),
        ("op109_2", "SONATA OP 109 II", "BEETHOVEN", 3, 0),
        ("afterlight", "AFTERLIGHT", "ORIGINAL", 1, 1),
    ]:
        mod = __import__(mod_name)
        ev_list, ped_list, _, _, _ = cdl_pianist.perform(mod)
        ev_notes = [
            [round(e["t_on"], 3), round(max(0.10, min(5.0, e["key_up"] - e["t_on"])), 3),
             int(e["m"]), max(24, min(118, int(round(e["vel"])))),
             role_map.get(e["role"], 5), int(e["bar"])]
            for e in ev_list if 21 <= int(e["m"]) <= 108
        ]
        ev_ped = [(round(float(a), 3), round(float(b), 3)) for a, b in ped_list]
        pieces.append((title, comp, cid, ev_notes, ev_ped, soft))

    # 2. Strictly Public Domain transcriptions from Mutopia Katalog (verified via QUELLEN.md)
    pd_set = load_mutopia_public_domain_set()
    seen_titles = {p[0] for p in pieces}
    mut_dirs = ("5 ", "6 ", "7 ", "3 ", "4 ")
    mut_files = [p for prefix in mut_dirs for p in sorted(MUTOPIA_ROOT.glob(f"{prefix}*/*.mid"))]
    for path in mut_files:
        if (path.parent.name, path.name) not in pd_set:
            continue
        stem = path.stem
        if "clair de lune" in stem.lower():
            continue
        raw_name = re.sub(r"^\d+\s+", "", stem)
        if " - " in raw_name:
            comp_raw, tit_raw = raw_name.split(" - ", 1)
        else:
            comp_raw, tit_raw = "CLASSICAL", raw_name
        comp = clean_title(comp_raw)[:16]
        tit = clean_title(tit_raw)
        if tit in seen_titles:
            continue
        n, p = load_midi_notes(path)
        if not n:
            continue
        seen_titles.add(tit)
        _, cid = classify_composer(comp, tit)
        pieces.append((tit, comp, cid, n, p, 0))
    # Pack blobs respecting 340 KB budget
    BLOB_BUDGET = 338 * 1024
    blob = bytearray()
    entries = []
    for title, comp, cid, notes, pedal, soft_mode in pieces:
        nn, np, dur10, comp_bytes = pack_piece_blob(notes, pedal)
        if len(blob) + len(comp_bytes) > BLOB_BUDGET:
            continue
        kt, km = detect_key(notes)
        off = len(blob)
        blob += comp_bytes
        entries.append((title, comp, cid, kt, km, soft_mode, nn, np, dur10, off, len(comp_bytes)))

    print(f"Packed {len(entries)} pieces into {len(blob)} bytes ({len(blob)/1024:.1f} KB)")
    cats = {}
    for e in entries: cats[e[2]] = cats.get(e[2], 0) + 1
    print("Categories count:", cats)

    lines.append("#define SCORE_NPIECES " + str(len(entries)) + "u")
    lines.append("typedef struct {")
    lines.append("    const char *title;")
    lines.append("    const char *composer;")
    lines.append("    uint8_t cat, tonic, minor, soft;")
    lines.append("    uint16_t nnotes, npedal, dur_10ms, pad;")
    lines.append("    uint32_t off, clen;")
    lines.append("} score_meta_t;")
    lines.append("")
    lines.append("static const score_meta_t SCORE_CATALOG[SCORE_NPIECES] = {")
    for t, c, cid, kt, km, sm, nn, np, d10, off, clen in entries:
        lines.append(f'    {{"{t}", "{c}", {cid}u, {kt}u, {km}u, {sm}u, {nn}u, {np}u, {d10}u, 0u, {off}u, {clen}u}},')
    lines.append("};")
    lines.append("")
    lines.append(f"static const uint8_t SCORE_BLOB[{len(blob)}] = {{")
    for i in range(0, len(blob), 24):
        lines.append("    " + ",".join(str(b) for b in blob[i:i+24]) + ",")
    lines.append("};")

    Path(args.out).write_text("\n".join(lines) + "\n")
    print("Wrote", args.out)

if __name__ == "__main__":
    main()
