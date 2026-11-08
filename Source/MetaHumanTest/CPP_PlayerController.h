// Player controller: owns input (Enhanced Input -> components on the possessed pawn), puts the UI on the
// viewport, and switches which pawn is possessed. It does not do the pawn's work itself: the widgets and
// input talk to components on whatever pawn is possessed, so a different pawn = different behaviour.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "CPP_PlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
struct FInputActionValue;
class UCPP_MouseFunctionComp;
class UCPP_UW_AnatomyViewSwitcher;
class UCPP_UW_MuscleInfo;
class UAnatomyPickerComp;
class UCPP_UW_PawnSwitcher;
class UCPP_UW_SpeedSlider;
class UWCPP_AnimationTimeline;
class UCPP_UW_IHPCHud;
class UPoseEstimationComp;

UCLASS()
class METAHUMANTEST_API ACPP_PlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ACPP_PlayerController();

	// ---------------- Pose estimation ----------------

	/** Sports2D-style pose estimation of the live view (overlay + data). Settings: select it in BP_PlayerController. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Pose Estimation")
	TObjectPtr<UPoseEstimationComp> PoseEstimation;

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	UPoseEstimationComp* GetPoseEstimation() const { return PoseEstimation; }

	// ---------------- Input ----------------

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> MouseManager;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseMove;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> MouseZoom;

	// ---------------- MetaHuman / placeholders ----------------

	// access the meta human // need to be attached on bp
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MetaHuman")
	TObjectPtr<AActor> TargetMetaHuman = nullptr;

	/** Filled in BeginPlay from the BP_PlayerPlaceHolderManagerA in the level: where switched-to pawns are spawned. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="MetaHuman")
	TArray<TObjectPtr<AActor>> PC_PlayerPlaceHolders;

	// ---------------- UI (set to the widget Blueprints in BP_PlayerController) ----------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UCPP_UW_SpeedSlider> SpeedSliderWidget;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UWCPP_AnimationTimeline> TimelineWidgetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UCPP_UW_AnatomyViewSwitcher> AnatomyViewSwitcherWidgetClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UCPP_UW_PawnSwitcher> PawnSwitcherWidgetClass;

	/** Muscle name tooltip + info popup (listens to the pawn's UAnatomyPickerComp). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UCPP_UW_MuscleInfo> MuscleInfoWidgetClass;

	/** IHPC HUD from the Figma design (title bar, menu, analysis panel, timeline bar).
	 *  Set to WBP_IHPC_HUD in BP_PlayerController > Class Defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	TSubclassOf<UCPP_UW_IHPCHud> MainHUDWidgetClass;

	/** Draw order of the HUD: higher = on top. 10 keeps it above the other widgets (they use 0); the muscle info
	 *  tooltip / popup (20) stays above the HUD. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UI")
	int32 MainHUDZOrder = 10;

	/** The HUD created in BeginPlay, e.g. GetMainHUD()->SelectTab(EIHPCNavTab::SavedVideos) from a key press. */
	UFUNCTION(BlueprintPure, Category="UI")
	UCPP_UW_IHPCHud* GetMainHUD() const { return MainHUDWidget; }

	/** A left click that moves less than this (pixels) selects the hovered muscle; more is a pan drag. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Input")
	float ClickMaxDragPixels = 6.f;

	// ---------------- Pawn switching (WB_PawnSwitcher) ----------------

	/** Pawns the Up / Down buttons cycle through. A class that isn't in the level yet is spawned at the matching placeholder. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pawn Switching")
	TArray<TSubclassOf<APawn>> SwitchablePawnClasses;

	/** Possess the next (+1) or previous (-1) pawn in SwitchablePawnClasses. */
	UFUNCTION(BlueprintCallable, Category="Pawn Switching")
	void SwitchPawn(int32 Direction);

	UFUNCTION(BlueprintCallable, Category="Pawn Switching")
	void SwitchToPawnIndex(int32 Index);

protected:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	void HandleMouseMove(const FInputActionValue& Value);
	void HandleMouseZoom(const FInputActionValue& Value);

	/** Mouse orbit / pan / zoom is done by the possessed pawn's component (found fresh, so pawn switches just work). */
	UCPP_MouseFunctionComp* GetMouseFunctionComp() const;

	/** Hover / click on muscles is done by the possessed pawn's picker component (if it has one). */
	void UpdateMusclePicking();

private:
	template <typename WidgetT>
	WidgetT* AddWidget(TSubclassOf<WidgetT> WidgetClass, int32 ZOrder = 0);

	UPROPERTY()
	TObjectPtr<UCPP_UW_SpeedSlider> SpeedSliderWidgetInstance = nullptr;

	UPROPERTY()
	TObjectPtr<UWCPP_AnimationTimeline> TimelineWidget = nullptr;

	UPROPERTY()
	TObjectPtr<UCPP_UW_AnatomyViewSwitcher> AnatomyViewSwitcherWidget = nullptr;

	UPROPERTY()
	TObjectPtr<UCPP_UW_PawnSwitcher> PawnSwitcherWidget = nullptr;

	UPROPERTY()
	TObjectPtr<UCPP_UW_MuscleInfo> MuscleInfoWidget = nullptr;

	/** IHPC HUD instance (see MainHUDWidgetClass). */
	UPROPERTY()
	TObjectPtr<UCPP_UW_IHPCHud> MainHUDWidget = nullptr;

	FVector2D LeftPressPosition = FVector2D::ZeroVector;

	/** One instance per SwitchablePawnClasses entry, created on first use (kept alive so switching back is instant). */
	UPROPERTY()
	TArray<TObjectPtr<APawn>> SwitchablePawns;

	int32 CurrentPawnIndex = 0;
};
