// Fill out your copyright notice in the Description page of Project Settings.

using UnrealBuildTool;

public class MetaHumanTest : ModuleRules
{
	public MetaHumanTest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "UMG" });

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"EnhancedInput",
			"Slate",
			"SlateCore",
			"MetaHumanAnatomyLayers", // UAnatomyViewComp (Plugins/MetaHumanAnatomyLayers)
			// Pose estimation (Source/MetaHumanTest/*/PoseEstimation)
			"NNE",        // Neural Network Engine: runs the ONNX models (runtime: NNERuntimeORT plugin)
			"RenderCore", // copying the game view back from the GPU
			"RHI",
			"ImageCore",  // loading / saving test images
		});

		// The Sports2D models (.onnx) are read at runtime: ship them with packaged builds.
		RuntimeDependencies.Add("$(ProjectDir)/PoseModels/*.onnx", StagedFileType.UFS);
	}
}
