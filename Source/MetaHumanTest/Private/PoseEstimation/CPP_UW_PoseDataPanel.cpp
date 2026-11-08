#include "PoseEstimation/CPP_UW_PoseDataPanel.h"

#include "PoseEstimation/PoseEstimationComp.h"
#include "PoseEstimation/PoseRecording.h"
#include "PoseEstimation/PoseSports2D.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Font.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

#include <cmath>
#include <limits>

#define LOCTEXT_NAMESPACE "PoseDataPanel"

namespace PoseDataPanelPrivate
{
	// HUD colours (README/02): panel = Figma "Rectangle 26", buttons = the accordion headers (#D9D9D9 at 20 %).
	const FLinearColor PanelColor(0.f, 0.f, 0.f, 0.62f);
	const FLinearColor HighlightGreen = FLinearColor(FColor(0xB3, 0xCA, 0x7F));
	const FLinearColor RecordRed = FLinearColor(FColor(0xE5, 0x48, 0x4D));

	FLinearColor Grey(float Alpha)
	{
		FLinearColor Color(FColor(217, 217, 217));
		Color.A = Alpha;
		return Color;
	}

	FSlateBrush Rounded(const FLinearColor& Color, float Radius)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.Width = 0.f;
		return Brush;
	}

	FButtonStyle ButtonStyle(float Alpha, float Radius)
	{
		FButtonStyle Style;
		Style.SetNormal(Rounded(Grey(Alpha), Radius));
		Style.SetHovered(Rounded(Grey(Alpha + 0.08f), Radius));
		Style.SetPressed(Rounded(Grey(Alpha + 0.14f), Radius));
		Style.SetNormalPadding(FMargin(0.f));
		Style.SetPressedPadding(FMargin(0.f));
		return Style;
	}

	/** A "nice" step (1, 2, 5 x 10^n) giving about Target ticks over Range. */
	double NiceStep(double Range, int32 Target)
	{
		const double Raw = Range / FMath::Max(1, Target);
		const double Power = std::pow(10.0, std::floor(std::log10(FMath::Max(Raw, 1e-9))));
		for (double M : { 1.0, 2.0, 5.0, 10.0 })
		{
			if (Raw <= M * Power)
			{
				return M * Power;
			}
		}
		return 10.0 * Power;
	}
}

using namespace PoseDataPanelPrivate;

// ================================================================================================
// Graph
// ================================================================================================

void UCPP_UW_PoseAngleGraph::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (!LabelFont.HasValidFont())
	{
		LabelFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	}
}

