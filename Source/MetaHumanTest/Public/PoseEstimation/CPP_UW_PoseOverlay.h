// Full-screen overlay that draws the estimated pose on top of the 3D view, the way Sports2D draws its real-time display
// (process.py + Pose2Sim common.py), in the same order:
//
//   1. a box around each person (one colour per person, with their number),
//   2. a dot per keypoint, coloured by confidence (RdYlGn: red = unsure, green = sure),
//   3. the skeleton: right side blue, left side green, middle orange,
//   4. the angles: segment angles in white (the segment + a horizontal reference), joint angles in green (the segment,
//      the parent segment dotted, an arc), each value written next to it, and a list of all values with bars.
//
// Sizes are Sports2D's, in pixels of the analysed image (= the screen), so it looks like Sports2D's video at the
// same size; SizeScale and FontSize make it bigger. Pure C++ (drawn in NativePaint): works without a Widget Blueprint.
// Clicks pass through it.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "PoseEstimation/PoseTypes.h"
#include "Styling/SlateBrush.h"
#include "CPP_UW_PoseOverlay.generated.h"

UCLASS()
class METAHUMANTEST_API UCPP_UW_PoseOverlay : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Show this result (keypoints are placed with the result's image -> screen mapping). */
	UFUNCTION(BlueprintCallable, Category="Pose Overlay")
	void SetPoseResult(const FPoseFrameResult& InResult);

	/** Draw nothing until the next result. */
	UFUNCTION(BlueprintCallable, Category="Pose Overlay")
	void ClearPose();

	// ---------------- What to draw (Sports2D real-time display) ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bShowBoundingBoxes = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bShowKeypoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bShowSkeleton = true;

	/** Angle lines and arcs on the body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bShowAngles = true;

	/** Sports2D display_angle_values_on 'body': each value written next to its arc. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bAngleValuesOnBody = true;

	/** Sports2D display_angle_values_on 'list': all values listed in a corner, with bars. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	bool bAngleValuesInList = true;

	// ---------------- Sizes / placement ----------------

	/** Sports2D fontSize (OpenCV units). Sports2D's default is 0.3; 0.4 here (a third bigger) so the values are easier to
	 *  read on screen. The list spacing grows with it, as in Sports2D. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay", meta=(ClampMin="0.1", ClampMax="3"))
	float FontSize = 0.4f;

	/** Multiplies every size (lines, dots, arcs, text, the list). 1 = Sports2D's sizes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay", meta=(ClampMin="0.25", ClampMax="5"))
	float SizeScale = 1.f;

	/** Top-left of the angle list, in widget units. Sports2D puts it in the image's top-left corner; here it starts
	 *  below the HUD title bar (105 tall) so the HUD doesn't cover it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay")
	FVector2D ListPosition = FVector2D(10.0, 112.0);

	// ---------------- Debug ----------------

	/** Outline the box the pose model looked at (detector box, or the box from the previous frame). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay|Debug")
	bool bShowModelBox = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay|Debug")
	FLinearColor ModelBoxColor = FLinearColor(1.f, 1.f, 1.f, 0.35f);

	/** Also draw the keypoints of people Sports2D's rules reject (faded). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Overlay|Debug")
	bool bShowRejectedPeople = false;

	/** Sports2D's keypoint colour: matplotlib "RdYlGn" at the score (clamped to 0..0.99). */
	static FLinearColor ScoreColor(float Score);

	/** Sports2D's colour for person N (Pose2Sim common.py "colors", cycled). */
	static FLinearColor PersonColor(int32 PersonIndex);

protected:
	virtual void NativeOnInitialized() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	void RequestRepaint();

	FPoseFrameResult Result;
	bool bHasResult = false;
	FSlateBrush DotBrush;     // white circle
	FSlateBrush SolidBrush;   // white rectangle (list bars)
};
