#include "PoseEstimation/CPP_UW_PoseButtons.h"

#include "PoseEstimation/PoseEstimationComp.h"
#include "PoseEstimation/PoseTypes.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"

#define LOCTEXT_NAMESPACE "PoseButtons"

namespace PoseButtonsPrivate
{
	const TCHAR* PoseButtonTextureFolder = TEXT("/Game/UserInterface/IHPC/Textures/");

	TSoftObjectPtr<UTexture2D> PoseButtonTexture(const TCHAR* Name)
	{
		return TSoftObjectPtr<UTexture2D>(FSoftObjectPath(FString::Printf(TEXT("%s%s.%s"), PoseButtonTextureFolder, Name, Name)));
	}

	FSlateBrush PoseButtonRoundedBrush(const FLinearColor& Color, float Radius)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.Width = 0.f;
		return Brush;
	}

	FLinearColor PoseButtonScaleColor(const FLinearColor& Color, float Tint)
	{
		return FLinearColor(Color.R * Tint, Color.G * Tint, Color.B * Tint, Color.A);
	}
}

using namespace PoseButtonsPrivate;

UCPP_UW_PoseButtons::UCPP_UW_PoseButtons(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	OverlayOnTexture = PoseButtonTexture(TEXT("T_IHPC_PB_Overlay_On"));
	OverlayOffTexture = PoseButtonTexture(TEXT("T_IHPC_PB_Overlay_Off"));
	DataOnTexture = PoseButtonTexture(TEXT("T_IHPC_PB_Data_On"));
	DataOffTexture = PoseButtonTexture(TEXT("T_IHPC_PB_Data_Off"));
}

TSharedRef<SWidget> UCPP_UW_PoseButtons::RebuildWidget()
{
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		BuildTree();
	}
	return Super::RebuildWidget();
}

UButton* UCPP_UW_PoseButtons::MakeRoundButton(const FName& Name)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
	// Like the layer buttons ("Is Focusable" off in the designer): clicking must not take keyboard focus from the game.
	// The setter is protected (it must be set before the Slate button exists), so set the property the way the
	// designer does.
	if (FBoolProperty* Focusable = FindFProperty<FBoolProperty>(UButton::StaticClass(), TEXT("IsFocusable")))
	{
		Focusable->SetPropertyValue_InContainer(Button, false);
	}
	Button->SetBackgroundColor(FLinearColor::White);
	return Button;
}

void UCPP_UW_PoseButtons::BuildTree()
{
	// Full-screen canvas that lets clicks through everywhere except on the pill.
	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("Root"));
	Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	WidgetTree->RootWidget = Root;

	// The pill: right edge of the screen, vertically centred. Same shape as the layer switcher's, standing up.
	const float PillLength = PillThickness + ButtonSpacing;
	UCanvasPanel* Pill = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("PoseButtons"));
	Pill->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (UCanvasPanelSlot* PillSlot = Root->AddChildToCanvas(Pill))
	{
		PillSlot->SetAnchors(FAnchors(1.f, 0.5f, 1.f, 0.5f));
		PillSlot->SetAlignment(FVector2D(1.f, 0.5f));
		PillSlot->SetPosition(FVector2D(-RightMargin, VerticalOffset));
		PillSlot->SetSize(FVector2D(PillThickness, PillLength));
	}

	// Background: Visible, so a click between the buttons doesn't reach the 3D view (as on the layer switcher).
	UImage* Background = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("PillBackground"));
	Background->SetBrush(PoseButtonRoundedBrush(PillColor, PillThickness * 0.5f));
	Background->SetVisibility(ESlateVisibility::Visible);
	if (UCanvasPanelSlot* BackgroundSlot = Pill->AddChildToCanvas(Background))
	{
		BackgroundSlot->SetPosition(FVector2D::ZeroVector);
		BackgroundSlot->SetSize(FVector2D(PillThickness, PillLength));
	}

	OverlayButton = MakeRoundButton(TEXT("PoseOverlay_Btn"));
	DataButton = MakeRoundButton(TEXT("PoseData_Btn"));
	const float Inset = (PillThickness - ButtonSize) * 0.5f;
	int32 Index = 0;
	for (UButton* Button : { OverlayButton.Get(), DataButton.Get() })
	{
		if (UCanvasPanelSlot* ButtonSlot = Pill->AddChildToCanvas(Button))
		{
			ButtonSlot->SetPosition(FVector2D(Inset, Inset + Index * ButtonSpacing));
			ButtonSlot->SetSize(FVector2D(ButtonSize, ButtonSize));
		}
		++Index;
	}
	OverlayButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseButtons::HandleOverlayClicked);
	DataButton->OnClicked.AddUniqueDynamic(this, &UCPP_UW_PoseButtons::HandleDataClicked);

	Refresh(/*bForce*/ true);
}

