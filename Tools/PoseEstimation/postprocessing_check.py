"""Checks the Unreal port of Sports2D's post-processing (Source/.../PoseSignal.cpp, PoseRecording.cpp) against the real
thing: Pose2Sim's own functions (interpolate_zeros_nans, indices_of_first_last_non_nan_chunks, hampel_filter,
butterworth_filter_1d, pulled out of its source with ast) and Sports2D's own lines for unwrapping and gap filling.

Needs: Python with numpy, scipy, pandas; <repos_dir>/pose2sim = a clone of https://github.com/perfanalytics/pose2sim

  python -I postprocessing_check.py make <signals.csv> <fps>
      Synthetic test signals (noise, short and long gaps, outlier spikes, zeros, short fragments, wrapping angles).
  UnrealEditor-Cmd.exe <project>/MetaHumanTest.uproject -run=PoseEstimation -SignalCsv=<signals.csv> -Out=<unreal.csv> -Fps=<fps> [-Angles] -unattended -nullrhi -nosplash
  python -I postprocessing_check.py ref <repos_dir> <signals.csv> <reference.csv> <fps> [angles]
      The same signals through Sports2D / Pose2Sim.
  python -I postprocessing_check.py compare <reference.csv> <unreal.csv>

  python -I postprocessing_check.py recording <repos_dir> <Name_angles.csv> <Name_Sports2D_px_person00.trc>
      For a recording exported by Unreal: re-runs Sports2D's chain on the raw angle columns of the CSV (at the frame
      rate written in the .trc) and compares with Unreal's filtered columns.
"""
import sys, ast, math
import numpy as np, pandas as pd
from scipy import signal, interpolate

def load_pose2sim(repos):
    ns = {'np': np, 'pd': pd, 'signal': signal, 'interpolate': interpolate}
    def load(path, names):
        tree = ast.parse(open(path, encoding='utf-8').read())
        for node in tree.body:
            if isinstance(node, ast.FunctionDef) and node.name in names:
                exec(compile(ast.Module([node], []), path, 'exec'), ns)
    load(f'{repos}/pose2sim/Pose2Sim/common.py', {'interpolate_zeros_nans'})
    load(f'{repos}/pose2sim/Pose2Sim/triangulation.py', {'indices_of_first_last_non_nan_chunks'})
    load(f'{repos}/pose2sim/Pose2Sim/filtering.py', {'hampel_filter', 'butterworth_filter_1d'})
    return ns

def sports2d_chain(ns, df, fps, angles, cutoff=6, order=4):
    """process.py, angle (or keypoint) post-processing for one person, default settings."""
    df = df.copy()
    if angles:   # np.unwrap(period=180) on valid values, then the mean shift
        for c in df.columns:
            m = ~np.isnan(df[c].values)
            if m.any():
                a = np.unwrap(df[c].values[m], period=180)
                a = a - 360 if a.mean() > 180 else a
                a = a + 360 if a.mean() < -180 else a
                vals = df[c].values.copy(); vals[m] = a; df[c] = vals
    interp = df.apply(ns['interpolate_zeros_nans'], axis=0, args=[100, 'linear'])
    starts, ends = [], []
    for c in df.columns:
        s, e = ns['indices_of_first_last_non_nan_chunks'](interp[c], min_chunk_size=10, chunk_choice_method='all')
        starts.append(s); ends.append(e)
    if (min(starts), max(ends)) == (0, 0):
        return df * np.nan
    for i, c in enumerate(interp.columns):   # fill_large_gaps_with = 'last_value'
        s, e = starts[i], ends[i]
        interp.loc[:s, c] = np.nan
        interp.loc[e:, c] = np.nan
        interp.loc[s:e, c] = interp.loc[s:e, c].ffill().bfill()
    window = 7 if angles else round(7 * fps / 30)
    hamp = interp.apply(ns['hampel_filter'], axis=0, args=[window, 2])
    if cutoff / (fps / 2) >= 1:   # Sports2D's rule for live (webcam) input
        cutoff = fps / (2 + 0.001)
    cfg = {'filtering': {'butterworth': {'order': order, 'cut_off_frequency': cutoff}}}
    return hamp.apply(lambda col: ns['butterworth_filter_1d'](cfg, fps, col), axis=0)

