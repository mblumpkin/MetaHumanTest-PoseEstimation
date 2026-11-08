#include "CPP_PlayerController.h"

#include "CPP_MouseFunctionComp.h"
#include "CPP_PlayerPlaceHolderManagerA.h"
#include "CPP_UW_AnatomyViewSwitcher.h"
#include "CPP_UW_MuscleInfo.h"
#include "AnatomyPickerComp.h"
#include "CPP_UW_PawnSwitcher.h"
#include "CPP_UW_SpeedSlider.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "WCPP_AnimationTimeline.h"
#include "IHPC_UI/CPP_UW_IHPCHud.h"
#include "PoseEstimation/PoseEstimationComp.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"

ACPP_PlayerController::ACPP_PlayerController()
{
	// On the controller (not the pawn) so it keeps running when the possessed pawn changes.
	PoseEstimation = CreateDefaultSubobject<UPoseEstimationComp>(TEXT("PoseEstimation"));
}

void ACPP_PlayerController::BeginPlay()
{
	Super::BeginPlay(); // was missing: BP_PlayerController's own BeginPlay never ran

	bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	SetInputMode(InputMode);

	if (ULocalPlayer* LP = GetLocalPlayer())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsys = LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>())
		{
			if (MouseManager)
			{
				Subsys->AddMappingContext(MouseManager, /*Priority*/ 1);
			}
		}
	}

	// UI: the controller only creates the widgets; each widget finds its component on the possessed pawn
	// and re-binds by itself when the possessed pawn changes.
	SpeedSliderWidgetInstance = AddWidget(SpeedSliderWidget);
	TimelineWidget = AddWidget(TimelineWidgetClass);
	AnatomyViewSwitcherWidget = AddWidget(AnatomyViewSwitcherWidgetClass);
	PawnSwitcherWidget = AddWidget(PawnSwitcherWidgetClass);
	MuscleInfoWidget = AddWidget(MuscleInfoWidgetClass, /*ZOrder*/ 20);
	// IHPC HUD (title bar, menu, analysis panel, timeline bar), drawn above the other widgets.
	MainHUDWidget = AddWidget(MainHUDWidgetClass, MainHUDZOrder);

	// Placeholders (was looking for APlayerCameraManager, so the manager was never found).
	if (ACPP_PlayerPlaceHolderManagerA* PlaceHolderManager = Cast<ACPP_PlayerPlaceHolderManagerA>(
		UGameplayStatics::GetActorOfClass(GetWorld(), ACPP_PlayerPlaceHolderManagerA::StaticClass())))
	{
		PC_PlayerPlaceHolders.Reset();
		for (AActor* PlaceHolder : PlaceHolderManager->PlayerPlaceHolders)
		{
			PC_PlayerPlaceHolders.Add(PlaceHolder);
		}
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("No PlayerPlaceholderManager found in level!"));
	}

	// The pawn the game mode gave us is the first switchable pawn of its class.
	SwitchablePawns.SetNum(SwitchablePawnClasses.Num());
	if (APawn* Current = GetPawn())
	{
		const int32 Index = SwitchablePawnClasses.IndexOfByPredicate([Current](const TSubclassOf<APawn>& Class) { return Class && Current->IsA(Class); });
		if (Index != INDEX_NONE)
		{
			SwitchablePawns[Index] = Current;
			CurrentPawnIndex = Index;
		}
	}
}

template <typename WidgetT>
WidgetT* ACPP_PlayerController::AddWidget(TSubclassOf<WidgetT> WidgetClass, int32 ZOrder)
{
	if (!WidgetClass)
	{
		return nullptr;
	}
	WidgetT* Widget = CreateWidget<WidgetT>(this, WidgetClass);
	if (Widget)
	{
		Widget->AddToViewport(ZOrder);
	}
	return Widget;
}

void ACPP_PlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	if (UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (MouseMove)
		{
			EIC->BindAction(MouseMove, ETriggerEvent::Triggered, this, &ACPP_PlayerController::HandleMouseMove);
		}
		if (MouseZoom)
		{
			EIC->BindAction(MouseZoom, ETriggerEvent::Triggered, this, &ACPP_PlayerController::HandleMouseZoom);
		}
	}
}