void UCPP_UW_PoseButtons::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	Refresh(/*bForce*/ false);
}

void UCPP_UW_PoseButtons::HandleOverlayClicked()
{
	if (UPoseEstimationComp* Comp = PoseComponent.Get())
	{
		Comp->SetEstimationEnabled(!Comp->IsEstimationEnabled());
	}
	Refresh(false);
}

void UCPP_UW_PoseButtons::HandleDataClicked()
{
	if (UPoseEstimationComp* Comp = PoseComponent.Get())
	{
		Comp->SetDataPanelOpen(!Comp->IsDataPanelOpen());
	}
	Refresh(false);
}

void UCPP_UW_PoseButtons::Refresh(bool bForce)
{
	if (!OverlayButton || !DataButton)
	{
		return;
	}
	const UPoseEstimationComp* Comp = PoseComponent.Get();
	const int32 Overlay = Comp && Comp->IsEstimationEnabled() ? 1 : 0;
	const int32 Data = Comp && Comp->IsDataPanelOpen() ? 1 : 0;
	if (bForce || Overlay != ShownOverlay)
	{
		ShownOverlay = Overlay;
		ApplyLook(OverlayButton, Overlay ? OverlayOnTexture : OverlayOffTexture, Overlay != 0);
	}
	if (bForce || Data != ShownData)
	{
		ShownData = Data;
		ApplyLook(DataButton, Data ? DataOnTexture : DataOffTexture, Data != 0);
	}

	// Tooltips: what the button does, plus "loading" or the reason it can't start.
	FString Extra;
	if (Comp)
	{
		switch (Comp->GetStatus())
		{
		case EPoseEstimationStatus::LoadingModels: Extra = TEXT("Loading the pose models (a second or two)..."); break;
		case EPoseEstimationStatus::Error:         Extra = TEXT("Pose estimation could not start: ") + Comp->GetLastError(); break;
		default: break;
		}
	}
	if (bForce || Extra != ShownTooltipState)
	{
		ShownTooltipState = Extra;
		const FText Suffix = Extra.IsEmpty() ? FText::GetEmpty() : FText::FromString(TEXT("\n\n") + Extra);
		OverlayButton->SetToolTipText(FText::Format(LOCTEXT("OverlayTip", "Pose Overlay\nShow or hide the pose skeleton and the joint angles on the person.{0}"), Suffix));
		DataButton->SetToolTipText(FText::Format(LOCTEXT("DataTip", "Pose Data\nOpen or close the joint angle values, graphs and recording.{0}"), Suffix));
	}
}

void UCPP_UW_PoseButtons::ApplyLook(UButton* Button, const TSoftObjectPtr<UTexture2D>& Texture, bool bOn)
{
	UTexture2D* Picture = Texture.LoadSynchronous();
	if (!Picture && !bWarnedMissingTexture)
	{
		bWarnedMissingTexture = true;
		UE_LOG(LogPoseEstimation, Warning, TEXT("Pose buttons: picture %s not found, drawing plain circles. Import it with Tools/PoseEstimation/ue_import_pose_textures.py."),
			*Texture.ToString());
	}

	// Without the picture: a plain circle in the same colours (dark = on, light blue = off).
	const FLinearColor Fallback = bOn ? FLinearColor(FColor(0x1F, 0x4E, 0x70)) : FLinearColor(FColor(0xB0, 0xCD, 0xE3));
	auto MakeBrush = [&](float Tint)
	{
		if (!Picture)
		{
			return PoseButtonRoundedBrush(PoseButtonScaleColor(Fallback, Tint), ButtonSize * 0.5f);
		}
		FSlateBrush Brush;
		Brush.SetResourceObject(Picture);
		Brush.ImageSize = FVector2D(ButtonSize, ButtonSize);
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.TintColor = FSlateColor(FLinearColor(Tint, Tint, Tint, 1.f));
		return Brush;
	};

	// Same as UCPP_UW_IHPCLayerSwitcher::ApplyButtonLook.
	FButtonStyle Style = Button->GetStyle();
	Style.SetNormal(MakeBrush(1.f));
	Style.SetHovered(MakeBrush(HoveredTint));
	Style.SetPressed(MakeBrush(PressedTint));
	Style.SetDisabled(MakeBrush(0.5f));
	Style.SetNormalPadding(FMargin(0.f));
	Style.SetPressedPadding(FMargin(0.f));
	Button->SetStyle(Style);
}

#undef LOCTEXT_NAMESPACE
