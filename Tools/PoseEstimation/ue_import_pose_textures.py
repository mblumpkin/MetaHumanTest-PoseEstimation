"""
ue_import_pose_textures.py  (runs INSIDE Unreal's Python, not regular Python)
==========================

Imports the four pose button pictures made by make_pose_button_textures.py:
    SourceArt/Figma/UE/T_IHPC_PB_*.png  ->  /Game/UserInterface/IHPC/Textures/T_IHPC_PB_*.uasset
with the same UI texture settings as Tools/FigmaUI/ue_import_textures.py (which imports *every* PNG; this one only
touches the pose buttons, so the other textures stay as they are in Perforce).

How to run it (editor closed), from the project folder:
    "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" ^
        "%CD%/MetaHumanTest.uproject" -run=pythonscript -script="%CD%/Tools/PoseEstimation/ue_import_pose_textures.py" ^
        -unattended -nosplash -nullrhi -SCCProvider=None

Or with the editor open: Tools > Execute Python Script... and pick this file.
"""

import os

import unreal

PROJECT_DIR = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
SOURCE_DIR = os.path.join(PROJECT_DIR, "SourceArt", "Figma", "UE")
DEST_PATH = "/Game/UserInterface/IHPC/Textures"
PREFIX = "T_IHPC_PB_"


def import_pose_textures():
    pngs = sorted(f for f in os.listdir(SOURCE_DIR) if f.startswith(PREFIX) and f.lower().endswith(".png"))
    unreal.log("[Pose] Importing {} pose button textures from {}".format(len(pngs), SOURCE_DIR))

    tasks = []
    for file_name in pngs:
        task = unreal.AssetImportTask()
        task.filename = os.path.join(SOURCE_DIR, file_name)
        task.destination_path = DEST_PATH
        task.destination_name = os.path.splitext(file_name)[0]
        task.automated = True
        task.replace_existing = True
        task.save = False
        tasks.append(task)

    unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks(tasks)

    for task in tasks:
        for object_path in task.imported_object_paths:
            texture = unreal.load_asset(object_path)
            if not isinstance(texture, unreal.Texture2D):
                continue
            # Same settings as the layer switcher's textures (see ue_import_textures.py for why).
            texture.set_editor_property("compression_settings", unreal.TextureCompressionSettings.TC_EDITOR_ICON)
            texture.set_editor_property("mip_gen_settings", unreal.TextureMipGenSettings.TMGS_SIMPLE_AVERAGE)
            texture.set_editor_property("filter", unreal.TextureFilter.TF_TRILINEAR)
            texture.set_editor_property("never_stream", True)
            texture.set_editor_property("lod_group", unreal.TextureGroup.TEXTUREGROUP_UI)
            texture.set_editor_property("srgb", True)
            unreal.EditorAssetLibrary.save_loaded_asset(texture, only_if_is_dirty=False)
            unreal.log("[Pose] Imported {}".format(object_path))


import_pose_textures()
