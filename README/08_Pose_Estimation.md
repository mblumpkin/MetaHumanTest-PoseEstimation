# 08 — Pose estimation (Sports2D inside Unreal)

The goal: a **Sports2D-style pose estimation** that runs inside the project. It finds the person in the 3D view, draws their
skeleton over the MetaHuman, and shows the joint-angle data in its own panel. Two round buttons on the right of the
screen (styled like the layer switcher) will switch it on: **Pose Overlay** on top, **Pose Data** underneath.

[Sports2D](https://github.com/davidpagnon/Sports2D) is the outside tool the team used until now. This is a rebuild of
it in C++: **the same two AI models, the same image preparation and the same rules**, so the numbers match what
Sports2D gets from the same picture.

![The Sports2D-style overlay in Play-In-Editor](images/pose_03_overlay_in_play.png)
*Play-In-Editor with the overlay on (phase 2): box, keypoints, skeleton, angle arcs and values on the body, and the list
of all angles on the left. The anatomy skeleton on the right is also detected as a person, but Sports2D's rules reject
it, so nothing is drawn on it.*

- [Status](#status)
- [Try it](#try-it)
- [How it works](#how-it-works)
- [The files](#the-files)
- [Settings](#settings)
- [How we know it matches Sports2D](#how-we-know-it-matches-sports2d)
- [Speed](#speed)
- [Things to know](#things-to-know)
- [Changes outside the new folder](#changes-outside-the-new-folder)
- [Work log](#work-log)
- [Source code on GitHub](#source-code-on-github)
- [Credits and licences](#credits-and-licences)

---

## Status

| Phase | What | State |
|---|---|---|
| 1 | Models running in Unreal on the live view + basic dot overlay, checked against Sports2D | **Done** (2026-11-08) |
| 2 | Sports2D-style overlay: skeleton lines, confidence colours, angle arcs on the joints, list of angle values; the 26 angles | **Done** (2026-11-08): drawing and angles match Sports2D; steady ~11 frames analysed per second |
| 3 | Angles over time: filtering (Butterworth 6 Hz), data panel with graphs, export (CSV + Sports2D's `.trc` / `.mot`) | To do |
| 4 | The two round buttons on the right (Pose Overlay, Pose Data) | To do |

Decisions so far:
* **Image route.** The models look at the rendered picture, like Sports2D looks at a video. They do *not* read the
  MetaHuman's bones. This keeps Sports2D's behaviour, including its occasional mistakes.
* **"Balanced" models**, Sports2D's default (YOLOX-m + RTMPose-m, 26 keypoints "HALPE_26").
* **CPU only**, so it also runs on Mac. A faster Windows-only GPU option can be added later.
* **Live**: it analyses the view while the animation plays.

---

## Try it

There are no buttons yet (phase 4). Use the console:

1. Press **Play**, click in the game view and press **`** (the key left of 1) to open the console. The **Cmd** box at
   the bottom of the Output Log also works.
2. `Pose.Enable 1`. The models load in the background (about 1–2 s the first time), then coloured dots appear on the
   MetaHuman.

| Command | Does |
|---|---|
| `Pose.Enable 1` / `0` (no number = toggle) | Start / stop analysing the view |
| `Pose.Overlay 1` / `0` | Show / hide the dots (the analysis keeps running) |
| `Pose.Stats` | Writes one line to the Output Log, averaged over the last 3 s: frames analysed per second and the game's fps, image size, time in each model, where the rest of the time goes (copying the frame, waiting, analysing), copy timeouts, people found, and for the main person: valid keypoints, mean score, the side facing the camera, number of angles |
| `Pose.Dump` | Saves the next analysed frame in `Saved/PoseEstimation/`: the image (PNG), the keypoints (JSON), the people kept + angles (`.sports2d.json`) and a screenshot with the overlay (`_screen.png`). The first screenshot can freeze the game for a few seconds (Unreal compiles a few shaders for it) |
| `Pose.TestImage <file.png> [out.json]` | Runs both models on an image file (Play not needed) and writes the keypoints as JSON |

If no result arrives for 1.5 s while it's on, a line starting *"Pose estimation: no result for"* appears in the Output
Log, describing what each stage is doing at that moment (the *watchdog*). Send that line to whoever is debugging.

**Hands-free test** (editor closed): run the game on its own with pose estimation switched on, stats logged every 5 s,
quitting after 75 s, all written to `Saved/Logs/PoseAutoTest.log`:
```
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor.exe" <project>/MetaHumanTest.uproject /Game/Main -game -windowed -PoseAutoEnable -PoseLogStats=5 -PoseQuitAfter=75 -log=PoseAutoTest.log
```
The three `-Pose…` switches do nothing unless they're on the command line.

From C++ or Blueprint: the player controller's **Pose Estimation** component (`GetPoseEstimation()`) has
**Set Estimation Enabled**, **Set Overlay Visible**, **Get Latest Result**, **Get Stats Text** and the event
**On Pose Estimated**, which fires after every analysed frame with all the keypoints.

---

## How it works

```
 game thread                 render thread                         worker thread
 ───────────                 ─────────────                         ─────────────
 PoseEstimationComp ──1──►  PoseFrameGrabber: copy the finished
 "I want the next frame"     3D image (before the HUD is drawn)
                             to a CPU-readable texture
                     ◄──2──  pixels ready
 launch analysis ────────────────────────────────────────────3──►  PoseEstimator:
                                                                    a) YOLOX finds people (every 4th frame)
                                                                    b) RTMPose finds 26 keypoints per person
                                                                    c) Sports2D's rules keep / reject people,
                                                                       which way they face, their 26 angles
 overlay + OnPoseEstimated ◄─────────────────────────────────4───  result
```

One frame is analysed at a time, on a thread of its own (`PoseInferenceThread`), so the game never waits for the
models. While they work, the next frame is copied, timed to be ready just as they finish (see [Speed](#speed)).

### 1. Getting the picture: `PoseFrameGrabber`
Sports2D reads frames from a video. Here the frame is **the image the player is looking at**. When a frame is
requested, a hook in the renderer (a *scene view extension*) copies the finished 3D image of the game viewport into a
small CPU-readable texture. A few frames later, once the GPU is done, the pixels are handed over.
* It is copied **after** lighting, exposure and anti-aliasing, and **before** the UMG HUD is drawn on top. So the
  models see exactly what you see, without the menus and without our own dots.
* Why not a second camera ("scene capture")? That would render the MetaHuman scene a second time (expensive), and the
  hair and anti-aliasing would look different from the screen.
* The copy costs nothing when pose estimation is off: the hook only switches on for the frame that was requested.

### 2. Finding the person: YOLOX-m (`FPoseEstimator::Detect`)
The same steps as RTMLib (the library Sports2D uses):
1. Shrink the image to fit 640 × 640, keeping its shape, top-left aligned. Fill the rest with grey (114).
2. Run the model. It returns boxes around people, each with a score.
3. Keep boxes with a score above 0.3, then scale them back to the full image.

### 3. Finding the keypoints: RTMPose-m (`FPoseEstimator::EstimatePose`)
For each person box:
1. Make the box 25 % bigger and widen or heighten it to the model's 3:4 shape.
2. Cut that area out and resize it to 192 × 256 (an *affine warp*), then normalise the colours.
3. Run the model. For each of the 26 keypoints it returns two lists of numbers (*SimCC*): one along x (384 values) and one
   along y (512 values). The position of the highest value in each list is the keypoint's position, at half-pixel
   steps. The keypoint's **score** is the average of the two highest values.
4. Convert back to image pixels.

The 26 keypoints (HALPE_26): nose, eyes, ears, shoulders, elbows, wrists, hips, knees, ankles, head, neck, hip
(centre), big toes, small toes, heels.

### 4. Following the person between detections
Like Sports2D (`det_frequency = 4`), the person detector only runs **every 4th analysed frame**. On the frames in
between, each person's box is the box around their keypoints from the previous frame, made 25 % bigger. This saves a lot of
time: the detector is the slow model.

### 5. Sports2D's rules
Straight from Sports2D's `process.py`, with its default settings:
* People whose average score over all 26 keypoints is 0.2 or less are dropped. Overlapping people are merged (pose-level
  NMS at 0.45 overlap), best score first.
* Keypoints with a score **below 0.3** are ignored (*keypoint likelihood threshold*). Sports2D does not draw or use
  them.
* A person is kept only if **at least 30 %** of their keypoints are valid (*keypoint number threshold*) **and** those
  keypoints average **0.5 or more** (*average likelihood threshold*). That is why the skeleton model in the picture
  above is rejected.
* The **main person** is the first person kept (the highest score).

### 6. Angles: `PoseSports2D`
The same 26 angles as Sports2D's default settings, with Pose2Sim's definitions (keypoints, offset, sign):

| Joint angles (12) | Segment angles (14), measured against the horizontal |
|---|---|
| right / left **ankle** (dorsiflexion: knee–ankle vs. toe–heel, +90°) | right / left **foot** (big toe – heel) |
| right / left **knee** (flexion: ankle–knee–hip, −180°) | right / left **shank** (ankle – knee) |
| right / left **hip** (flexion: knee–hip vs. hip centre–neck) | right / left **thigh** (knee – hip) |
| right / left **shoulder** (flexion: elbow–shoulder vs. hip centre–neck) | **pelvis** (left hip – right hip), **shoulders** (left – right shoulder) |
| right / left **elbow** (flexion: wrist–elbow–shoulder, +180°, sign flipped) | **trunk** (neck – hip centre), **head** (head – neck) |
| right / left **wrist**: *never available*, the 26-point model has no finger point (Sports2D shows nothing either) | right / left **arm** (elbow – shoulder), **forearm** (wrist – elbow) |

* **Which way the person faces** (*visible side*, setting **Auto**): on every frame, if the feet point right on screen
  (heel → big toe), the person "faces right"; otherwise "left", and the x positions are mirrored before computing the
  angles so a knee bend reads the same either way. As in Sports2D, if any foot keypoint is missing the answer is
  "left".
* An angle is only computed when all its keypoints are valid (score ≥ 0.3); otherwise it is missing on that frame.
* Results are wrapped to −180…180° (pelvis and shoulders: −90…90°).
* Every frame's angles are in the result (`FPosePerson::Angles`, usable from Blueprint) for the phase 3 data panel.

### 7. The overlay: `CPP_UW_PoseOverlay`
A see-through widget over the whole screen, between the 3D view and the HUD (Z-order 5; the HUD is 10). Clicks pass
through it, so orbit, pan and muscle picking still work. It is pure C++ (drawn in `NativePaint`), so it needs no Widget
Blueprint. It draws what Sports2D's real-time display draws, in the same order, ported from its drawing functions:

1. **Box** around each person's valid keypoints (25 px margin), one colour per person, with their number.
2. **Keypoints**: dots coloured with Sports2D's colour map (*RdYlGn*: red = unsure, green = sure).
3. **Skeleton**: right-side bones blue, left-side green, the middle (head, neck, spine) orange.
4. **Angles**:
   * segment angles in **white**: a short line along the segment and a horizontal reference line;
   * joint angles in **green**: a line along the segment, the parent segment dotted, and an arc between them;
   * each value written next to it (*on the body*), and all values listed as "name: value" with a bar (50 px per 180°)
     (*the list*).

![Sports2D (Python) and the Unreal overlay on the same frame](images/pose_02_overlay_vs_sports2d.png)
*Left: real Sports2D's drawing functions run on a frame copied from Play. Right: the Unreal overlay with the result of
that same frame. The lines, colours and numbers are the same. The MetaHuman underneath has moved on a little because the
screenshot was taken a moment later: the overlay always shows the most recent result, which is a few frames old.*

Sizes are Sports2D's, in pixels of the analysed image (= the screen), so it looks like a Sports2D video of the same size,
except the text: Sports2D's default *fontSize* 0.3 was hard to read on screen, so the overlay uses **0.4** (a third
bigger; the list spacing grows with it). *Font Size* and *Size Scale* on the overlay change this.
Sports2D puts the list in the image's top-left corner; here it starts below the HUD title bar (*List Position*).

---

## The files

All new code is in `Source/MetaHumanTest/Public/PoseEstimation/` (headers) and `…/Private/PoseEstimation/` (code).

| File | What it does |
|---|---|
| `PoseTypes.h/.cpp` | The data: `EPoseKeypoint` (the 26 names), `FPoseKeypoint` (position in the image, position on screen, score, valid), `FPoseAngle`, `EPoseVisibleSide`, `FPosePerson` (keypoints, angles, side faced), `FPoseFrameResult` (everyone found in one frame + timings). Also the skeleton bone pairs from Pose2Sim. Usable from Blueprint. |
| `PoseSports2D.h/.cpp` | Sports2D's per-frame rules ported from `process.py` / Pose2Sim: which people are kept, which side they face, and the 26 angles (`GetAngleDefinitions`, `ComputeAngle`, `BuildFrameResult`). Plain functions, shared by the live component, the command-line test and (phase 3) the data panel. |
| `PoseInferenceThread.h/.cpp` (Private) | A dedicated thread that runs one analysis at a time, so the models never queue behind other editor work. |
| `PoseEstimator.h/.cpp` | **The core.** Loads the two `.onnx` models into Unreal's Neural Network Engine (ONNX Runtime, CPU) and runs them, with RTMLib's preparation and decoding ported line by line: `Detect`, `EstimatePose`, `ProcessFrame` (the every-4th-frame logic), `ResizeLinear` / `WarpAffineLinear` (copies of OpenCV's image resizing), plus PNG/JSON helpers for tests. Not tied to the game, so tests can run it on image files. |
| `PoseFrameGrabber.h/.cpp` (Private) | Copies the on-screen image from the GPU without stalling the game ([step 1](#1-getting-the-picture-poseframegrabber)). Converts it to the 8-bit BGR image the models expect, whatever pixel format the screen uses. |
| `PoseEstimationComp.h/.cpp` | `UPoseEstimationComp`, on the player controller. Loads the models in the background the first time, runs the capture → analyse → publish loop (timing the next copy), owns the overlay, fires `OnPoseEstimated`, keeps the timing statistics. The `Pose.*` console commands live here too. |
| `CPP_UW_PoseOverlay.h/.cpp` | The Sports2D-style drawing ([step 7](#7-the-overlay-cpp_uw_poseoverlay)). |
| `PoseEstimationCommandlet.h/.cpp` | Runs the models on image files from the command line, without opening the editor, and writes the keypoints plus the people kept and their angles. Used to compare with Sports2D ([below](#how-we-know-it-matches-sports2d)). |

Other new files:

| Path | What |
|---|---|
| `PoseModels/*.onnx` | The two models, exactly the files RTMLib downloads (about 150 MB). `PoseModels/README.md` lists where they come from, their checksums and their licences. |
| `Tools/PoseEstimation/parity_check.py` | Runs real RTMLib on the same images and compares its output with Unreal's. |

---

## Settings

Select **PoseEstimation** in `BP_PlayerController`'s Components panel:

| Setting | Default | Meaning |
|---|---|---|
| Detector Model File / Pose Model File | `PoseModels/...onnx` | Which models to use (relative to the project folder) |
| Detection Frequency | 4 | Run the person detector every N analysed frames (Sports2D `det_frequency`) |
| Keypoint Likelihood Threshold | 0.3 | Keypoints scoring less are ignored |
| Average Likelihood Threshold | 0.5 | People whose valid keypoints average less are rejected |
| Keypoint Number Threshold | 0.3 | People with fewer than this fraction of valid keypoints are rejected |
| Visible Side | Auto | Which side of the person faces the camera (Auto = decided every frame from the feet; or Right / Left / Front / Back / None). Sports2D's demo setting gives each person their own; here it is the same for everyone |
| Max Analyses Per Second | 0 | Cap on frames analysed per second (0 = as many as the CPU manages) |
| Copy Next Frame During Analysis | on | Copy the next frame while the models work (timed to arrive as they finish): more frames per second |
| Enable On Begin Play | off | Start analysing as soon as Play starts |
| Overlay Widget Class / Overlay Z Order | `CPP_UW_PoseOverlay` / 5 | The overlay and where it sits between the 3D view and the HUD |

The overlay's own settings are on `CPP_UW_PoseOverlay`. To change them, make a Widget Blueprint child of it and set it
as *Overlay Widget Class*:

| Setting | Default | Meaning |
|---|---|---|
| Show Bounding Boxes / Keypoints / Skeleton / Angles | on | Each part of the drawing |
| Angle Values On Body / In List | on | Sports2D's `display_angle_values_on = ['body', 'list']` |
| Font Size | 0.4 | Sports2D's `fontSize` (OpenCV units). Sports2D's default is 0.3; 0.4 is easier to read on screen |
| Size Scale | 1 | Multiplies every size (lines, dots, arcs, text, list) |
| List Position | (10, 112) | Top-left of the angle list, in screen units (below the HUD title bar) |
| Show Model Box / Show Rejected People | off | Debug: the box the pose model looked at; faded keypoints of rejected people |

---

## How we know it matches Sports2D

The test runs **real RTMLib, set up exactly the way Sports2D sets it up**, and the Unreal code on the **same PNG
images**, then compares every keypoint.

| Test | Result |
|---|---|
| One frame of Sports2D's demo video (detector + pose) | Both people found; **all 52 keypoints identical**; detector boxes identical; scores within 0.0001 |
| 40 frames of the demo video, detector every 4th frame | Same people in every frame, detector on the same frames, boxes identical. **99.5 % of the 2,184 keypoints identical**, the rest at most **1.2 px** apart |
| A live frame from Play-In-Editor (`Pose.Dump`) | **All 52 keypoints identical** |
| **Angles** (phase 2): Sports2D's own angle functions on the keypoints Unreal found, vs. Unreal's `PoseSports2D` | Two live frames: 16/16 and 24/24 angles identical. 40-frame demo video: **1,656 angle values identical** (largest difference 0.00006°, rounding), same people kept, same facing side on every frame |
| **Drawing** (phase 2) | Side by side with Sports2D's drawing of the same frame (picture in [step 7](#7-the-overlay-cpp_uw_poseoverlay)): same elements, colours and values |

Where the tiny differences come from: OpenCV 5 cuts out the person with a float calculation that can't be matched bit for bit. On
a few pixels the crop differs by 1 grey level, and once in a while that moves a keypoint by one SimCC step (half a
model pixel ≈ 1 screen pixel). The image shrinking for the detector *is* bit-exact with OpenCV.

**Run the test yourself** (editor closed; Python with `pip install rtmlib onnxruntime opencv-python numpy`):
```
python -I Tools/PoseEstimation/parity_check.py frames <video.mp4> <frames_folder> 40 3
python -I Tools/PoseEstimation/parity_check.py ref <frames_folder> ref.json 4
"C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <project>/MetaHumanTest.uproject -run=PoseEstimation -Images=<frames_folder> -Out=unreal.json -DetFrequency=4 -unattended -nullrhi -nosplash
python -I Tools/PoseEstimation/parity_check.py compare ref.json unreal.json
```
The command-line run ends with exit code 1 because of unrelated engine start-up errors (GameFeatures settings). The
`LogPoseEstimation` lines in its output say whether the pose part worked. To check a live frame, use `Pose.Dump` in Play
and run `ref` and `compare` on the saved PNG / JSON.

---

## Speed

Measured on this PC (8 cores / 16 threads, CPU only), standalone game on the `Main` level, 75 s
([hands-free test](#try-it)):

| | Time |
|---|---|
| Loading the models | ~1.1–1.5 s (in the background, first time only) |
| **Frames analysed per second** | **10.7–12, steady** (game at 113–120 fps) |
| Copying a frame back from the GPU | ~30 ms |
| Waiting for the models to be free | ~10 ms |
| Analysis of a frame without / with the person detector (every 4th frame) | ~35 ms / ~220 ms (detector ~190 ms, pose model ~33 ms) |
| From the frame being on screen to its result (*latency*) | ~120 ms |

How it got there:
* **Phase 1** analysed one frame, then asked for the next: 6–8 frames/s in Play.
* **Phase 2, first try**: copying the next frame while the models work. In Play this gave only ~4 frames/s with gaps of
  several seconds. The models ran on Unreal's shared background task threads, which also do editor work (shaders,
  DDC, asset builds), so a frame could wait behind them.
* **Fix**: the models now have **their own thread** (`PoseInferenceThread`), and the next frame is copied **just in time**
  (the component keeps the recent copy and analysis times and asks for the next frame so it arrives as the models
  finish; earlier, it would wait and the overlay would lag more). Result: steady ~11 frames/s, no gaps, no copy
  timeouts.

Other notes:
* With the editor window **in the background**, Unreal deliberately slows itself down, and so does the analysis.
* Packaged builds would run the models on 1 CPU thread by default. `Config/DefaultEngine.ini` raises that to 4.
* Phase 3's angle filtering (Butterworth 6 Hz) needs at least ~12 samples per second. ~11 is close; if needed, Sports2D's
  "lightweight" models or the GPU option will lift it.

---

## Things to know

* **NNE output sizes.** Unreal's ONNX Runtime plugin remembers the previous run's output size, unless the input sizes
  are set again before each run. The detector's output size changes with the number of people, so the code sets the
  input sizes before **every** run. Without that you get an ONNX error in the editor and a crash in packaged builds.
* **Model files** are loaded from `PoseModels/` at runtime, not imported as Unreal assets. `MetaHumanTest.Build.cs`
  (`RuntimeDependencies`) copies them into packaged builds.
* **Mac:** the ONNX plugin supports Mac on the CPU. Not tested on a Mac yet.
* **Standalone / packaged build:** the game target (`MetaHumanTest.Target.cs`) didn't build with the installed 5.8
  engine because of its old `BuildSettingsVersion.V5` settings. This was the case before pose estimation too. **Fixed**
  (now `V7`, like the editor target): `MetaHumanTest.exe` builds, with pose estimation in it. Packaging (cooking the
  content) hasn't been tried yet.
* **Default level:** `Config/DefaultEngine.ini` listed `EditorStartupMap=/Game/Main.Main` twice and had no
  `GameDefaultMap`, so a standalone / packaged game opened Unreal's empty "OpenWorld" template instead of `Main`.
  **Fixed**: the second line is now `GameDefaultMap=/Game/Main.Main`.
* **Two people in the level:** the anatomy skeleton model is found as a person but rejected (see the picture at the
  top). If it ever passes the rules, the main person is still the highest-scoring one.
* **Facing side when the feet are hidden:** Sports2D decides "left" whenever a foot keypoint is missing (an accident of
  how it compares numbers). We do the same so the angles match; with the feet out of frame the angles may be mirrored
  for someone facing right.
* **The overlay lags slightly:** it shows the most recent result, whose frame was copied ~120 ms earlier. Ways to cut
  this later: Sports2D's "lightweight" models, the GPU (DirectML, Windows only), or smaller analysed images.
* **`Pose.Stats` in Play can read near zero.** While an editor popup or menu is open, for example the autocomplete
  list of the Output Log's **Cmd** box while you type `Pose.Stats`, Unreal pauses the Play world to keep the editor
  responsive (`FSlateThrottleManager`, `UEditorEngine::Tick`). The MetaHuman stops too, and so does pose estimation
  until the popup closes. It's an editor effect: the standalone / packaged game isn't affected. For speed numbers use
  the [hands-free test](#try-it), or type the command in the in-game console (**`** key).

---

## Changes outside the new folder

| File | Change |
|---|---|
| `MetaHumanTest.uproject` | Enables the **NNERuntimeORT** plugin (Unreal's ONNX Runtime; it ships with the engine, off by default) |
| `Source/MetaHumanTest/MetaHumanTest.Build.cs` | Adds the modules `NNE`, `RenderCore`, `RHI`, `ImageCore`; stages `PoseModels/*.onnx` with packaged builds |
| `Source/MetaHumanTest/CPP_PlayerController.h/.cpp` | New constructor that creates the `PoseEstimation` component; `GetPoseEstimation()`. It sits on the controller, not the pawn, so it keeps running when you switch pawns |
| `Config/DefaultEngine.ini` | `[/Script/NNERuntimeORT.NNERuntimeORTSettings]`: 4 CPU threads for the models in packaged builds. `GameDefaultMap=/Game/Main.Main` (was a duplicated `EditorStartupMap` line), so the standalone game opens `Main` |
| `Source/MetaHumanTest.Target.cs` | `BuildSettingsVersion.V5` → `V7`, so the standalone game builds again (needed to give the app to coaches) |
| `README/README.md` | Link to this page; the new folders in "Where everything lives" |

**Perforce:** these files were edited without being checked out first (they were made writable). Check them out or use
*Reconcile Offline Work* in P4V, and add the new files (`PoseEstimation/` source, `PoseModels/`,
`Tools/PoseEstimation/`, this page and its image).

---

## Work log

| Date | Step |
|---|---|
| 2026-11-07 | Read Sports2D, RTMLib and Pose2Sim; plan and phases agreed (image route, balanced models, CPU so Mac works). |
| 2026-11-08 | Confirmed the Unreal 5.8 ONNX API and the models' inputs/outputs. Checked our ports of OpenCV's resize (bit-exact) and warp (within 1 grey level) in Python before writing C++. |
| 2026-11-08 | Phase 1 code written (12 files) and built: no errors, no warnings. This build also included the earlier tab-separator and speed-slider fixes, which until then were only applied through Live Coding. |
| 2026-11-08 | Command-line test vs RTMLib: one frame identical; 40-frame sequence 99.5 % identical, max 1.2 px. |
| 2026-11-08 | Tested in Play: dots appear on the MetaHuman; 6–8 frames/s; a live frame (`Pose.Dump`) matches RTMLib exactly; the anatomy skeleton is detected but rejected by Sports2D's rules. **Phase 1 done.** |
| 2026-11-08 | Phase 2 started. Read Sports2D's drawing code (`draw_bounding_box`, `draw_keypts`, `draw_skel`, `draw_angles`) and Pose2Sim's angle definitions. Made a reference image of what real Sports2D draws on our live frame, by running its own drawing functions. |
| 2026-11-08 | Fixed the standalone game build (`MetaHumanTest.Target.cs` V5 → V7): `MetaHumanTest.exe` builds. Needed so the app can be given to coaches and trainers. |
| 2026-11-08 | Phase 2 code written: `PoseSports2D` (Sports2D's rules + the 26 angles, shared with phase 3), the new overlay, copying the next frame during analysis (speed), averaged `Pose.Stats`, angles in `Pose.Dump`. Compiles cleanly; waiting for the editor build and tests. |
| 2026-11-08 | Phase 2 built (no errors, no warnings). Angles vs real Sports2D (its own functions, on Unreal's keypoints): live frame 16/16 identical; 40-frame demo video 1,656 angle values identical (largest difference 0.00006°, rounding); same people kept and same facing side on every frame. |
| 2026-11-08 | Phase 2 tested in Play: the overlay draws correctly on the MetaHuman (26/26 keypoints, facing left, 24 angles); side by side with Sports2D's drawing of the same frame. Found the speed problem (~4 frames/s with gaps): the models now get their own thread, the next frame is copied just in time, and `Pose.Stats` shows where the time goes. Compiles; to be tested in Play. **Phase 2 done** apart from that speed check. |
| 2026-11-08 | First Play test of the fix still showed 0 frames/s at times, while each frame's own timings were fine. Added a watchdog (logs what every stage is doing when results stop) and a hands-free test mode (`-PoseAutoEnable -PoseLogStats -PoseQuitAfter`). Found the standalone game opened the wrong level (missing `GameDefaultMap`); fixed. Hands-free test on `Main`, 75 s: **steady 10.7–12 frames/s, no stalls, no copy timeouts**, latency ~120 ms, game 113–120 fps. |
| 2026-11-08 | Play test: dots on the MetaHuman, slightly behind but close. The watchdog explained the remaining "0 frames/s": typing `Pose.Stats` in the Output Log's Cmd box opens an autocomplete popup, and the editor pauses the Play world while a popup is open (`FSlateThrottleManager`), so nothing was stalled in our code. **Phase 2 done.** |
| 2026-11-08 | Angle text a third bigger (overlay *Font Size* 0.3 → 0.4) after the Play test. Public GitHub repo [MetaHumanTest-PoseEstimation](https://github.com/mblumpkin/MetaHumanTest-PoseEstimation) created with the pose estimation code, models (Git LFS), tools, licences and docs. |

---

## Source code on GitHub

Everything in this page (the code, the models through Git LFS, the test tools and this documentation) is also in the
public repo **[mblumpkin/MetaHumanTest-PoseEstimation](https://github.com/mblumpkin/MetaHumanTest-PoseEstimation)**.
The git copy lives inside this project folder; its `.gitignore` lets git see only the pose estimation files (plus the
few shared files the feature changes), and the project's `.p4ignore` keeps git's files out of Perforce. Each further
step is committed there as it is done.

---

## Credits and licences

* Models: [OpenMMLab RTMPose / MMPose](https://github.com/open-mmlab/mmpose/tree/main/projects/rtmpose), Apache 2.0.
* Pre/post-processing ported from [RTMLib](https://github.com/Tau-J/rtmlib), Apache 2.0.
* Rules, settings and keypoint set from [Sports2D](https://github.com/davidpagnon/Sports2D) and
  [Pose2Sim](https://github.com/perfanalytics/pose2sim), BSD 3-Clause, by David Pagnon et al.