void UCPP_UW_PoseAngleGraph::SetLines(TArray<FPoseGraphLine>&& InLines, double InTimeMin, double InTimeMax)
{
	Lines = MoveTemp(InLines);
	TimeMin = InTimeMin;
	TimeMax = InTimeMax > InTimeMin ? InTimeMax : InTimeMin + 1.0;
	if (const TSharedPtr<SWidget> SlateWidget = GetCachedWidget())
	{
		SlateWidget->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

int32 UCPP_UW_PoseAngleGraph::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	LayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	if (Size.X < 40.f || Size.Y < 40.f)
	{
		return LayerId;
	}
	const FLinearColor Tint = InWidgetStyle.GetColorAndOpacityTint();
	const FPaintGeometry PaintGeometry = AllottedGeometry.ToPaintGeometry();
	const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	// Value range from the data (degrees), with some room.
	double MinV = TNumericLimits<double>::Max(), MaxV = TNumericLimits<double>::Lowest();
	for (const FPoseGraphLine& Line : Lines)
	{
		for (double V : Line.Values)
		{
			if (!std::isnan(V))
			{
				MinV = FMath::Min(MinV, V);
				MaxV = FMath::Max(MaxV, V);
			}
		}
	}
	if (MinV > MaxV)
	{
		MinV = -90.0;
		MaxV = 90.0;
	}
	if (MaxV - MinV < 20.0)
	{
		const double Mid = 0.5 * (MinV + MaxV);
		MinV = Mid - 10.0;
		MaxV = Mid + 10.0;
	}
	const double VStep = NiceStep(MaxV - MinV, 4);
	MinV = std::floor(MinV / VStep) * VStep;
	MaxV = std::ceil(MaxV / VStep) * VStep;

	const float Left = 40.f, Right = 8.f, Top = 6.f, Bottom = 20.f;
	const float PlotW = Size.X - Left - Right;
	const float PlotH = Size.Y - Top - Bottom;
	auto ToX = [&](double T) { return Left + PlotW * static_cast<float>((T - TimeMin) / (TimeMax - TimeMin)); };
	auto ToY = [&](double V) { return Top + PlotH * static_cast<float>(1.0 - (V - MinV) / (MaxV - MinV)); };
	auto DrawText = [&](const FString& Text, float X, float Y, bool bRightAligned, int32 Layer)
	{
		const FVector2f TextSize = FVector2f(Measure->Measure(Text, LabelFont));
		const float PosX = bRightAligned ? X - TextSize.X : X - TextSize.X * 0.5f;
		FSlateDrawElement::MakeText(OutDrawElements, Layer, AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(FVector2f(PosX, Y - TextSize.Y * 0.5f))),
			Text, LabelFont, ESlateDrawEffect::None, AxisColor * Tint);
	};

	const int32 GridLayer = LayerId + 1;
	const int32 LineLayer = LayerId + 2;
	// Horizontal grid + degree labels.
	for (double V = MinV; V <= MaxV + VStep * 0.01; V += VStep)
	{
		const float Y = ToY(V);
		TArray<FVector2f> Points = { FVector2f(Left, Y), FVector2f(Left + PlotW, Y) };
		FSlateDrawElement::MakeLines(OutDrawElements, GridLayer, PaintGeometry, Points, ESlateDrawEffect::None,
			(FMath::IsNearlyZero(V) ? AxisColor * 0.5f : GridColor) * Tint, true, 1.f);
		DrawText(FString::Printf(TEXT("%d°"), FMath::RoundToInt32(V)), Left - 5.f, Y, true, GridLayer);
	}
	// Time labels along the bottom.
	const double TStep = NiceStep(TimeMax - TimeMin, 5);
	for (double T = std::ceil(TimeMin / TStep) * TStep; T <= TimeMax + 1e-6; T += TStep)
	{
		const float X = ToX(T);
		TArray<FVector2f> Points = { FVector2f(X, Top), FVector2f(X, Top + PlotH) };
		FSlateDrawElement::MakeLines(OutDrawElements, GridLayer, PaintGeometry, Points, ESlateDrawEffect::None, GridColor * Tint, true, 1.f);
		DrawText(TStep < 1.0 ? FString::Printf(TEXT("%.1f s"), T) : FString::Printf(TEXT("%.0f s"), T), X, Top + PlotH + 10.f, false, GridLayer);
	}

	// Lines, broken where values are missing.
	for (const FPoseGraphLine& Line : Lines)
	{
		TArray<FVector2f> Run;
		auto Flush = [&]()
		{
			if (Run.Num() >= 2)
			{
				FSlateDrawElement::MakeLines(OutDrawElements, LineLayer, PaintGeometry, Run, ESlateDrawEffect::None, Line.Color * Tint, true, Line.Thickness);
			}
			else if (Run.Num() == 1)   // a lone value between gaps: a short tick so it stays visible
			{
				TArray<FVector2f> Tick = { Run[0] - FVector2f(1.5f, 0.f), Run[0] + FVector2f(1.5f, 0.f) };
				FSlateDrawElement::MakeLines(OutDrawElements, LineLayer, PaintGeometry, Tick, ESlateDrawEffect::None, Line.Color * Tint, true, Line.Thickness + 1.f);
			}
			Run.Reset();
		};
		for (int32 I = 0; I < Line.Values.Num() && I < Line.Times.Num(); ++I)
		{
			if (std::isnan(Line.Values[I]))
			{
				Flush();
				continue;
			}
			Run.Add(FVector2f(ToX(Line.Times[I]), ToY(Line.Values[I])));
		}
		Flush();
	}
	return LineLayer;
}

// ================================================================================================
// Row
// ================================================================================================

void UCPP_UW_PoseAngleRow::Setup(UCPP_UW_PoseDataPanel* InPanel, int32 InAngleIndex, const FString& InName, const FSlateFontInfo& InFont)
{
	Panel = InPanel;
	AngleIndex = InAngleIndex;
	AngleName = InName;
	Font = InFont;
}