def make(path, fps):
    fps = float(fps); rng = np.random.default_rng(1)
    n = 600; t = np.arange(n) / fps
    cols = {}
    a = 30 * np.sin(2 * np.pi * 1.2 * t) + 5 * np.sin(2 * np.pi * 7 * t) + rng.normal(0, 2, n)
    a[[50, 51, 200, 410]] += [60, -45, 80, -70]               # outlier spikes
    a[100:130] = np.nan; a[300:420] = np.nan; a[500:504] = np.nan   # 30-frame gap, 120-frame gap, short gap
    cols['wave'] = a
    b = 400 + 120 * np.cos(2 * np.pi * 0.5 * t) + rng.normal(0, 1.5, n)
    b[:25] = np.nan; b[30:35] = np.nan; b[35:40] = 410; b[40:45] = np.nan; b[580:] = np.nan; b[250] = 0.0
    cols['position'] = b
    c = np.full(n, np.nan); c[100:107] = 50.0; c[200:290] = 20 + rng.normal(0, 1, 90)   # a too-short fragment + one run
    cols['fragment'] = c
    w = (170 + 30 * t + rng.normal(0, 1.0, n)); w = (w + 180) % 360 - 180   # crosses +-180: unwrapping matters
    w[60:70] = np.nan
    cols['wrapping'] = w
    pd.DataFrame(cols).to_csv(path, index=False, na_rep='nan')
    print(f'{n} rows x {len(cols)} columns at {fps} fps -> {path}')

def ref(repos, src, out, fps, angles=False):
    ns = load_pose2sim(repos)
    df = pd.read_csv(src)
    res = sports2d_chain(ns, df, float(fps), angles)
    res.to_csv(out, index=False, na_rep='nan')
    print(f'reference -> {out}')

def compare_frames(a, b, label=''):
    worst = 0.0; mismatch = 0; n = 0
    for c in a.columns:
        x, y = a[c].values.astype(float), b[c].values.astype(float)
        both = ~np.isnan(x) & ~np.isnan(y)
        mismatch += int(np.sum(np.isnan(x) != np.isnan(y)))
        if both.any():
            d = np.abs(x[both] - y[both]).max(); worst = max(worst, d)
            print(f'  {label}{c:30s} values {both.sum():5d}  max difference {d:.3e}  missing-value mismatches {int(np.sum(np.isnan(x) != np.isnan(y)))}')
        n += both.sum()
    print(f'TOTAL: {n} values compared, max difference {worst:.3e}, missing-value mismatches {mismatch}')

def compare(a, b):
    compare_frames(pd.read_csv(a), pd.read_csv(b))

def recording(repos, csv_path, trc_path):
    ns = load_pose2sim(repos)
    fps = float(open(trc_path).read().splitlines()[2].split('\t')[0])
    df = pd.read_csv(csv_path)
    names = [c for c in df.columns if c != 'time' and not c.endswith(' (raw)')]
    raw = pd.DataFrame({n: df[n + ' (raw)'].values for n in names})
    mine = pd.DataFrame({n: df[n].values for n in names})
    # The raw columns are already unwrapped by Unreal (before resampling); unwrapping again is a no-op on them.
    res = pd.DataFrame({n: sports2d_chain(ns, raw[[n]], fps, angles=True)[n].values for n in names})
    print(f'recording at {fps:.3f} fps, {len(df)} rows')
    compare_frames(res, mine)

if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'make': make(sys.argv[2], sys.argv[3])
    elif cmd == 'ref': ref(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], len(sys.argv) > 6 and sys.argv[6] == 'angles')
    elif cmd == 'compare': compare(sys.argv[2], sys.argv[3])
    elif cmd == 'recording': recording(sys.argv[2], sys.argv[3], sys.argv[4])
