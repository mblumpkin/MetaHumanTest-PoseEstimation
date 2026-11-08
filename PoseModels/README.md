# Pose estimation models

The two ONNX models that Sports2D uses in its default **"balanced"** mode with `pose_model = 'Body_with_feet'`
(HALPE_26 keypoints). They are the exact files RTMLib downloads, renamed after their zip (each zip contains an
`end2end.onnx`). Unreal loads them at runtime through the Neural Network Engine (`NNERuntimeORT` plugin, CPU), see
`Source/MetaHumanTest/*/PoseEstimation`. `MetaHumanTest.Build.cs` stages them with packaged builds.

| File | What | Input → output | Source zip | SHA-256 |
|---|---|---|---|---|
| `yolox_m_8xb8-300e_humanart-c2c7a14a.onnx` | YOLOX-m person detector (trained on COCO + Human-Art) | 1×3×640×640 BGR 0–255 → `dets` N×5 (x1, y1, x2, y2, score) + `labels` N; NMS is inside the model (keep top 100) | [yolox_m_8xb8-300e_humanart-c2c7a14a.zip](https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/yolox_m_8xb8-300e_humanart-c2c7a14a.zip) | `3dea6513388889f0fff4b77bf7a26013600321b9eb9ceb0e9a400a82572f5f23` |
| `rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192-4d3e73dd_20230605.onnx` | RTMPose-m, 26 keypoints (HALPE_26) | 1×3×256×192 normalised crop → `simcc_x` 1×26×384, `simcc_y` 1×26×512 | [rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192-4d3e73dd_20230605.zip](https://download.openmmlab.com/mmpose/v1/projects/rtmposev1/onnx_sdk/rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192-4d3e73dd_20230605.zip) | `26f3a19e61304a600dfb82d1001d41d24343b89fc70a33ffc84657e0b0bf2ecf` |

Both files are ONNX opset 11.

## Licences and credits

* Models: [OpenMMLab MMPose / RTMPose](https://github.com/open-mmlab/mmpose/tree/main/projects/rtmpose), Apache 2.0.
* The pre/post-processing and tracking in `PoseEstimator.cpp` are a C++ port of
  [RTMLib](https://github.com/Tau-J/rtmlib) (Apache 2.0); the person filtering and settings follow
  [Sports2D](https://github.com/davidpagnon/Sports2D) and [Pose2Sim](https://github.com/perfanalytics/pose2sim)
  (BSD 3-Clause), by David Pagnon et al.

## Using other models

Sports2D's "lightweight" pair (YOLOX-tiny 416×416 + RTMPose-s, ~40 MB) or "performance" pair (YOLOX-x + RTMPose-x 384×288)
should also work: the code reads the input sizes from the models. Put the `.onnx` files here and change
*Detector Model File* / *Pose Model File* on the `PoseEstimation` component of `BP_PlayerController`.