TSharedRef<SWidget> UCPP_UW_PoseAngleRow::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		RowButton = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("RowButton"));
		RowButton->SetStyle(ButtonStyle(0.f, 8.f));
		RowButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseAngleRow::HandleClicked);
		WidgetTree->RootWidget = RowButton;

		UHorizontalBox* Box = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(RowButton->AddChild(Box)))
		{
			ButtonSlot->SetPadding(FMargin(10.f, 4.f));
			ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
		}

		USizeBox* SwatchSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
		SwatchSize->SetWidthOverride(10.f);
		SwatchSize->SetHeightOverride(10.f);
		Swatch = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass());
		Swatch->SetBrush(Rounded(FLinearColor::White, 5.f));
		Swatch->SetBrushColor(Grey(0.15f));
		SwatchSize->AddChild(Swatch);
		if (UHorizontalBoxSlot* S = Box->AddChildToHorizontalBox(SwatchSize))
		{
			S->SetVerticalAlignment(VAlign_Center);
			S->SetPadding(FMargin(0.f, 0.f, 10.f, 0.f));
		}

		NameText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		NameText->SetFont(Font);
		NameText->SetText(FText::FromString(AngleName));
		NameText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
		if (UHorizontalBoxSlot* S = Box->AddChildToHorizontalBox(NameText))
		{
			S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			S->SetVerticalAlignment(VAlign_Center);
		}

		ValueText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
		ValueText->SetFont(Font);
		ValueText->SetJustification(ETextJustify::Right);
		ValueText->SetText(FText::FromString(TEXT("-")));
		if (UHorizontalBoxSlot* S = Box->AddChildToHorizontalBox(ValueText))
		{
			S->SetVerticalAlignment(VAlign_Center);
		}
	}
	return Super::RebuildWidget();
}

void UCPP_UW_PoseAngleRow::SetValue(const FText& InValue)
{
	if (ValueText)
	{
		ValueText->SetText(InValue);
	}
}

void UCPP_UW_PoseAngleRow::SetSelected(bool bSelected, const FLinearColor& Color)
{
	if (Swatch)
	{
		Swatch->SetBrushColor(bSelected ? Color : Grey(0.15f));
	}
	if (RowButton)
	{
		RowButton->SetStyle(ButtonStyle(bSelected ? 0.14f : 0.f, 8.f));
	}
}

void UCPP_UW_PoseAngleRow::HandleClicked()
{
	if (UCPP_UW_PoseDataPanel* PanelPtr = Panel.Get())
	{
		PanelPtr->ToggleAngle(AngleIndex);
	}
}

// ================================================================================================
// Panel
// ================================================================================================

FSlateFontInfo UCPP_UW_PoseDataPanel::AvenirFont(bool bHeavy, float Size) const
{
	// The HUD's fonts (README/02: Inter Medium -> Avenir Medium, Inter Bold -> Avenir Heavy); Roboto if missing.
	const TCHAR* Path = bHeavy ? TEXT("/Game/UserInterface/Avenir-Font/Avenir/Avenir-Heavy_Font.Avenir-Heavy_Font")
	                           : TEXT("/Game/UserInterface/Avenir-Font/Avenir/Avenir-Medium_Font.Avenir-Medium_Font");
	if (const UObject* FontObject = LoadObject<UFont>(nullptr, Path))
	{
		return FSlateFontInfo(FontObject, Size);
	}
	return FCoreStyle::GetDefaultFontStyle(bHeavy ? "Bold" : "Regular", Size);
}

UButton* UCPP_UW_PoseDataPanel::MakeButton(const FText& Label, UTextBlock*& OutLabel, float Alpha)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
	Button->SetStyle(ButtonStyle(Alpha, 11.f));
	OutLabel = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	OutLabel->SetFont(AvenirFont(true, 11.f));
	OutLabel->SetText(Label);
	OutLabel->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UButtonSlot* ButtonSlot = Cast<UButtonSlot>(Button->AddChild(OutLabel)))
	{
		ButtonSlot->SetPadding(FMargin(16.f, 7.f));
	}
	return Button;
}

TSharedRef<SWidget> UCPP_UW_PoseDataPanel::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

