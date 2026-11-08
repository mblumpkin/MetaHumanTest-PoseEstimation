// Runs the pose models on image files without opening the editor, to compare with real Sports2D / RTMLib.
//
//   UnrealEditor-Cmd.exe <project>/MetaHumanTest.uproject -run=PoseEstimation -Images=<image or folder> -Out=<file.json>
//                        [-DetFrequency=4] -unattended -nullrhi -nosplash
//
// A folder is treated as a video: its .png files in name order, with Sports2D's tracking (the detector runs every
// DetFrequency frames). A single image always runs the detector. Output: a JSON list, one entry per image, with the
// detector boxes and each person's 26 keypoints + scores (FPoseEstimator::ToJson). Next to it, <file>.sports2d.json: the
// people Sports2D's rules keep, the side they face and their angles (PoseSports2D::ResultToJson).

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PoseEstimationCommandlet.generated.h"

UCLASS()
class METAHUMANTEST_API UPoseEstimationCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UPoseEstimationCommandlet();

	virtual int32 Main(const FString& Params) override;
};
