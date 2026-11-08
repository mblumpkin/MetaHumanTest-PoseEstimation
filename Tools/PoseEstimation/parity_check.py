"""Parity test: RTMLib (as Sports2D sets it up) vs the Unreal port (Source/MetaHumanTest/*/PoseEstimation), on the same PNG frames.

Needs Python with: pip install rtmlib onnxruntime opencv-python numpy
Unreal side (editor closed):
  UnrealEditor-Cmd.exe <project>/MetaHumanTest.uproject -run=PoseEstimation -Images=<png or dir> -Out=<unreal.json> -unattended -nullrhi -nosplash
  (exit code 1 is expected: unrelated engine start-up errors; check "LogPoseEstimation" lines in the log)

  python -I parity_check.py frames <video> <out_dir> [count] [step]      extract PNG frames from a video
  python -I parity_check.py ref <png or dir> <out.json> [det_frequency]  run RTMLib PoseTracker, same JSON layout as Unreal
  python -I parity_check.py compare <ref.json> <unreal.json>             compare
"""
import sys, os, json, glob
import numpy as np
import cv2

MODELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "PoseModels")
DET = os.path.join(MODELS, "yolox_m_8xb8-300e_humanart-c2c7a14a.onnx")
POSE = os.path.join(MODELS, "rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192-4d3e73dd_20230605.onnx")

def frames(video, out_dir, count=40, step=3):
    os.makedirs(out_dir, exist_ok=True)
    cap = cv2.VideoCapture(video)
    i = n = 0
    while n < count:
        ok, f = cap.read()
        if not ok:
            break
        if i % step == 0:
            cv2.imwrite(os.path.join(out_dir, f"frame_{n:04d}.png"), f)
            n += 1
        i += 1
    print(f"{n} frames -> {out_dir}")

def ref(src, out, det_frequency=4):
    from functools import partial
    from rtmlib import BodyWithFeet, PoseTracker
    files = sorted(glob.glob(os.path.join(src, "*.png"))) if os.path.isdir(src) else [src]
    solution = partial(BodyWithFeet, det=DET, det_input_size=(640, 640), pose=POSE, pose_input_size=(192, 256))
    # Sports2D: setup_pose_tracker(ModelClass, det_frequency, mode, tracking=False, backend, device)
    tracker = PoseTracker(solution, det_frequency=det_frequency, tracking=False,
                          backend="onnxruntime", device="cpu")
    results = []
    for f in files:
        img = cv2.imread(f)
        det_ran = tracker.frame_cnt % tracker.det_frequency == 0
        if det_ran:
            dets = tracker.det_model(img)
        bboxes = (list(dets) if det_ran else list(tracker.bboxes_last_frame))
        kpts, scores = tracker(img)
        # boxes the pose model actually used (same rule as RTMPose.__call__)
        used = bboxes if len(bboxes) else [[0, 0, img.shape[1], img.shape[0]]]
        results.append({
            "name": os.path.basename(f), "width": img.shape[1], "height": img.shape[0], "detector_ran": det_ran,
            "detections": [list(map(float, b)) for b in (dets if det_ran else [])],
            "persons": [{"box": list(map(float, used[p])), "keypoints": kpts[p].tolist(), "scores": scores[p].tolist()}
                        for p in range(len(kpts))],
        })
    with open(out, "w") as fh:
        json.dump(results, fh)
    print(f"{len(files)} image(s) -> {out}")

def load(path):
    d = json.load(open(path))
    return d if isinstance(d, list) else [d]

def compare(ref_path, ue_path):
    R, U = load(ref_path), load(ue_path)
    assert len(R) == len(U), f"{len(R)} vs {len(U)} frames"
    all_kp, all_sc, all_box = [], [], []
    worst = []
    for r, u in zip(R, U):
        assert r["name"] == u["name"], (r["name"], u["name"])
        if len(r["persons"]) != len(u["persons"]) or r["detector_ran"] != u["detector_ran"] or len(r["detections"]) != len(u["detections"]):
            print(f'  {r["name"]}: MISMATCH persons {len(r["persons"])} vs {len(u["persons"])}, '
                  f'detections {len(r["detections"])} vs {len(u["detections"])}, detector {r["detector_ran"]} vs {u["detector_ran"]}')
            continue
        for a, b in zip(r["detections"], u["detections"]):
            all_box.append(np.abs(np.array(a) - np.array(b)).max())
        for pr, pu in zip(r["persons"], u["persons"]):
            kr, ku = np.array(pr["keypoints"]), np.array(pu["keypoints"])
            d = np.linalg.norm(kr - ku, axis=1)
            all_kp.extend(d.tolist())
            all_sc.extend(np.abs(np.array(pr["scores"]) - np.array(pu["scores"])).tolist())
            worst.append((d.max(), r["name"], int(d.argmax())))
    kp = np.array(all_kp)
    print(f"frames {len(R)}, keypoints compared {kp.size}")
    if kp.size:
        print(f"keypoint distance px: mean {kp.mean():.4f}, median {np.median(kp):.4f}, max {kp.max():.3f}; "
              f"exact (<0.01px) {np.mean(kp < 0.01) * 100:.1f}%, within 1px {np.mean(kp <= 1.0) * 100:.1f}%, within 3px {np.mean(kp <= 3.0) * 100:.1f}%")
        sc = np.array(all_sc)
        print(f"score diff: mean {sc.mean():.5f}, max {sc.max():.5f}")
    if all_box:
        print(f"detector box diff px: max {max(all_box):.4f}")
    worst.sort(reverse=True)
    print("worst:", [(f"{w[0]:.2f}px", w[1], w[2]) for w in worst[:5]])

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "frames":
        frames(sys.argv[2], sys.argv[3], *(int(x) for x in sys.argv[4:6]))
    elif cmd == "ref":
        ref(sys.argv[2], sys.argv[3], *(int(x) for x in sys.argv[4:5]))
    elif cmd == "compare":
        compare(sys.argv[2], sys.argv[3])