void UCPP_UW_PoseDataPanel::BuildTree()
{
	if (LineColors.IsEmpty())
	{
		LineColors = { HighlightGreen, FLinearColor(FColor(0x5B, 0xB8, 0xF0)), FLinearColor(FColor(0xF2, 0xA6, 0x3B)), FLinearColor(FColor(0xE0, 0x7B, 0xD6)) };
	}

	// Full-screen canvas (clicks pass through it) holding the panel docked on the right.
	UCanvasPanel* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Canvas"));
	Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Canvas;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("PanelBackground"));
	Panel->SetBrush(Rounded(PanelColor, 29.f));
	Panel->SetPadding(FMargin(26.f, 20.f, 26.f, 22.f));
	if (UCanvasPanelSlot* PanelSlot = Canvas->AddChildToCanvas(Panel))
	{
		PanelSlot->SetAnchors(FAnchors(1.f, 0.f, 1.f, 1.f));
		PanelSlot->SetOffsets(FMargin(-(PanelWidth + PanelMargins.Right), PanelMargins.Top, PanelWidth, PanelMargins.Bottom));
	}

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass());
	Panel->SetContent(Column);
	auto Add = [Column](UWidget* Widget, const FMargin& SlotPadding, bool bFill = false)
	{
		UVerticalBoxSlot* VSlot = Column->AddChildToVerticalBox(Widget);
		VSlot->SetPadding(SlotPadding);
		if (bFill)
		{
			VSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}
		return VSlot;
	};

	// Title + close.
	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	Title->SetFont(AvenirFont(true, 16.f));
	Title->SetText(LOCTEXT("Title", "POSE DATA"));
	Title->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	if (UHorizontalBoxSlot* S = Header->AddChildToHorizontalBox(Title))
	{
		S->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		S->SetVerticalAlignment(VAlign_Center);
	}
	UTextBlock* CloseLabel = nullptr;
	UButton* CloseButton = MakeButton(FText::FromString(TEXT("×")), CloseLabel, 0.2f);
	CloseLabel->SetFont(AvenirFont(true, 14.f));
	CloseButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseDataPanel::HandleClose);
	Header->AddChildToHorizontalBox(CloseButton);
	Add(Header, FMargin(0.f, 0.f, 0.f, 4.f));

	StatusText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	StatusText->SetFont(AvenirFont(false, 10.f));
	StatusText->SetColorAndOpacity(FSlateColor(Grey(0.85f)));
	StatusText->SetAutoWrapText(true);
	Add(StatusText, FMargin(0.f, 0.f, 0.f, 10.f));

	// Live / last recording.
	UHorizontalBox* Views = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UTextBlock* LiveLabel = nullptr;
	LiveButton = MakeButton(LOCTEXT("Live", "Live"), LiveLabel, 0.2f);
	LiveButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseDataPanel::HandleLive);
	Views->AddChildToHorizontalBox(LiveButton)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	UTextBlock* RecordingViewLabel = nullptr;
	RecordingViewButton = MakeButton(LOCTEXT("LastRecording", "Last recording"), RecordingViewLabel, 0.2f);
	RecordingViewButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseDataPanel::HandleLastRecording);
	Views->AddChildToHorizontalBox(RecordingViewButton);
	Add(Views, FMargin(0.f, 0.f, 0.f, 8.f));

	GraphCaption = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	GraphCaption->SetFont(AvenirFont(false, 9.f));
	GraphCaption->SetColorAndOpacity(FSlateColor(Grey(0.7f)));
	GraphCaption->SetAutoWrapText(true);
	Add(GraphCaption, FMargin(0.f, 0.f, 0.f, 4.f));

	USizeBox* GraphSize = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass());
	GraphSize->SetHeightOverride(230.f);
	Graph = WidgetTree->ConstructWidget<UCPP_UW_PoseAngleGraph>(UCPP_UW_PoseAngleGraph::StaticClass(), TEXT("AngleGraph"));
	Graph->LabelFont = AvenirFont(false, 8.f);
	GraphSize->AddChild(Graph);
	Add(GraphSize, FMargin(0.f, 0.f, 0.f, 12.f));

	UTextBlock* ListTitle = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	ListTitle->SetFont(AvenirFont(true, 11.f));
	ListTitle->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	ListTitle->SetText(LOCTEXT("ListTitle", "ANGLES  (degrees, click to show on the graph)"));
	Add(ListTitle, FMargin(0.f, 0.f, 0.f, 4.f));

	// The 26 angles, in Sports2D's order, with a heading for each group.
	UScrollBox* List = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass());
	const TConstArrayView<PoseSports2D::FAngleDefinition> Definitions = PoseSports2D::GetAngleDefinitions();
	const FSlateFontInfo RowFont = AvenirFont(false, 10.f);
	bool bLastJoint = false;
	for (int32 Index = 0; Index < Definitions.Num(); ++Index)
	{
		if (Index == 0 || Definitions[Index].bJoint != bLastJoint)
		{
			bLastJoint = Definitions[Index].bJoint;
			UTextBlock* Group = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
			Group->SetFont(AvenirFont(true, 9.f));
			Group->SetColorAndOpacity(FSlateColor(HighlightGreen));
			Group->SetText(bLastJoint ? LOCTEXT("JointAngles", "JOINT ANGLES") : LOCTEXT("SegmentAngles", "SEGMENT ANGLES  (vs. horizontal)"));
			List->AddChild(Group);
		}
		UCPP_UW_PoseAngleRow* Row = WidgetTree->ConstructWidget<UCPP_UW_PoseAngleRow>(UCPP_UW_PoseAngleRow::StaticClass());
		Row->Setup(this, Index, Definitions[Index].Name, RowFont);
		List->AddChild(Row);
		Rows.Add(Row);
	}
	Add(List, FMargin(0.f, 0.f, 0.f, 12.f), /*bFill*/ true);

	// Record / open folder / last saved.
	UHorizontalBox* Footer = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
	UTextBlock* RecordText = nullptr;
	UButton* RecordButton = MakeButton(LOCTEXT("Record", "●  Record"), RecordText, 0.2f);
	RecordLabel = RecordText;
	RecordButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseDataPanel::HandleRecord);
	Footer->AddChildToHorizontalBox(RecordButton)->SetPadding(FMargin(0.f, 0.f, 8.f, 0.f));
	UTextBlock* FolderLabel = nullptr;
	UButton* FolderButton = MakeButton(LOCTEXT("OpenFolder", "Open folder"), FolderLabel, 0.2f);
	FolderButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseDataPanel::HandleOpenFolder);
	Footer->AddChildToHorizontalBox(FolderButton);
	Add(Footer, FMargin(0.f, 0.f, 0.f, 6.f));

	SavedText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass());
	SavedText->SetFont(AvenirFont(false, 9.f));
	SavedText->SetColorAndOpacity(FSlateColor(Grey(0.7f)));
	SavedText->SetAutoWrapText(true);
	Add(SavedText, FMargin(0.f));

	// Default graph selection.
	for (const FName& AngleName : DefaultAngles)
	{
		for (int32 Index = 0; Index < Definitions.Num(); ++Index)
		{
			if (AngleName == FName(Definitions[Index].Name) && SelectedAngles.Num() < LineColors.Num())
			{
				SelectedAngles.Add(Index);
			}
		}
	}
	if (const UPoseEstimationComp* Comp = PoseComponent.Get())
	{
		ShownRecordingFolder = Comp->GetLastRecordingFolder();   // open on the live view, even if a recording exists
	}
	TimeSinceRefresh = RefreshInterval;   // refresh on the first tick
}

