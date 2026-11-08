// Sports2D's per-frame rules and angle maths, ported from Sports2D process.py and Pose2Sim common.py:
//
//   * which detected people are kept (pose-level NMS, keypoint / person likelihood thresholds),
//   * which side of each person faces the camera ("auto": from the direction the feet point),
//   * the 26 default joint and segment angles (angle_dict: keypoints, offset, sign, wrap-around).
//
// Plain functions, no Unreal objects: used by the live component, the command-line test and (phase 3) the data panel.

#pragma once

#include "CoreMinimal.h"
#include "PoseEstimation/PoseTypes.h"

struct FPoseRawFrame;

namespace PoseSports2D
{
	/** One entry of Pose2Sim's angle_dict. */
	struct FAngleDefinition
	{
		const TCHAR* Name;      // lower case, as Sports2D writes it ("right knee")
		int32 Keypoints[4];     // EPoseKeypoint indices; -1 = a keypoint HALPE_26 doesn't have (RIndex / LIndex)
		int32 NumKeypoints;     // 2 = segment angle vs. horizontal, 3 = joint angle around the middle point, 4 = between two segments
		double Offset;          // added before Sign
		double Sign;            // 1 or -1
		bool bJoint;
	};

	/** Sports2D's default angle list (Config_demo.toml): 12 joint angles then 14 segment angles. */
	METAHUMANTEST_API TConstArrayView<FAngleDefinition> GetAngleDefinitions();

	struct FSettings
	{
		float KeypointLikelihoodThreshold = 0.3f;
		float AverageLikelihoodThreshold = 0.5f;
		float KeypointNumberThreshold = 0.3f;
		EPoseVisibleSide VisibleSide = EPoseVisibleSide::Auto;
	};

	/** Raw model output of one frame -> the people Sports2D keeps, their valid keypoints and their angles.
	 *  ImageToScreen* map image pixels to fractions of the viewport (for the overlay). */
	METAHUMANTEST_API void BuildFrameResult(const FPoseRawFrame& Raw, const FIntPoint& ImageSize, const FVector2D& ImageToScreenOffset,
		const FVector2D& ImageToScreenScale, const FSettings& Settings, FPoseFrameResult& Out);

	/** Resolves the visible side and fills Person.Angles (compute_angles_for_person). Only meaningful for valid people. */
	METAHUMANTEST_API void ComputeAngles(FPosePerson& Person, EPoseVisibleSide VisibleSide);

	/** Pose2Sim points_to_angles + fixed_angles for one definition, on already flipped coordinates. NaN if not computable. */
	METAHUMANTEST_API double ComputeAngle(const FAngleDefinition& Definition, TConstArrayView<FVector2D> FlippedPoints, TConstArrayView<bool> Valid);

	/** Kept people, valid keypoints and angles as JSON (for comparing with real Sports2D). */
	METAHUMANTEST_API FString ResultToJson(const FPoseFrameResult& Result, const FString& Name);
}