void ACPP_PlayerController::HandleMouseMove(const FInputActionValue& Value)
{
	const FVector2D MouseDelta = Value.Get<FVector2D>();

	if (UCPP_MouseFunctionComp* MouseComp = GetMouseFunctionComp())
	{
		if (IsInputKeyDown(EKeys::RightMouseButton))
		{
			MouseComp->OrbitFromMouse(MouseDelta); // rotate / orbit
		}
		else if (IsInputKeyDown(EKeys::LeftMouseButton))
		{
			MouseComp->PanFromMouse(MouseDelta); // pan / relocate camera
		}
	}
}

void ACPP_PlayerController::HandleMouseZoom(const FInputActionValue& Value)
{
	if (UCPP_MouseFunctionComp* MouseComp = GetMouseFunctionComp())
	{
		MouseComp->ZoomCamera(Value.Get<float>());
	}
}

void ACPP_PlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	UpdateMusclePicking();
}

void ACPP_PlayerController::UpdateMusclePicking()
{
	// One picker per pickable layer (muscles, bones...). Each one only picks while its own layer is the one
	// you're looking at (UAnatomyPickerComp::IsPickingActive), so at most one of them reacts at a time.
	TArray<UAnatomyPickerComp*> Pickers;
	if (APawn* CurrentPawn = GetPawn())
	{
		CurrentPawn->GetComponents<UAnatomyPickerComp>(Pickers);
	}
	if (Pickers.Num() == 0)
	{
		return;
	}

	// Hover: the ray from the camera through the mouse (not while orbiting with the right button).
	FVector RayOrigin, RayDirection;
	if (!IsInputKeyDown(EKeys::RightMouseButton) && DeprojectMousePositionToWorld(RayOrigin, RayDirection))
	{
		for (UAnatomyPickerComp* Picker : Pickers)
		{
			Picker->UpdateHover(RayOrigin, RayDirection); // an inactive picker clears its own hover
		}
	}

	// Click (press + release without dragging, since a left drag pans the camera) selects.
	FVector2D Mouse;
	if (GetMousePosition(Mouse.X, Mouse.Y))
	{
		if (WasInputKeyJustPressed(EKeys::LeftMouseButton))
		{
			LeftPressPosition = Mouse;
		}
		else if (WasInputKeyJustReleased(EKeys::LeftMouseButton) && FVector2D::Distance(Mouse, LeftPressPosition) <= ClickMaxDragPixels)
		{
			for (UAnatomyPickerComp* Picker : Pickers)
			{
				if (Picker->IsPickingActive())
				{
					Picker->SelectHovered();
				}
			}
		}
	}
}

UCPP_MouseFunctionComp* ACPP_PlayerController::GetMouseFunctionComp() const
{
	const APawn* CurrentPawn = GetPawn();
	return CurrentPawn ? CurrentPawn->FindComponentByClass<UCPP_MouseFunctionComp>() : nullptr;
}

void ACPP_PlayerController::SwitchPawn(int32 Direction)
{
	const int32 Num = SwitchablePawnClasses.Num();
	if (Num > 0)
	{
		SwitchToPawnIndex(((CurrentPawnIndex + Direction) % Num + Num) % Num);
	}
}

void ACPP_PlayerController::SwitchToPawnIndex(int32 Index)
{
	if (!SwitchablePawnClasses.IsValidIndex(Index) || !SwitchablePawnClasses[Index])
	{
		return;
	}
	SwitchablePawns.SetNum(SwitchablePawnClasses.Num());

	APawn* Target = SwitchablePawns[Index];
	if (!IsValid(Target))
	{
		// Spawn at the matching placeholder, or next to the current pawn if there are none.
		FTransform SpawnTransform = GetPawn() ? GetPawn()->GetActorTransform() : FTransform::Identity;
		if (PC_PlayerPlaceHolders.Num() > 0)
		{
			if (const AActor* PlaceHolder = PC_PlayerPlaceHolders[Index % PC_PlayerPlaceHolders.Num()])
			{
				SpawnTransform = PlaceHolder->GetActorTransform();
			}
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Target = GetWorld()->SpawnActor<APawn>(SwitchablePawnClasses[Index], SpawnTransform, Params);
		SwitchablePawns[Index] = Target;
	}

	if (Target && Target != GetPawn())
	{
		// Possess fires OnPossessedPawnChanged -> every widget re-binds to the new pawn's components.
		Possess(Target);
		CurrentPawnIndex = Index;
	}
}