void UCPP_UW_PoseDataPanel::ToggleAngle(int32 AngleIndex)
{
	if (SelectedAngles.Contains(AngleIndex))
	{
		SelectedAngles.Remove(AngleIndex);
	}
	else
	{
		if (SelectedAngles.Num() >= LineColors.Num())
		{
			SelectedAngles.RemoveAt(0);   // full: the oldest choice makes room
		}
		SelectedAngles.Add(AngleIndex);
	}
	Refresh();
}

void UCPP_UW_PoseDataPanel::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	TimeSinceRefresh += InDeltaTime;
	if (TimeSinceRefresh >= RefreshInterval)
	{
		TimeSinceRefresh = 0.f;
		Refresh();
	}
}

void UCPP_UW_PoseDataPanel::Refresh()
{
	const UPoseEstimationComp* Comp = PoseComponent.Get();
	if (!Comp || !StatusText)
	{
		return;
	}

	// Status line.
	FString Status;
	switch (Comp->GetStatus())
	{
	case EPoseEstimationStatus::LoadingModels: Status = TEXT("Loading the pose models..."); break;
	case EPoseEstimationStatus::Error:         Status = TEXT("Pose estimation unavailable: ") + Comp->GetLastError(); break;
	case EPoseEstimationStatus::Off:           Status = TEXT("Pose estimation is off."); break;
	default:
	{
		const FPoseFrameResult& Result = Comp->GetLatestResult();
		if (const FPosePerson* Person = Result.GetPrimary())
		{
			int32 ValidAngles = 0;
			for (const FPoseAngle& Angle : Person->Angles)
			{
				ValidAngles += Angle.bValid ? 1 : 0;
			}
			static const TCHAR* Sides[] = { TEXT("auto"), TEXT("right"), TEXT("left"), TEXT("front"), TEXT("back"), TEXT("none") };
			Status = FString::Printf(TEXT("Analysing %.0f frames/s  ·  person facing %s  ·  %d of %d keypoints  ·  %d angles"),
				Comp->GetAnalysesPerSecond(), Sides[static_cast<int32>(Person->VisibleSide)], Person->NumValidKeypoints, Person->Keypoints.Num(), ValidAngles);
		}
		else
		{
			Status = FString::Printf(TEXT("Analysing %.0f frames/s  ·  nobody detected"), Comp->GetAnalysesPerSecond());
		}
	}
	}
	if (Comp->IsRecording())
	{
		Status += FString::Printf(TEXT("  ·  RECORDING %d:%02d"), FMath::FloorToInt32(Comp->GetRecordingSeconds() / 60.f), FMath::FloorToInt32(Comp->GetRecordingSeconds()) % 60);
	}
	StatusText->SetText(FText::FromString(Status));

	RecordLabel->SetText(Comp->IsRecording() ? LOCTEXT("Stop", "■  Stop") : LOCTEXT("Record", "●  Record"));
	RecordLabel->SetColorAndOpacity(FSlateColor(Comp->IsRecording() ? RecordRed : FLinearColor::White));
	const FString Folder = Comp->GetLastRecordingFolder();
	if (Folder != ShownRecordingFolder)
	{
		ShownRecordingFolder = Folder;
		bShowLastRecording = !Folder.IsEmpty();   // a recording was just saved: show it
	}
	SavedText->SetText(Folder.IsEmpty()
		? LOCTEXT("NoRecording", "Record with this button or the timeline's Record button. Recordings are saved in Sports2D's formats (.mot angles, .trc keypoints) and as .csv.")
		: FText::Format(LOCTEXT("Saved", "Last recording: {0}"), FText::FromString(Folder)));

	LiveButton->SetStyle(ButtonStyle(bShowLastRecording ? 0.12f : 0.34f, 11.f));
	RecordingViewButton->SetStyle(ButtonStyle(bShowLastRecording ? 0.34f : 0.12f, 11.f));

	RefreshRows();
	RefreshGraph();
}

