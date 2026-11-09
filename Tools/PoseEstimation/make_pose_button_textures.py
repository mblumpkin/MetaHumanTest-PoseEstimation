"""
make_pose_button_textures.py
============================

Builds the pictures of the two round pose estimation buttons on the right of the screen (phase 4), in the
exact style of the layer switcher's Body / Muscles / Skeleton buttons:

    T_IHPC_PB_Overlay_On / _Off   "Pose Overlay": a stick figure with joint dots (the Sports2D skeleton)
    T_IHPC_PB_Data_On / _Off      "Pose Data":    a line graph

    _Off = navy icon on the light-blue glossy circle (like an unselected layer button)
    _On  = white icon on the dark glossy circle      (like the selected layer button)

How: the empty circles are taken from the same Figma pictures the layer buttons come from
(Tools/FigmaUI/make_ui_textures.py), with their icons removed, and the new icons are drawn on them with the same
line width and colours. Output: SourceArt/Figma/UE/T_IHPC_PB_*.png (214 x 214 px, drawn at 46.66 px like the layer
buttons). Then import them into Unreal with Tools/PoseEstimation/ue_import_pose_textures.py.

Run from the project root (needs numpy and Pillow):
    python -I Tools/PoseEstimation/make_pose_button_textures.py
"""

import os
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "Tools", "FigmaUI"))
import make_ui_textures as ui   # noqa: E402  (the layer switcher's helpers and measurements)

SIZE = ui.LAYER_CROP            # 214 px, the layer button textures' size
SUPER = 8                       # draw the icons 8x bigger, then shrink: smooth edges
LINE = 6.0                      # icon line width in texture px (measured on the layer icons: ~6 px)
DOT = 7.5                       # joint dot radius
NAVY = np.array([24.0, 60.0, 85.0])    # icon colour on an unselected circle (same as the layer icons)
WHITE = np.array([255.0, 255.0, 255.0])


def empty_circles():
    """The selected (dark) and unselected (light) circles without any icon."""
    f1 = np.array(Image.open(os.path.join(ui.SRC, "layers_f1_raw_1.png")).convert("RGBA")).astype(np.float32)
    f4 = np.array(Image.open(os.path.join(ui.SRC, "layers_f4_raw_1.png")).convert("RGBA")).astype(np.float32)
    body_on, body_off = ui._layer_crop(f1, "Body"), ui._layer_crop(f4, "Body")
    muscles_on, muscles_off = ui._layer_crop(f4, "Muscles"), ui._layer_crop(f1, "Muscles")

    # Same trick as make_ui_textures: where one circle has icon lines, the other usually has clean background;
    # where both have lines, paint over from the surrounding colours.
    body_lines = ui._grow(ui._icon_alpha(body_off) > 0.02, 3)
    muscle_lines = ui._grow(ui._icon_alpha(muscles_off) > 0.02, 3)

    empty_on = body_on.copy()
    empty_on[body_lines] = muscles_on[body_lines]
    empty_on = ui._diffuse_fill(empty_on, body_lines & muscle_lines)

    empty_off = body_off.copy()
    empty_off[body_lines] = muscles_off[body_lines]
    empty_off = ui._diffuse_fill(empty_off, body_lines & muscle_lines)

    # Check: putting the body icon back on the empty circles should give Figma's real pictures.
    a = ui._icon_alpha(body_off)[..., None]
    off_diff = np.abs(empty_off[..., :3] * (1 - a) + NAVY * a - body_off[..., :3]).mean()
    on_diff = np.abs(empty_on[..., :3] * (1 - a) + WHITE * a - body_on[..., :3]).mean()
    print("empty circles: body icon put back vs Figma, mean difference %.2f (off) / %.2f (on) of 255" % (off_diff, on_diff))
    return empty_on, empty_off


def icon_alpha(draw_fn):
    """Draws an icon (white on black) 8x bigger, shrinks it to 214 px, returns its coverage 0..1."""
    big = Image.new("L", (SIZE * SUPER, SIZE * SUPER), 0)
    draw_fn(ImageDraw.Draw(big), SUPER)
    small = big.resize((SIZE, SIZE), Image.LANCZOS)
    return np.clip(np.array(small).astype(np.float32) / 255.0, 0.0, 1.0)


def polyline(d, k, pts, width=LINE):
    """Line through the points with round ends and joins."""
    p = [(x * k, y * k) for x, y in pts]
    d.line(p, fill=255, width=round(width * k), joint="curve")
    r = width * k / 2
    for x, y in (p[0], p[-1]):
        d.ellipse([x - r, y - r, x + r, y + r], fill=255)


def dot(d, k, x, y, r=DOT):
    d.ellipse([(x - r) * k, (y - r) * k, (x + r) * k, (y + r) * k], fill=255)


def ring(d, k, x, y, r, width=LINE):
    d.ellipse([(x - r) * k, (y - r) * k, (x + r) * k, (y + r) * k], outline=255, width=round(width * k))


def draw_overlay_icon(d, k):
    """A walking stick figure with dots on the joints: what the Pose Overlay draws on the MetaHuman."""
    neck, hip = (110, 84), (104, 122)
    ring(d, k, 113, 62, 12)                                   # head
    polyline(d, k, [neck, hip])                               # spine
    polyline(d, k, [neck, (128, 104), (147, 96)])             # front arm: elbow, wrist
    polyline(d, k, [neck, (92, 106), (79, 124)])              # back arm
    polyline(d, k, [hip, (124, 142), (121, 166)])             # front leg: knee, ankle
    polyline(d, k, [hip, (91, 146), (71, 157)])               # back leg
    for x, y in [neck, hip, (128, 104), (147, 96), (92, 106), (79, 124), (124, 142), (121, 166), (91, 146), (71, 157)]:
        dot(d, k, x, y)


def draw_data_icon(d, k):
    """A line graph: two axes and a curve with its data points. What the Pose Data panel shows."""
    polyline(d, k, [(60, 52), (60, 160), (162, 160)])        # axes
    points = [(76, 134), (95, 100), (114, 122), (134, 76), (152, 96)]
    polyline(d, k, points)
    for x, y in points:
        dot(d, k, x, y, 7.0)


def save(arr, name):
    path = os.path.join(ui.OUT, name)
    Image.fromarray(np.clip(np.round(arr), 0, 255).astype(np.uint8)).save(path)
    ui.bleed_transparent_edges(path)    # clean edges when drawn small (see make_ui_textures.py)
    print("wrote", os.path.relpath(path, ROOT), (arr.shape[1], arr.shape[0]))


def main():
    empty_on, empty_off = empty_circles()
    for name, draw_fn in [("Overlay", draw_overlay_icon), ("Data", draw_data_icon)]:
        a = icon_alpha(draw_fn)[..., None]
        on = empty_on.copy()
        on[..., :3] = empty_on[..., :3] * (1 - a) + WHITE * a
        off = empty_off.copy()
        off[..., :3] = empty_off[..., :3] * (1 - a) + NAVY * a
        save(on, "T_IHPC_PB_%s_On.png" % name)
        save(off, "T_IHPC_PB_%s_Off.png" % name)


if __name__ == "__main__":
    main()
