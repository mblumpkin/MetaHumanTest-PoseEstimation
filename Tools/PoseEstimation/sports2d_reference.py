"""Reference: what real Sports2D draws / computes for given keypoints, using Sports2D's and Pose2Sim's own functions
(pulled out of their source files with ast, so nothing is re-implemented here). Used to check the Unreal port's
angles (PoseSports2D) and overlay (CPP_UW_PoseOverlay).

Needs: Python with numpy, opencv-python, matplotlib, anytree; and a folder <repos_dir> with clones of
  https://github.com/davidpagnon/Sports2D  -> <repos_dir>/sports2d
  https://github.com/perfanalytics/pose2sim -> <repos_dir>/pose2sim
  https://github.com/Tau-J/rtmlib           -> <repos_dir>/rtmlib

  python -I sports2d_reference.py draw <repos_dir> <image.png> <unreal_raw.json> <out.png>
      Sports2D's real-time display (box, keypoints, skeleton, angles on body + list) for one frame.
  python -I sports2d_reference.py angles <repos_dir> <unreal_raw.json> <unreal.sports2d.json>
      For every frame: Sports2D's kept people, visible side and angles from Unreal's raw keypoints, compared with
      Unreal's own result.

unreal_raw.json: FPoseEstimator::ToJson output (Pose.Dump's .json, or the PoseEstimation commandlet's -Out file).
unreal.sports2d.json: Pose.Dump's .sports2d.json, or the commandlet's <Out>.sports2d.json.
visible_side is 'auto' for everyone (Unreal's default).
"""
import sys, ast, json, itertools as it
import numpy as np, cv2
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from anytree import Node, PreOrderIter

KPT_THR, AVG_THR, NUM_THR = 0.3, 0.5, 0.3
FONT_SIZE = 0.3
JOINT = ['Right ankle', 'Left ankle', 'Right knee', 'Left knee', 'Right hip', 'Left hip', 'Right shoulder', 'Left shoulder', 'Right elbow', 'Left elbow', 'Right wrist', 'Left wrist']
SEGMENT = ['Right foot', 'Left foot', 'Right shank', 'Left shank', 'Right thigh', 'Left thigh', 'Pelvis', 'Trunk', 'Shoulders', 'Head', 'Right arm', 'Left arm', 'Right forearm', 'Left forearm']
ANGLE_NAMES = [a.lower() for a in JOINT + SEGMENT]

def load_sports2d(repos):
    ns = {'np': np, 'cv2': cv2, 'it': it, 'plt': plt, 'PreOrderIter': PreOrderIter, 'Node': Node}
    def load(path, names):
        tree = ast.parse(open(path, encoding='utf-8').read())
        for node in tree.body:
            if (isinstance(node, ast.FunctionDef) and node.name in names) or \
               (isinstance(node, ast.Assign) and any(getattr(t, 'id', None) in names for t in node.targets)):
                exec(compile(ast.Module([node], []), path, 'exec'), ns)
    load(f'{repos}/pose2sim/Pose2Sim/common.py', {'angle_dict', 'colors', 'thickness', 'points_to_angles', 'fixed_angles',
         'draw_bounding_box', 'draw_skel', 'draw_keypts'})
    load(f'{repos}/pose2sim/Pose2Sim/skeletons.py', {'HALPE_26'})
    load(f'{repos}/rtmlib/rtmlib/tools/object_detection/post_processings.py', {'nms'})
    load(f'{repos}/sports2d/Sports2D/process.py', {'compute_angle', 'compute_angles_for_person', 'draw_dotted_line',
         'draw_angles', 'draw_segment_angle', 'draw_joint_angle', 'write_angle_on_body', 'write_angle_as_list'})
    model = ns['HALPE_26']
    nodes = [n for n in PreOrderIter(model) if n.id is not None]
    order = np.argsort([n.id for n in nodes])
    ns['keypoints_ids'] = [nodes[i].id for i in order]
    ns['keypoints_names'] = [nodes[i].name for i in order]
    ns['L_R'] = [ns['keypoints_ids'][ns['keypoints_names'].index(k)] for k in ('LBigToe', 'LHeel', 'RBigToe', 'RHeel')]
    return ns