void UCPP_UW_PoseDataPanel::RefreshRows()
{
	const UPoseEstimationComp* Comp = PoseComponent.Get();
	const FPosePerson* Person = Comp ? Comp->GetLatestResult().GetPrimary() : nullptr;
	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		FText Value = FText::FromString(TEXT("-"));
		if (Person && Person->Angles.IsValidIndex(Index) && Person->Angles[Index].bValid)
		{
			Value = FText::FromString(FString::Printf(TEXT("%.1f"), Person->Angles[Index].Degrees));
		}
		Rows[Index]->SetValue(Value);
		const int32 ColorSlot = SelectedAngles.IndexOfByKey(Index);
		Rows[Index]->SetSelected(ColorSlot != INDEX_NONE, LineColors.IsValidIndex(ColorSlot) ? LineColors[ColorSlot] : FLinearColor::White);
	}
}

void UCPP_UW_PoseDataPanel::RefreshGraph()
{
	const UPoseEstimationComp* Comp = PoseComponent.Get();
	if (!Comp || !Graph)
	{
		return;
	}
	TArray<FPoseGraphLine> Lines;
	const FPoseProcessedRecording* Recording = Comp->GetLastRecording();
	if (bShowLastRecording && Recording && Recording->Time.Num() > 1)
	{
		// Like Sports2D's angle plots: post-processed (solid) over raw (faint).
		for (int32 ColorSlot = 0; ColorSlot < SelectedAngles.Num(); ++ColorSlot)
		{
			const int32 Angle = SelectedAngles[ColorSlot];
			if (!Recording->Angles.IsValidIndex(Angle))
			{
				continue;
			}
			FPoseGraphLine Raw;
			Raw.Color = LineColors[ColorSlot] * FLinearColor(1.f, 1.f, 1.f, 0.35f);
			Raw.Thickness = 1.f;
			Raw.Times = Recording->Time;
			Raw.Values = Recording->AnglesRaw[Angle];
			Lines.Add(MoveTemp(Raw));
			FPoseGraphLine Filtered;
			Filtered.Color = LineColors[ColorSlot];
			Filtered.Times = Recording->Time;
			Filtered.Values = Recording->Angles[Angle];
			Lines.Add(MoveTemp(Filtered));
		}
		GraphCaption->SetText(FText::Format(LOCTEXT("RecordingCaption", "Last recording: {0} s at {1} frames/s. Solid: after Sports2D's post-processing (gap filling, outliers, Butterworth {2} Hz). Faint: raw."),
			FText::AsNumber(FMath::RoundToInt32(Recording->Time.Last())), FText::AsNumber(FMath::RoundToInt32(Recording->FrameRate)), FText::AsNumber(FMath::RoundToInt32(Recording->CutoffHz))));
		Graph->SetLines(MoveTemp(Lines), 0.0, Recording->Time.Last());
		return;
	}

	// Live: the last LiveHistorySeconds of raw values, each analysed frame at its own time (seconds before now).
	const TArray<FPoseRecordingSample>& Samples = Comp->GetLiveHistory().GetSamples();
	const double End = Samples.Num() ? Samples.Last().Time : 0.0;
	const double Window = Comp->LiveHistorySeconds;
	for (int32 ColorSlot = 0; ColorSlot < SelectedAngles.Num(); ++ColorSlot)
	{
		FPoseGraphLine Line;
		Line.Color = LineColors[ColorSlot];
		for (const FPoseRecordingSample& Sample : Samples)
		{
			Line.Times.Add(Sample.Time - End);
			Line.Values.Add(Sample.Angles[SelectedAngles[ColorSlot]]);
		}
		Lines.Add(MoveTemp(Line));
	}
	GraphCaption->SetText(bShowLastRecording
		? LOCTEXT("NoRecordingYet", "No recording yet: showing live values. Press Record to make one.")
		: FText::Format(LOCTEXT("LiveCaption", "Live: the last {0} seconds, raw values (no smoothing), as on the overlay."), FText::AsNumber(FMath::RoundToInt32(Window))));
	Graph->SetLines(MoveTemp(Lines), -Window, 0.0);
}

