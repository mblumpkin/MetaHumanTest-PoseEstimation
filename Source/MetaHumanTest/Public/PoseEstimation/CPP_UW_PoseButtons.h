// The two round buttons on the right of the screen (phase 4), so nobody needs the console:
//
//        ( O )   Pose Overlay: show / hide the Sports2D skeleton and angles on the MetaHuman  (SetEstimationEnabled)
//        ( D )   Pose Data:    open / close the Pose Data panel (graphs, values, recording)  (SetDataPanelOpen)
//
// Styled exactly like the layer switcher (WBP_IHPC_LayerSwitcher): the same glossy 46 px circles (dark navy with a
// white icon = on, light blue with a navy icon = off), the same see-through pill behind them (#1D3C55 at 26 %), the
// same hover / press brightness. The pictures are T_IHPC_PB_* (Tools/PoseEstimation/make_pose_button_textures.py).
//
// The buttons follow the component's state every frame, so they stay right when the panel is closed with its X,
// from the console, or from Blueprint. Built in C++ (no Widget Blueprint); UPoseEstimationComp creates it.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "CPP_UW_PoseButtons.generated.h"

class UButton;
class UTexture2D;
class UPoseEstimationComp;

UCLASS()
class METAHUMANTEST_API UCPP_UW_PoseButtons : public UUserWidget
{
	GENERATED_BODY()

public:
	UCPP_UW_PoseButtons(const FObjectInitializer& ObjectInitializer);

	/** The component the buttons drive (set by UPoseEstimationComp when it creates them). */
	void SetPoseComponent(UPoseEstimationComp* InComponent) { PoseComponent = InComponent; }

	// ---- Pictures (on = dark circle, white icon; off = light circle, navy icon) ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	TSoftObjectPtr<UTexture2D> OverlayOnTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	TSoftObjectPtr<UTexture2D> OverlayOffTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	TSoftObjectPtr<UTexture2D> DataOnTexture;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	TSoftObjectPtr<UTexture2D> DataOffTexture;

	// ---- Layout (screen units; the layer switcher's sizes) ----

	/** Size each round button is drawn at (the layer buttons: 46.66, circle = 46). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	float ButtonSize = 46.66f;

	/** Distance between the two button centres (the layer buttons: 58.88). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	float ButtonSpacing = 58.88f;

	/** Width of the pill behind the buttons (the layer switcher's pill is 65 high); its length follows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	float PillThickness = 65.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	FLinearColor PillColor = FLinearColor(FColor(0x1D, 0x3C, 0x55)).CopyWithNewOpacity(0.26f);

	/** Gap between the pill and the right edge of the screen. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	float RightMargin = 16.f;

	/** Moves the pill up (negative) or down from the middle of the screen's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Layout")
	float VerticalOffset = 0.f;

	/** Brightness while the mouse is over a button / while it is pressed (same as the layer buttons). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	float HoveredTint = 1.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Buttons|Style")
	float PressedTint = 0.8f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	UButton* MakeRoundButton(const FName& Name);
	/** Gives the buttons their on / off picture and tooltip when the component's state changed. */
	void Refresh(bool bForce);
	void ApplyLook(UButton* Button, const TSoftObjectPtr<UTexture2D>& Texture, bool bOn);

	UFUNCTION() void HandleOverlayClicked();
	UFUNCTION() void HandleDataClicked();

	TWeakObjectPtr<UPoseEstimationComp> PoseComponent;

	UPROPERTY(Transient) TObjectPtr<UButton> OverlayButton;
	UPROPERTY(Transient) TObjectPtr<UButton> DataButton;

	int32 ShownOverlay = -1;   // -1 = not drawn yet, 0 = off, 1 = on
	int32 ShownData = -1;
	FString ShownTooltipState;
	bool bWarnedMissingTexture = false;
};