def sports2d_frame(ns, frame_json):
    """process.py, one frame: pose-level NMS, likelihood filters, visible side + angles."""
    W, H = frame_json['width'], frame_json['height']
    keypoints = np.array([p['keypoints'] for p in frame_json['persons']]).reshape(-1, 26, 2)
    scores = np.array([p['scores'] for p in frame_json['persons']]).reshape(-1, 26)
    out = dict(X=[], Y=[], S=[], angles=[], Xf=[], sides=[])
    if len(keypoints) == 0:
        return out
    mask = np.mean(scores, axis=1) > 0.2
    lk = np.where(mask[:, None, None], keypoints, np.nan); ls = np.where(mask[:, None], scores, np.nan)
    bx = []
    for p in lk:
        if np.isnan(p).all(): bx.append([np.nan] * 4); continue
        bx.append([np.clip(np.nanmin(p[:, 0]), 0, W), np.clip(np.nanmin(p[:, 1]), 0, H), np.clip(np.nanmax(p[:, 0]), 0, W), np.clip(np.nanmax(p[:, 1]), 0, H)])
    bx = np.array(bx); sb = np.nanmean(ls, axis=1)
    vi = np.where(~np.isnan(sb))[0]
    keep = vi[ns['nms'](bx[vi], sb[vi], nms_thr=0.45)] if len(vi) else []
    for pi in keep:
        X, Y = np.where(ls[pi][:, None] < KPT_THR, np.nan, lk[pi]).T
        S = np.where(ls[pi] < KPT_THR, np.nan, ls[pi])
        good = S[~np.isnan(S)]
        if not (len(good) >= len(S) * NUM_THR) or not ((np.nanmean(good) if len(good) else 0) >= AVG_THR):
            X = np.full_like(X, np.nan); Y = np.full_like(Y, np.nan); S = np.full_like(S, np.nan)
        Xf, angs, vs = ns['compute_angles_for_person'](X, Y, 'auto', True, ns['keypoints_ids'], ns['keypoints_names'], ANGLE_NAMES, ns['angle_dict'], L_R_direction_idx=ns['L_R'])
        for k, v in [('X', X), ('Y', Y), ('S', S), ('angles', angs), ('Xf', Xf), ('sides', vs)]:
            out[k].append(v)
    return out

def draw(repos, img_path, raw_path, out_png):
    ns = load_sports2d(repos)
    d = json.load(open(raw_path)); d = d[0] if isinstance(d, list) else d
    f = sports2d_frame(ns, d)
    img = cv2.imread(img_path)
    th = 1 if FONT_SIZE < 0.8 else 2
    img = ns['draw_bounding_box'](img, f['X'], f['Y'], colors=ns['colors'], fontSize=FONT_SIZE, thickness=th)
    img = ns['draw_keypts'](img, f['X'], f['Y'], f['S'], cmap_str='RdYlGn')
    img = ns['draw_skel'](img, f['X'], f['Y'], ns['HALPE_26'])
    img = ns['draw_angles'](img, f['X'], f['Y'], f['angles'], f['Xf'], ns['keypoints_ids'], ns['keypoints_names'], ANGLE_NAMES,
                            display_angle_values_on=['body', 'list'], colors=ns['colors'], fontSize=FONT_SIZE, thickness=th, visible_sides=f['sides'])
    cv2.imwrite(out_png, img)
    for pi, (angs, s) in enumerate(zip(f['angles'], f['sides'])):
        print(f'person {pi} side {s}:', ', '.join(f'{n} {a:.1f}' for n, a in zip(ANGLE_NAMES, angs) if not np.isnan(a)) or 'no angles')

def angles(repos, raw_path, ue_path):
    ns = load_sports2d(repos)
    R = json.load(open(raw_path)); R = R if isinstance(R, list) else [R]
    U = json.load(open(ue_path)); U = U if isinstance(U, list) else [U]
    assert U[0]['angle_names'] == ANGLE_NAMES, U[0]['angle_names']
    diffs, n_angles, mismatches = [], 0, []
    for r, u in zip(R, U):
        f = sports2d_frame(ns, r)
        if len(f['angles']) != len(u['persons']):
            mismatches.append((r['name'], f'persons {len(f["angles"])} vs {len(u["persons"])}')); continue
        for pi, (angs, side, up) in enumerate(zip(f['angles'], f['sides'], u['persons'])):
            valid = not np.isnan(f['X'][pi]).all()
            if valid != up['valid']:
                mismatches.append((r['name'], pi, 'valid', valid, up['valid'])); continue
            if not valid:
                continue
            if side != up['visible_side']:
                mismatches.append((r['name'], pi, 'side', side, up['visible_side']))
            for name, a, b in zip(ANGLE_NAMES, angs, up['angles']):
                if np.isnan(a) != (b is None):
                    mismatches.append((r['name'], pi, name, a, b)); continue
                if b is not None:
                    n_angles += 1
                    diffs.append(abs(a - b) if abs(a - b) < 180 else 360 - abs(a - b))
    d = np.array(diffs)
    print(f'frames {len(R)}, angle values compared {n_angles}' + (f', max difference {d.max():.6f} deg, mean {d.mean():.6f}' if len(d) else ''))
    print('mismatches:', mismatches[:10] if mismatches else 'none')

if __name__ == '__main__':
    if sys.argv[1] == 'draw':
        draw(*sys.argv[2:6])
    elif sys.argv[1] == 'angles':
        angles(*sys.argv[2:5])