void UCPP_UW_PoseDataPanel::HandleClose()
{
	if (UPoseEstimationComp* Comp = PoseComponent.Get())
	{
		Comp->SetDataPanelOpen(false);
	}
}

void UCPP_UW_PoseDataPanel::HandleLive()
{
	bShowLastRecording = false;
	Refresh();
}

void UCPP_UW_PoseDataPanel::HandleLastRecording()
{
	bShowLastRecording = true;
	Refresh();
}

void UCPP_UW_PoseDataPanel::HandleRecord()
{
	if (UPoseEstimationComp* Comp = PoseComponent.Get())
	{
		const bool bWasRecording = Comp->IsRecording();
		Comp->ToggleRecording();
		if (bWasRecording && !Comp->IsRecording())
		{
			bShowLastRecording = true;   // show what was just recorded
		}
		Refresh();
	}
}

void UCPP_UW_PoseDataPanel::HandleOpenFolder()
{
	const UPoseEstimationComp* Comp = PoseComponent.Get();
	FString Folder = Comp ? Comp->GetLastRecordingFolder() : FString();
	if (Folder.IsEmpty())
	{
		Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PoseEstimation") / TEXT("Recordings"));
		IFileManager::Get().MakeDirectory(*Folder, true);
	}
	FPlatformProcess::ExploreFolder(*Folder);
}

#undef LOCTEXT_NAMESPACE
