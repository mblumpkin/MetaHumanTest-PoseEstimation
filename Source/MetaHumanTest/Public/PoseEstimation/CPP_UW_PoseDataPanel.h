// The Pose Data panel (phase 3): Sports2D's angles as numbers and graphs, recording and saved files.
//
//   +-------------------------------------------------+
//   | POSE DATA                                    X  |
//   | Analysing 11 frames/s - facing left - 20 angles  |
//   | [ Live ] [ Last recording ]                      |
//   | (graph: the selected angles over time)           |
//   | ANGLES - click to show on the graph (up to 4)    |
//   |   Joint angles       right knee      23.4        |
//   |   ...                                            |
//   | [ Record ]  [ Open folder ]   saved: Recording_..|
//   +-------------------------------------------------+
//
// "Live" shows the last LiveHistorySeconds of raw values (as on the overlay). "Last recording" shows the last saved
// recording after Sports2D's post-processing (solid) over the raw values (faint), like Sports2D's angle plots.
// Record toggles the timeline's Record (so the video card and the pose data are recorded together).
//
// Everything is built and drawn in C++ (no Widget Blueprint), in the HUD's style: the panel background of the other
// panels (black 62 %, radius 29), Avenir Heavy / Medium, the accordion headers' light-grey buttons. It is docked on
// the right so the MetaHuman and the overlay stay visible.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Styling/SlateTypes.h"
#include "CPP_UW_PoseDataPanel.generated.h"

class UBorder;
class UButton;
class UScrollBox;
class UTextBlock;
class UPoseEstimationComp;
class UCPP_UW_PoseDataPanel;

/** One line on the angle graph. Values may be NaN (gaps are left open). */
struct FPoseGraphLine
{
	FLinearColor Color = FLinearColor::White;
	float Thickness = 2.f;
	TArray<double> Times;    // seconds
	TArray<double> Values;   // degrees
};

/** A simple time / degrees graph that leaves gaps where values are missing. */
UCLASS()
class METAHUMANTEST_API UCPP_UW_PoseAngleGraph : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetLines(TArray<FPoseGraphLine>&& InLines, double InTimeMin, double InTimeMax);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	FSlateFontInfo LabelFont;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	FLinearColor AxisColor = FLinearColor(1.f, 1.f, 1.f, 0.6f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	FLinearColor GridColor = FLinearColor(1.f, 1.f, 1.f, 0.08f);

protected:
	virtual void NativeOnInitialized() override;
	virtual int32 NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

private:
	TArray<FPoseGraphLine> Lines;
	double TimeMin = 0.0;
	double TimeMax = 1.0;
};

/** One clickable row of the angle list: colour swatch (when on the graph), name, current value. */
UCLASS()
class METAHUMANTEST_API UCPP_UW_PoseAngleRow : public UUserWidget
{
	GENERATED_BODY()

public:
	void Setup(UCPP_UW_PoseDataPanel* InPanel, int32 InAngleIndex, const FString& InName, const FSlateFontInfo& InFont);
	void SetValue(const FText& InValue);
	void SetSelected(bool bSelected, const FLinearColor& Color);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	UFUNCTION()
	void HandleClicked();

	TWeakObjectPtr<UCPP_UW_PoseDataPanel> Panel;
	int32 AngleIndex = INDEX_NONE;
	FString AngleName;
	FSlateFontInfo Font;

	UPROPERTY(Transient) TObjectPtr<UButton> RowButton;
	UPROPERTY(Transient) TObjectPtr<UBorder> Swatch;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> ValueText;
};

UCLASS()
class METAHUMANTEST_API UCPP_UW_PoseDataPanel : public UUserWidget
{
	GENERATED_BODY()

public:
	/** The component the panel reads (set by UPoseEstimationComp when it creates the panel). */
	void SetPoseComponent(UPoseEstimationComp* InComponent) { PoseComponent = InComponent; }

	/** Show / hide an angle on the graph (called by the rows). */
	void ToggleAngle(int32 AngleIndex);

	/** Width of the panel and its distance from the right / top / bottom edges of the screen (widget units). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	float PanelWidth = 560.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	FMargin PanelMargins = FMargin(0.f, 112.f, 96.f, 130.f);   // left unused; top below the title bar; right leaves room for the phase 4 buttons; bottom above the timeline

	/** Angles on the graph when the panel opens (Sports2D names). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	TArray<FName> DefaultAngles = { TEXT("right knee"), TEXT("left knee"), TEXT("trunk") };

	/** Colours of the graph lines, in order of selection (at most this many angles at once). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data")
	TArray<FLinearColor> LineColors;

	/** Seconds between refreshes of the numbers and the live graph. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Data", meta=(ClampMin="0.02"))
	float RefreshInterval = 0.1f;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void BuildTree();
	void Refresh();
	void RefreshRows();
	void RefreshGraph();
	FSlateFontInfo AvenirFont(bool bHeavy, float Size) const;
	UButton* MakeButton(const FText& Label, UTextBlock*& OutLabel, float Alpha);

	UFUNCTION() void HandleClose();
	UFUNCTION() void HandleLive();
	UFUNCTION() void HandleLastRecording();
	UFUNCTION() void HandleRecord();
	UFUNCTION() void HandleOpenFolder();

	TWeakObjectPtr<UPoseEstimationComp> PoseComponent;
	TArray<int32> SelectedAngles;
	bool bShowLastRecording = false;
	FString ShownRecordingFolder;   // switches to "Last recording" when a new recording has been saved
	float TimeSinceRefresh = 0.f;

	UPROPERTY(Transient) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> GraphCaption;
	UPROPERTY(Transient) TObjectPtr<UButton> LiveButton;
	UPROPERTY(Transient) TObjectPtr<UButton> RecordingViewButton;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> RecordLabel;
	UPROPERTY(Transient) TObjectPtr<UTextBlock> SavedText;
	UPROPERTY(Transient) TObjectPtr<UCPP_UW_PoseAngleGraph> Graph;
	UPROPERTY(Transient) TArray<TObjectPtr<UCPP_UW_PoseAngleRow>> Rows;
};
