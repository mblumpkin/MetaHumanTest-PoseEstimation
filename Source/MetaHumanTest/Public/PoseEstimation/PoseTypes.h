// Pose estimation data shared by the estimator, the component, the overlay and (later) the data panel.
//
// The keypoints are HALPE_26: the 26-point body + feet set that Sports2D uses with its default
// "Body_with_feet" model (RTMPose-m, "balanced" mode). Order and names match Pose2Sim's skeletons.py, so
// files we export later line up with Sports2D's own .trc / .mot output.

#pragma once

#include "CoreMinimal.h"
#include "PoseTypes.generated.h"

METAHUMANTEST_API DECLARE_LOG_CATEGORY_EXTERN(LogPoseEstimation, Log, All);

/** HALPE_26 keypoints, in the order the RTMPose model outputs them. */
UENUM(BlueprintType)
enum class EPoseKeypoint : uint8
{
	Nose = 0,
	LEye,
	REye,
	LEar,
	REar,
	LShoulder,
	RShoulder,
	LElbow,
	RElbow,
	LWrist,
	RWrist,
	LHip,
	RHip,
	LKnee,
	RKnee,
	LAnkle,
	RAnkle,
	Head,
	Neck,
	Hip,
	LBigToe,
	RBigToe,
	LSmallToe,
	RSmallToe,
	LHeel,
	RHeel,
	Count UMETA(Hidden)
};

namespace PoseKeypoints
{
	constexpr int32 Num = static_cast<int32>(EPoseKeypoint::Count);   // 26

	/** Sports2D / Pose2Sim name of a keypoint ("Nose", "LShoulder"...). */
	METAHUMANTEST_API const TCHAR* GetName(int32 Index);

	/** Parent -> child pairs of Pose2Sim's HALPE_26 tree (used to draw the skeleton). */
	METAHUMANTEST_API TConstArrayView<TPair<int32, int32>> GetBones();
}

/** Which side of the person faces the camera. Sports2D flips x for "left" so angles keep the same sign either way. */
UENUM(BlueprintType)
enum class EPoseVisibleSide : uint8
{
	Auto,    // per frame, from the direction the feet point (heel -> big toe)
	Right,
	Left,
	Front,
	Back,
	None,
};

/** One joint or segment angle (Sports2D / Pose2Sim definitions, degrees). */
USTRUCT(BlueprintType)
struct FPoseAngle
{
	GENERATED_BODY()

	/** Sports2D name, lower case: "right knee", "trunk"... */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FName Name;

	/** Degrees. Only meaningful when bValid. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float Degrees = 0.f;

	/** All the keypoints it needs were valid. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	bool bValid = false;

	/** Joint angle (between two segments, e.g. knee) or segment angle (one segment vs. the horizontal, e.g. thigh). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	bool bJointAngle = false;
};

/** One keypoint of one person. */
USTRUCT(BlueprintType)
struct FPoseKeypoint
{
	GENERATED_BODY()

	/** Position in the analysed image, in pixels (origin top-left, y down: the same coordinates Sports2D uses). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D ImagePosition = FVector2D::ZeroVector;

	/** The same point as a fraction (0..1) of the game viewport, for drawing on top of it. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D ScreenUV = FVector2D::ZeroVector;

	/** Model confidence (RTMPose SimCC score, roughly 0..1). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float Score = 0.f;

	/** Score >= the keypoint likelihood threshold (Sports2D ignores the others). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	bool bValid = false;
};

/** One detected person. */
USTRUCT(BlueprintType)
struct FPosePerson
{
	GENERATED_BODY()

	/** 26 keypoints in EPoseKeypoint order. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	TArray<FPoseKeypoint> Keypoints;

	/** Box the pose model looked at (detector box, or the box grown from the previous frame's keypoints), image pixels. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D BoxMin = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D BoxMax = FVector2D::ZeroVector;

	/** Mean score of the valid keypoints. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float AverageScore = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="Pose")
	int32 NumValidKeypoints = 0;

	/** Passes Sports2D's person filter (enough valid keypoints, and their average score is high enough). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	bool bValid = false;

	/** The side facing the camera on this frame (resolved when the setting is Auto). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	EPoseVisibleSide VisibleSide = EPoseVisibleSide::Right;

	/** Sports2D's default angles, in its order: 12 joint angles then 14 segment angles (PoseSports2D::GetAngleDefinitions).
	 *  Empty for people who fail the filter. The wrist / hand angles are never valid: HALPE_26 has no finger keypoint. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	TArray<FPoseAngle> Angles;
};

/** Everything estimated from one captured frame. */
USTRUCT(BlueprintType)
struct FPoseFrameResult
{
	GENERATED_BODY()

	/** People kept after Sports2D's pose-level NMS, highest mean score first. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	TArray<FPosePerson> Persons;

	/** Index in Persons of the person to show / analyse (the first valid one), or -1. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	int32 PrimaryPerson = INDEX_NONE;

	/** Size of the analysed image, in pixels. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FIntPoint ImageSize = FIntPoint::ZeroValue;

	/** Image pixel -> fraction of the viewport: ScreenUV = ImageToScreenOffset + ImagePosition * ImageToScreenScale. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D ImageToScreenOffset = FVector2D::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category="Pose")
	FVector2D ImageToScreenScale = FVector2D::ZeroVector;

	/** Counts analysed frames since estimation was switched on. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	int32 FrameIndex = 0;

	/** True when the person detector ran on this frame (Sports2D runs it every DetectionFrequency frames). */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	bool bDetectorRan = false;

	/** Time spent in the detector / pose model, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float DetectorMs = 0.f;

	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float PoseMs = 0.f;

	/** From the frame being on screen to this result being ready, in milliseconds. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	float LatencyMs = 0.f;

	/** World time (seconds) when the analysed frame was rendered. */
	UPROPERTY(BlueprintReadOnly, Category="Pose")
	double WorldTime = 0.0;

	const FPosePerson* GetPrimary() const
	{
		return Persons.IsValidIndex(PrimaryPerson) ? &Persons[PrimaryPerson] : nullptr;
	}

	FVector2D ImageToScreenUV(const FVector2D& ImagePosition) const
	{
		return ImageToScreenOffset + ImagePosition * ImageToScreenScale;
	}
};
