#include "PoseEstimation/PoseEstimationComp.h"

#include "PoseFrameGrabber.h"
#include "PoseInferenceThread.h"
#include "PoseEstimation/CPP_UW_PoseOverlay.h"
#include "PoseEstimation/CPP_UW_PoseDataPanel.h"
#include "IHPC_UI/CPP_UW_IHPCTimeline.h"
#include "UObject/UObjectIterator.h"
#include "PoseEstimation/PoseSports2D.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SceneViewExtension.h"
#include "UnrealClient.h"

namespace PoseEstimationCompPrivate
{
	constexpr int32 MaxConsecutiveFailures = 10;
	constexpr double StatsWindowSeconds = 3.0;   // Pose.Stats averages over this many seconds
	constexpr double StallSeconds = 1.5;         // no result for this long: log what each stage is doing

	struct FAnalysisSettings
	{
		int32 DetectionFrequency = 4;
		PoseSports2D::FSettings Sports2D;
	};

	/** The pose component of the game / PIE world (the console may hand us the editor world). */
	UPoseEstimationComp* FindPoseComp(UWorld* World)
	{
		auto FromWorld = [](UWorld* InWorld) -> UPoseEstimationComp*
		{
			if (InWorld && InWorld->IsGameWorld())
			{
				if (APlayerController* PC = InWorld->GetFirstPlayerController())
				{
					return PC->FindComponentByClass<UPoseEstimationComp>();
				}
			}
			return nullptr;
		};
		if (UPoseEstimationComp* Comp = FromWorld(World))
		{
			return Comp;
		}
		if (GEngine)
		{
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
			{
				if (UPoseEstimationComp* Comp = FromWorld(Context.World()))
				{
					return Comp;
				}
			}
		}
		return nullptr;
	}

	bool ParseOnOff(const TArray<FString>& Args, bool bCurrent)
	{
		return Args.Num() > 0 ? Args[0].ToBool() : !bCurrent;
	}

	FAutoConsoleCommandWithWorldAndArgs EnableCommand(
		TEXT("Pose.Enable"),
		TEXT("Pose estimation on the live view. Pose.Enable 1 / 0, or no argument to toggle. Needs Play."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				Comp->SetEstimationEnabled(ParseOnOff(Args, Comp->IsEstimationEnabled()));
				UE_LOG(LogPoseEstimation, Display, TEXT("%s"), *Comp->GetStatsText());
			}
			else
			{
				UE_LOG(LogPoseEstimation, Warning, TEXT("No pose estimation component: press Play first."));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs OverlayCommand(
		TEXT("Pose.Overlay"),
		TEXT("Show / hide the pose overlay. Pose.Overlay 1 / 0, or no argument to toggle."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				Comp->SetOverlayVisible(ParseOnOff(Args, Comp->IsOverlayVisible()));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs StatsCommand(
		TEXT("Pose.Stats"),
		TEXT("Log the pose estimation status, rate and timings."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				UE_LOG(LogPoseEstimation, Display, TEXT("%s"), *Comp->GetStatsText());
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs DumpCommand(
		TEXT("Pose.Dump"),
		TEXT("Save the next analysed frame (PNG) and its keypoints (JSON) in Saved/PoseEstimation."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				Comp->DumpNextFrame();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs DataCommand(
		TEXT("Pose.Data"),
		TEXT("Open / close the Pose Data panel (graphs, angle values, recording). Pose.Data 1 / 0, or no argument to toggle."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				Comp->SetDataPanelOpen(ParseOnOff(Args, Comp->IsDataPanelOpen()));
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs RecordCommand(
		TEXT("Pose.Record"),
		TEXT("Start / stop a pose recording (like the timeline's Record button; saved in Saved/PoseEstimation/Recordings)."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UPoseEstimationComp* Comp = FindPoseComp(World))
			{
				Comp->ToggleRecording();
			}
		}));

	FAutoConsoleCommandWithWorldAndArgs TestImageCommand(
		TEXT("Pose.TestImage"),
		TEXT("Pose.TestImage <image> [out.json]: run the detector + pose model on an image file (no Play needed) and write the keypoints as JSON."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (Args.Num() < 1)
			{
				UE_LOG(LogPoseEstimation, Warning, TEXT("Usage: Pose.TestImage <image> [out.json]"));
				return;
			}
			const FString ImagePath = FPaths::ConvertRelativePathToFull(Args[0].TrimQuotes());
			const FString JsonPath = Args.Num() > 1 ? Args[1].TrimQuotes() : FPaths::ChangeExtension(ImagePath, TEXT("unreal.json"));

			FPoseImage Image;
			if (!FPoseEstimator::LoadImageFile(ImagePath, Image))
			{
				UE_LOG(LogPoseEstimation, Error, TEXT("Could not read %s"), *ImagePath);
				return;
			}

			// Loaded for this call only (a second or two): a cached copy would outlive the ONNX runtime at exit.
			const UPoseEstimationComp* Defaults = GetDefault<UPoseEstimationComp>();
			FString Error;
			const TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> TestEstimator = FPoseEstimator::CreateBlocking(
				UPoseEstimationComp::ResolveModelPath(Defaults->DetectorModelFile), UPoseEstimationComp::ResolveModelPath(Defaults->PoseModelFile), Error);
			if (!TestEstimator.IsValid())
			{
				UE_LOG(LogPoseEstimation, Error, TEXT("Could not load the pose models: %s"), *Error);
				return;
			}
			FPoseRawFrame Raw;
			if (!TestEstimator->ProcessFrame(Image, 1, Raw))
			{
				UE_LOG(LogPoseEstimation, Error, TEXT("Pose estimation failed on %s"), *ImagePath);
				return;
			}
			FFileHelper::SaveStringToFile(FPoseEstimator::ToJson(Raw, Image.Width, Image.Height, FPaths::GetCleanFilename(ImagePath)), *JsonPath);
			UE_LOG(LogPoseEstimation, Display, TEXT("%s: %dx%d, %d detection(s), %d person(s), detector %.1f ms, pose %.1f ms -> %s"),
				*FPaths::GetCleanFilename(ImagePath), Image.Width, Image.Height, Raw.Detections.Num(), Raw.Persons.Num(), Raw.DetectorMs, Raw.PoseMs, *JsonPath);
		}));
}

using namespace PoseEstimationCompPrivate;

UPoseEstimationComp::UPoseEstimationComp()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bTickEvenWhenPaused = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;   // after animation: the frame we ask for shows this tick's pose
	OverlayWidgetClass = UCPP_UW_PoseOverlay::StaticClass();
	DataPanelWidgetClass = UCPP_UW_PoseDataPanel::StaticClass();
}

FString UPoseEstimationComp::ResolveModelPath(const FString& File)
{
	return FPaths::IsRelative(File) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), File) : File;
}

void UPoseEstimationComp::BeginPlay()
{
	Super::BeginPlay();

	Grabber = FSceneViewExtensions::NewExtension<FPoseFrameGrabber>();
	InferenceThread = MakeShared<FPoseInferenceThread>();

	// Hands-free test switches (see the header).
	FParse::Value(FCommandLine::Get(), TEXT("PoseLogStats="), AutoLogStatsSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("PoseQuitAfter="), AutoQuitAfterSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("PoseRecordAfter="), AutoRecordAfterSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("PoseRecordFor="), AutoRecordForSeconds);
	FParse::Value(FCommandLine::Get(), TEXT("PoseScreenshotEvery="), AutoScreenshotEverySeconds);
	AutoStartTime = FPlatformTime::Seconds();
	NextAutoLogTime = AutoStartTime + AutoLogStatsSeconds;
	NextAutoScreenshotTime = AutoStartTime + AutoScreenshotEverySeconds;
	const bool bAutoEnable = FParse::Param(FCommandLine::Get(), TEXT("PoseAutoEnable"));
	if (bAutoEnable || AutoLogStatsSeconds > 0.f || AutoQuitAfterSeconds > 0.f)
	{
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation test switches: auto-enable %s, log stats every %.0f s, quit after %.0f s."),
			bAutoEnable ? TEXT("yes") : TEXT("no"), AutoLogStatsSeconds, AutoQuitAfterSeconds);
	}

	if (bEnableOnBeginPlay || bAutoEnable)
	{
		SetEstimationEnabled(true);
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("PoseDataPanel")))
	{
		SetDataPanelOpen(true);
	}
}

void UPoseEstimationComp::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bRecording)
	{
		StopRecording();   // Play stopped while recording: keep what was recorded
	}
	bEnabled = false;
	bRunForRecording = false;
	// Worker threads hold the models: let them finish before the world (and possibly the NNE runtime) goes away.
	if (InferenceThread.IsValid())
	{
		InferenceThread->WaitUntilIdle();
		InferenceThread.Reset();   // stops the thread
	}
	AnalysisOutput.Reset();
	if (LoadTask.IsValid())
	{
		LoadTask.Wait();
	}
	if (Grabber.IsValid())
	{
		Grabber->Shutdown();
		Grabber.Reset();
	}
	if (OverlayWidget)
	{
		OverlayWidget->RemoveFromParent();
		OverlayWidget = nullptr;
	}
	if (DataPanelWidget)
	{
		DataPanelWidget->RemoveFromParent();
		DataPanelWidget = nullptr;
	}
	bDataPanelOpen = false;
	Estimator.Reset();
	Super::EndPlay(EndPlayReason);
}

// ---------------- Control ----------------

void UPoseEstimationComp::SetEstimationEnabled(bool bEnable)
{
	if (bEnable == bEnabled)
	{
		return;
	}
	const bool bWasRunning = IsRunning();
	bEnabled = bEnable;
	OnRunningChanged(bWasRunning);
	RefreshOverlay();
}

void UPoseEstimationComp::OnRunningChanged(bool bWasRunning)
{
	if (IsRunning() == bWasRunning)
	{
		return;
	}
	if (IsRunning())
	{
		bResetTracking = true;   // the previous boxes are stale: start with a detection
		LastEnableTime = FPlatformTime::Seconds();
		LastResultTime = 0.0;
		bStallReported = false;
		NextFrameIndex = 0;
		ConsecutiveFailures = 0;
		RecentResults.Reset();
		if (Estimator.IsValid())
		{
			LastError.Reset();      // e.g. stopped earlier after failed frames: try again
		}
		else
		{
			StartLoadingModels();   // also retries after a failed load
		}
	}
	else
	{
		if (Grabber.IsValid())
		{
			Grabber->Cancel();
		}
		LatestResult = FPoseFrameResult();
		LiveHistory.Reset();
	}
}

void UPoseEstimationComp::SetDataPanelOpen(bool bOpen)
{
	if (bOpen == bDataPanelOpen)
	{
		return;
	}
	const bool bWasRunning = IsRunning();
	bDataPanelOpen = bOpen;
	OnRunningChanged(bWasRunning);

	if (bOpen && !DataPanelWidget && DataPanelWidgetClass)
	{
		APlayerController* PC = Cast<APlayerController>(GetOwner());
		if (!PC && GetWorld())
		{
			PC = GetWorld()->GetFirstPlayerController();
		}
		if (PC && PC->IsLocalController())
		{
			DataPanelWidget = CreateWidget<UCPP_UW_PoseDataPanel>(PC, DataPanelWidgetClass);
			if (DataPanelWidget)
			{
				DataPanelWidget->SetPoseComponent(this);
				DataPanelWidget->AddToViewport(DataPanelZOrder);
			}
		}
	}
	if (DataPanelWidget)
	{
		// The panel's full-screen canvas lets clicks through; only the panel itself is clickable.
		DataPanelWidget->SetVisibility(bOpen ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
	}
}

void UPoseEstimationComp::SetOverlayVisible(bool bVisible)
{
	bOverlayVisible = bVisible;
	RefreshOverlay();
}

EPoseEstimationStatus UPoseEstimationComp::GetStatus() const
{
	if (!LastError.IsEmpty())
	{
		return EPoseEstimationStatus::Error;
	}
	if (!IsRunning())
	{
		return EPoseEstimationStatus::Off;
	}
	return Estimator.IsValid() ? EPoseEstimationStatus::Running : EPoseEstimationStatus::LoadingModels;
}

float UPoseEstimationComp::GetAnalysesPerSecond() const
{
	const double Now = FPlatformTime::Seconds();
	int32 Count = 0;
	for (const FPoseStatSample& Sample : RecentResults)
	{
		Count += (Now - Sample.Time <= StatsWindowSeconds) ? 1 : 0;
	}
	return Count / static_cast<float>(StatsWindowSeconds);
}

FString UPoseEstimationComp::GetStatsText() const
{
	static const TCHAR* StatusNames[] = { TEXT("Off"), TEXT("Loading models"), TEXT("Running"), TEXT("Error") };
	const EPoseEstimationStatus Status = GetStatus();
	FString Text = FString::Printf(TEXT("Pose estimation: %s"), StatusNames[static_cast<int32>(Status)]);
	if (Status == EPoseEstimationStatus::Error)
	{
		return Text + TEXT(" - ") + LastError;
	}
	if (Status == EPoseEstimationStatus::Running && LatestResult.ImageSize.X > 0)
	{
		// Averages over the last few seconds (one frame says little: the detector only runs every Nth frame).
		FPoseStatSample Avg;
		int32 DetectorRuns = 0;
		for (const FPoseStatSample& Sample : RecentResults)
		{
			DetectorRuns += Sample.bDetectorFrame ? 1 : 0;
			Avg.DetectorMs += Sample.bDetectorFrame ? Sample.DetectorMs : 0.f;
			Avg.PoseMs += Sample.PoseMs;
			Avg.CopyMs += Sample.CopyMs;
			Avg.WaitMs += Sample.WaitMs;
			Avg.StartMs += Sample.StartMs;
			Avg.LatencyMs += Sample.LatencyMs;
		}
		const float N = static_cast<float>(FMath::Max(1, RecentResults.Num()));
		Text += FString::Printf(TEXT(" | %.1f frames analysed/s (game %.0f fps) | image %dx%d | detector %.0f ms (every %d frames) | pose %.0f ms")
			TEXT(" | copy %.0f ms, waiting %.0f ms, thread start %.1f ms, analysis %.0f / %.0f ms (detector / other frames), latency %.0f ms | copy timeouts %d | %d person(s)"),
			GetAnalysesPerSecond(), SmoothedFrameSeconds > 0.f ? 1.f / SmoothedFrameSeconds : 0.f, LatestResult.ImageSize.X, LatestResult.ImageSize.Y,
			DetectorRuns ? Avg.DetectorMs / DetectorRuns : 0.f, DetectionFrequency, Avg.PoseMs / N,
			Avg.CopyMs / N, Avg.WaitMs / N, Avg.StartMs / N, AverageAnalysisMs(true), AverageAnalysisMs(false), Avg.LatencyMs / N,
			Grabber.IsValid() ? Grabber->GetNumTimeouts() : 0, LatestResult.Persons.Num());
		Text += TEXT(" | now: ") + DescribePipeline(FPlatformTime::Seconds());
		if (const FPosePerson* Primary = LatestResult.GetPrimary())
		{
			int32 ValidAngles = 0;
			for (const FPoseAngle& Angle : Primary->Angles)
			{
				ValidAngles += Angle.bValid ? 1 : 0;
			}
			static const TCHAR* SideNames[] = { TEXT("auto"), TEXT("right"), TEXT("left"), TEXT("front"), TEXT("back"), TEXT("none") };
			Text += FString::Printf(TEXT(" | main: %d/%d keypoints, mean score %.2f, facing %s, %d angles"), Primary->NumValidKeypoints,
				Primary->Keypoints.Num(), Primary->AverageScore, SideNames[static_cast<int32>(Primary->VisibleSide)], ValidAngles);
		}
		else
		{
			Text += TEXT(" | nobody passes the Sports2D filter");
		}
	}
	return Text;
}

void UPoseEstimationComp::DumpNextFrame()
{
	bDumpNextFrame = true;
	if (!IsRunning())
	{
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose.Dump: will save the next frame once estimation is on (Pose.Enable 1)."));
	}
}

// ---------------- Model loading ----------------

void UPoseEstimationComp::StartLoadingModels()
{
	if (bModelsLoading || Estimator.IsValid())
	{
		return;
	}
	bModelsLoading = true;
	LastError.Reset();
	UE_LOG(LogPoseEstimation, Display, TEXT("Loading pose models..."));

	const FString DetectorPath = ResolveModelPath(DetectorModelFile);
	const FString PosePath = ResolveModelPath(PoseModelFile);
	TWeakObjectPtr<UPoseEstimationComp> WeakThis(this);

	// 1. Read the files on a worker thread (~150 MB).
	LoadTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, DetectorPath, PosePath]()
	{
		TArray64<uint8> DetectorBytes;
		TArray64<uint8> PoseBytes;
		FString Error;
		if (!FFileHelper::LoadFileToArray(DetectorBytes, *DetectorPath))
		{
			Error = FString::Printf(TEXT("Could not read the detector model %s"), *DetectorPath);
		}
		else if (!FFileHelper::LoadFileToArray(PoseBytes, *PosePath))
		{
			Error = FString::Printf(TEXT("Could not read the pose model %s"), *PosePath);
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, LoadedDetector = MoveTemp(DetectorBytes), LoadedPose = MoveTemp(PoseBytes), Error]() mutable
		{
			if (UPoseEstimationComp* This = WeakThis.Get())
			{
				This->OnModelFilesLoaded(MoveTemp(LoadedDetector), MoveTemp(LoadedPose), Error);
			}
		});
	}, UE::Tasks::ETaskPriority::BackgroundNormal);
}

void UPoseEstimationComp::OnModelFilesLoaded(TArray64<uint8>&& DetectorBytes, TArray64<uint8>&& PoseBytes, const FString& Error)
{
	if (!Error.IsEmpty())
	{
		FailLoading(Error);
		return;
	}

	// 2. Hand the bytes to the NNE runtime (needs the game thread; it copies them).
	FString CreateError;
	const TSharedPtr<UE::NNE::IModelCPU> DetectorModel = FPoseEstimator::CreateModelFromOnnx(DetectorBytes, CreateError);
	const TSharedPtr<UE::NNE::IModelCPU> PoseModel = DetectorModel.IsValid() ? FPoseEstimator::CreateModelFromOnnx(PoseBytes, CreateError) : nullptr;
	DetectorBytes.Empty();
	PoseBytes.Empty();
	if (!DetectorModel.IsValid() || !PoseModel.IsValid())
	{
		FailLoading(CreateError);
		return;
	}

	// 3. Create the ONNX Runtime sessions on a worker thread (the slow part).
	TWeakObjectPtr<UPoseEstimationComp> WeakThis(this);
	LoadTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [WeakThis, DetectorModel, PoseModel]()
	{
		TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> NewEstimator = MakeShared<FPoseEstimator, ESPMode::ThreadSafe>();
		FString InitError;
		if (!NewEstimator->Initialize(DetectorModel, PoseModel, InitError))
		{
			NewEstimator.Reset();
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, NewEstimator, InitError]()
		{
			if (UPoseEstimationComp* This = WeakThis.Get())
			{
				This->OnEstimatorInitialized(NewEstimator, InitError);
			}
		});
	}, UE::Tasks::ETaskPriority::BackgroundNormal);
}

void UPoseEstimationComp::OnEstimatorInitialized(const TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe>& NewEstimator, const FString& Error)
{
	if (!NewEstimator.IsValid())
	{
		FailLoading(Error);
		return;
	}
	bModelsLoading = false;
	Estimator = NewEstimator;
	LastEnableTime = FPlatformTime::Seconds();   // the stall watchdog starts counting now, not during loading
	UE_LOG(LogPoseEstimation, Display, TEXT("Pose models loaded."));
}

void UPoseEstimationComp::FailLoading(const FString& Error)
{
	bModelsLoading = false;
	LastError = Error.IsEmpty() ? FString(TEXT("Unknown error while loading the pose models.")) : Error;
	UE_LOG(LogPoseEstimation, Error, TEXT("Pose estimation unavailable: %s"), *LastError);
}

// ---------------- Per frame ----------------

void UPoseEstimationComp::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SmoothedFrameSeconds = SmoothedFrameSeconds > 0.f ? FMath::Lerp(SmoothedFrameSeconds, DeltaTime, 0.05f) : DeltaTime;

	// Hands-free test switches.
	if (AutoLogStatsSeconds > 0.f && FPlatformTime::Seconds() >= NextAutoLogTime)
	{
		NextAutoLogTime += AutoLogStatsSeconds;
		UE_LOG(LogPoseEstimation, Display, TEXT("[auto %.0f s] %s"), FPlatformTime::Seconds() - AutoStartTime, *GetStatsText());
	}
	if (AutoRecordAfterSeconds > 0.f && AutoRecordForSeconds > 0.f && AutoRecordStep < 2)
	{
		const double Elapsed = FPlatformTime::Seconds() - AutoStartTime;
		if ((AutoRecordStep == 0 && Elapsed >= AutoRecordAfterSeconds) || (AutoRecordStep == 1 && Elapsed >= AutoRecordAfterSeconds + AutoRecordForSeconds))
		{
			++AutoRecordStep;
			UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation test: pressing Record (%s)."), AutoRecordStep == 1 ? TEXT("start") : TEXT("stop"));
			ToggleRecording();
		}
	}
	if (AutoScreenshotEverySeconds > 0.f && FPlatformTime::Seconds() >= NextAutoScreenshotTime)
	{
		NextAutoScreenshotTime += AutoScreenshotEverySeconds;
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PoseEstimation") / TEXT("Screenshots")
			/ FString::Printf(TEXT("auto_%02d.png"), ++AutoScreenshotCount));
		FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation test: screenshot %s"), *Path);
	}
	if (AutoQuitAfterSeconds > 0.f && FPlatformTime::Seconds() - AutoStartTime >= AutoQuitAfterSeconds)
	{
		AutoQuitAfterSeconds = 0.f;
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation test: quitting after the requested time."));
		FPlatformMisc::RequestExit(/*bForce*/ false, TEXT("PoseQuitAfter"));
	}
	FollowTimelineRecording();
	if (!Grabber.IsValid())
	{
		return;
	}
	Grabber->SetTargetViewport(GetGameViewport());
	Grabber->TickGameThread();

	// The inference thread finished: publish its result.
	if (AnalysisOutput.IsValid() && InferenceThread.IsValid() && !InferenceThread->IsBusy())
	{
		TSharedPtr<FPoseAnalysisOutput, ESPMode::ThreadSafe> Output = MoveTemp(AnalysisOutput);
		if (IsRunning())
		{
			HandleAnalysisOutput(*Output);
		}
	}

	if (!IsRunning() || !Estimator.IsValid() || !InferenceThread.IsValid())
	{
		return;
	}

	// A copied frame is waiting and the models are free: analyse it.
	if (!AnalysisOutput.IsValid())
	{
		FPoseCapturedFrame Frame;
		if (Grabber->TakeFrame(Frame))
		{
			LaunchAnalysis(MoveTemp(Frame));
		}
	}

	// Ask for the frame that is about to be rendered. While the models are busy, ask just early enough that the copy
	// (a few frames) arrives as they finish: earlier, the frame would wait and its result would be older on screen.
	const double Now = FPlatformTime::Seconds();
	const bool bRateOk = MaxAnalysesPerSecond <= 0.f || Now - LastRequestRealTime >= 1.0 / MaxAnalysesPerSecond;
	bool bCanCopy = !AnalysisOutput.IsValid();
	if (!bCanCopy && bCopyNextFrameDuringAnalysis)
	{
		const double ElapsedMs = (Now - AnalysisLaunchTime) * 1000.0;
		const double ExpectedMs = AverageAnalysisMs(bInFlightIsDetectorFrame);
		const double LeadMs = AverageCopyMs() + SmoothedFrameSeconds * 1000.0;
		bCanCopy = ElapsedMs >= ExpectedMs - LeadMs;
	}
	if (bCanCopy && !Grabber->IsBusy() && bRateOk && Grabber->RequestFrame())
	{
		LastRequestRealTime = Now;
		LastRequestWorldTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	}

	CheckForStall(Now);
}

FString UPoseEstimationComp::DescribePipeline(double Now) const
{
	static const TCHAR* StepNames[] = { TEXT("queued"), TEXT("converting the image"), TEXT("running the models"), TEXT("Sports2D rules / angles"), TEXT("finished") };
	FString Text = FString::Printf(TEXT("frame copy: %s, last request %.2f s ago"),
		Grabber.IsValid() ? Grabber->GetStageName() : TEXT("-"), Grabber.IsValid() ? Grabber->GetSecondsSinceRequest() : 0.0);
	if (AnalysisOutput.IsValid())
	{
		const int32 Step = FMath::Clamp(AnalysisOutput->Step.load(), 0, 4);
		Text += FString::Printf(TEXT(" | analysis: %s, handed over %.2f s ago, frame %s, inference thread %s"), StepNames[Step],
			Now - AnalysisLaunchTime, bInFlightIsDetectorFrame ? TEXT("with detector") : TEXT("pose only"),
			InferenceThread.IsValid() && InferenceThread->IsBusy() ? TEXT("busy") : TEXT("idle"));
	}
	else
	{
		Text += TEXT(" | analysis: none in progress");
	}
	Text += FString::Printf(TEXT(" | last result %.2f s ago | game %.0f fps"), LastResultTime > 0.0 ? Now - LastResultTime : -1.0,
		SmoothedFrameSeconds > 0.f ? 1.f / SmoothedFrameSeconds : 0.f);
	return Text;
}

void UPoseEstimationComp::CheckForStall(double Now)
{
	const double Since = Now - FMath::Max(LastResultTime, LastEnableTime);
	if (!bStallReported && Since > StallSeconds)
	{
		bStallReported = true;
		StallStartTime = FMath::Max(LastResultTime, LastEnableTime);
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation: no result for %.1f s. %s"), Since, *DescribePipeline(Now));
	}
}

double UPoseEstimationComp::AverageAnalysisMs(bool bDetectorFrames) const
{
	double Sum = 0.0;
	int32 Count = 0;
	for (const FPoseStatSample& Sample : RecentResults)
	{
		if (Sample.bDetectorFrame == bDetectorFrames)
		{
			Sum += Sample.AnalysisMs;
			++Count;
		}
	}
	return Count ? Sum / Count : 0.0;
}

double UPoseEstimationComp::AverageCopyMs() const
{
	double Sum = 0.0;
	for (const FPoseStatSample& Sample : RecentResults)
	{
		Sum += Sample.CopyMs;
	}
	return RecentResults.Num() ? Sum / RecentResults.Num() : 50.0;
}

void UPoseEstimationComp::LaunchAnalysis(FPoseCapturedFrame&& Frame)
{
	FAnalysisSettings Settings;
	Settings.DetectionFrequency = DetectionFrequency;
	Settings.Sports2D.KeypointLikelihoodThreshold = KeypointLikelihoodThreshold;
	Settings.Sports2D.AverageLikelihoodThreshold = AverageLikelihoodThreshold;
	Settings.Sports2D.KeypointNumberThreshold = KeypointNumberThreshold;
	Settings.Sports2D.VisibleSide = VisibleSide;

	const bool bKeepImage = bDumpNextFrame;
	bDumpNextFrame = false;
	const bool bReset = bResetTracking;
	bResetTracking = false;

	const int32 FrameIndex = NextFrameIndex++;
	bInFlightIsDetectorFrame = FrameIndex % FMath::Max(1, DetectionFrequency) == 0;
	AnalysisLaunchTime = FPlatformTime::Seconds();

	TSharedPtr<FPoseAnalysisOutput, ESPMode::ThreadSafe> Output = MakeShared<FPoseAnalysisOutput, ESPMode::ThreadSafe>();
	Output->RequestTime = Frame.RequestTime;
	Output->ReadyTime = Frame.ReadyTime;
	Output->LaunchTime = AnalysisLaunchTime;
	const bool bSubmitted = InferenceThread->Submit(
		[EstimatorRef = Estimator, Output, CapturedFrame = MoveTemp(Frame), Settings, bKeepImage, bReset,
		 WorldTime = LastRequestWorldTime, FrameIndex]() mutable
	{
		Output->StartTime = FPlatformTime::Seconds();
		ON_SCOPE_EXIT { Output->EndTime = FPlatformTime::Seconds(); Output->Step.store(4); };
		Output->Step.store(1);
		FPoseImage Image;
		if (!CapturedFrame.ConvertToBGR(Image))
		{
			Output->Error = TEXT("Unsupported viewport pixel format.");
			return;
		}
		if (bReset)
		{
			EstimatorRef->ResetTracking();
		}
		Output->Step.store(2);
		if (!EstimatorRef->ProcessFrame(Image, Settings.DetectionFrequency, Output->Raw))
		{
			Output->Error = TEXT("The pose models failed on this frame (see LogPoseEstimation / LogNNERuntimeORT).");
			return;
		}
		Output->Step.store(3);
		const FVector2D ImageToScreenScale(1.0 / FMath::Max(1, CapturedFrame.TargetSize.X), 1.0 / FMath::Max(1, CapturedFrame.TargetSize.Y));
		PoseSports2D::BuildFrameResult(Output->Raw, FIntPoint(Image.Width, Image.Height), FVector2D(CapturedFrame.ViewRectMin) * ImageToScreenScale,
			ImageToScreenScale, Settings.Sports2D, Output->Result);
		Output->Result.FrameIndex = FrameIndex;
		Output->Result.WorldTime = WorldTime;
		Output->Result.LatencyMs = static_cast<float>((FPlatformTime::Seconds() - Output->RequestTime) * 1000.0);
		if (bKeepImage)
		{
			Output->Image = MoveTemp(Image);
		}
		Output->bOk = true;
	});
	if (bSubmitted)
	{
		AnalysisOutput = Output;
	}
}

void UPoseEstimationComp::HandleAnalysisOutput(FPoseAnalysisOutput& Output)
{
	if (!Output.bOk)
	{
		++ConsecutiveFailures;
		UE_LOG(LogPoseEstimation, Warning, TEXT("%s"), *Output.Error);
		if (ConsecutiveFailures >= MaxConsecutiveFailures)
		{
			LastError = FString::Printf(TEXT("Stopped after %d failed frames: %s"), ConsecutiveFailures, *Output.Error);
			UE_LOG(LogPoseEstimation, Error, TEXT("%s"), *LastError);
			SetEstimationEnabled(false);
		}
		return;
	}
	ConsecutiveFailures = 0;
	{
		const double ResultTime = FPlatformTime::Seconds();
		if (bStallReported)
		{
			bStallReported = false;
			UE_LOG(LogPoseEstimation, Display, TEXT("Pose estimation: results again after %.1f s. That frame: copy %.0f ms, waited %.0f ms, thread start %.0f ms, analysis %.0f ms (detector %.0f ms, pose %.0f ms)."),
				ResultTime - StallStartTime, (Output.ReadyTime - Output.RequestTime) * 1000.0, (Output.LaunchTime - Output.ReadyTime) * 1000.0,
				(Output.StartTime - Output.LaunchTime) * 1000.0, (Output.EndTime - Output.StartTime) * 1000.0, Output.Raw.DetectorMs, Output.Raw.PoseMs);
		}
		LastResultTime = ResultTime;
	}

	if (Output.Image.IsValid())
	{
		SaveDump(Output);
	}

	LatestResult = MoveTemp(Output.Result);
	LiveHistory.AddFrame(LatestResult);
	LiveHistory.TrimBefore(LatestResult.WorldTime - LiveHistorySeconds);
	if (bRecording)
	{
		Recording.AddFrame(LatestResult);
	}
	const double Now = FPlatformTime::Seconds();
	auto Ms = [](double From, double To) { return static_cast<float>(FMath::Max(0.0, To - From) * 1000.0); };
	FPoseStatSample& Sample = RecentResults.AddDefaulted_GetRef();
	Sample.Time = Now;
	Sample.bDetectorFrame = LatestResult.bDetectorRan;
	Sample.DetectorMs = LatestResult.DetectorMs;
	Sample.PoseMs = LatestResult.PoseMs;
	Sample.CopyMs = Ms(Output.RequestTime, Output.ReadyTime);
	Sample.WaitMs = Ms(Output.ReadyTime, Output.LaunchTime);
	Sample.StartMs = Ms(Output.LaunchTime, Output.StartTime);
	Sample.AnalysisMs = Ms(Output.StartTime, Output.EndTime);
	Sample.LatencyMs = Ms(Output.RequestTime, Output.EndTime);
	RecentResults.RemoveAll([Now](const FPoseStatSample& Old) { return Now - Old.Time > StatsWindowSeconds; });

	if (OverlayWidget)
	{
		OverlayWidget->SetPoseResult(LatestResult);
	}
	OnPoseEstimated.Broadcast(LatestResult);
}

void UPoseEstimationComp::SaveDump(const FPoseAnalysisOutput& Output) const
{
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PoseEstimation"));
	const FString Base = Folder / FString::Printf(TEXT("frame_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	const bool bImage = FPoseEstimator::SaveImageFile(Base + TEXT(".png"), Output.Image);
	const bool bJson = FFileHelper::SaveStringToFile(
		FPoseEstimator::ToJson(Output.Raw, Output.Image.Width, Output.Image.Height, FPaths::GetCleanFilename(Base + TEXT(".png"))), *(Base + TEXT(".json")));
	FFileHelper::SaveStringToFile(PoseSports2D::ResultToJson(Output.Result, FPaths::GetCleanFilename(Base + TEXT(".png"))), *(Base + TEXT(".sports2d.json")));
	// What the player sees, overlay and HUD included (taken a frame or two later, so the pose may have moved slightly).
	FScreenshotRequest::RequestScreenshot(Base + TEXT("_screen.png"), /*bShowUI*/ true, /*bAddFilenameSuffix*/ false, /*bHdr*/ false, FIntRect(), /*bRestrictToGameViewport*/ true);
	UE_LOG(LogPoseEstimation, Display, TEXT("Pose.Dump: %s.png %s, .json %s, .sports2d.json (angles), _screen.png (with the overlay)"),
		*Base, bImage ? TEXT("saved") : TEXT("FAILED"), bJson ? TEXT("saved") : TEXT("FAILED"));
}

// ---------------- Recording ----------------

PoseSignal::FSettings UPoseEstimationComp::GetPostProcessingSettings() const
{
	PoseSignal::FSettings Settings;
	Settings.bInterpolate = bInterpolate;
	Settings.InterpolateGapSmallerThan = InterpolateGapSmallerThan;
	Settings.MinChunkSize = MinChunkSize;
	Settings.bRejectOutliers = bRejectOutliers;
	Settings.bFilter = bFilter;
	Settings.ButterworthOrder = ButterworthOrder;
	Settings.ButterworthCutoffHz = ButterworthCutoffHz;
	return Settings;
}

float UPoseEstimationComp::GetRecordingSeconds() const
{
	return bRecording ? static_cast<float>(FPlatformTime::Seconds() - RecordingStartTime) : 0.f;
}

void UPoseEstimationComp::ToggleRecording()
{
	if (bRecordWithTimeline)
	{
		if (UCPP_UW_IHPCTimeline* TimelineWidget = FindTimeline())
		{
			TimelineWidget->ToggleRecording();   // FollowTimelineRecording picks the change up
			FollowTimelineRecording();
			return;
		}
	}
	if (bRecording)
	{
		StopRecording();
	}
	else
	{
		StartRecording();
	}
}

void UPoseEstimationComp::StartRecording()
{
	if (bRecording)
	{
		return;
	}
	bRecording = true;
	RecordingStartTime = FPlatformTime::Seconds();
	RecordingName = FString::Printf(TEXT("Recording_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	Recording.Reset();
	const bool bWasRunning = IsRunning();
	bRunForRecording = true;   // estimation stays on until the recording stops; the overlay is unchanged
	OnRunningChanged(bWasRunning);
	UE_LOG(LogPoseEstimation, Display, TEXT("Pose recording started (%s)."), *RecordingName);
}

void UPoseEstimationComp::StopRecording()
{
	if (!bRecording)
	{
		return;
	}
	bRecording = false;
	const double Seconds = FPlatformTime::Seconds() - RecordingStartTime;

	FPoseProcessedRecording Processed;
	if (Recording.Process(GetPostProcessingSettings(), Processed) && Processed.NumSamplesWithPerson > 0)
	{
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PoseEstimation") / TEXT("Recordings") / RecordingName);
		const TArray<FString> Files = FPoseRecording::Export(Processed, Folder, RecordingName);
		LastRecording = MoveTemp(Processed);
		bHasLastRecording = true;
		LastRecordingFolder = Folder;
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose recording saved: %.1f s, %d analysed frames (%d with the person) -> %d steps at %.2f fps, Butterworth %.0f Hz. %d file(s) in %s"),
			Seconds, LastRecording.NumSamples, LastRecording.NumSamplesWithPerson, LastRecording.Time.Num(), LastRecording.FrameRate, LastRecording.CutoffHz, Files.Num(), *Folder);
		OnRecordingSaved.Broadcast(Folder);
	}
	else
	{
		UE_LOG(LogPoseEstimation, Display, TEXT("Pose recording stopped after %.1f s: %d analysed frame(s), nobody detected - nothing saved."), Seconds, Recording.Num());
	}
	Recording.Reset();

	const bool bWasRunning = IsRunning();
	bRunForRecording = false;
	OnRunningChanged(bWasRunning);
}

UCPP_UW_IHPCTimeline* UPoseEstimationComp::FindTimeline()
{
	if (UCPP_UW_IHPCTimeline* Found = Timeline.Get())
	{
		return Found;
	}
	const double Now = FPlatformTime::Seconds();
	if (Now < NextTimelineSearchTime)
	{
		return nullptr;
	}
	NextTimelineSearchTime = Now + 1.0;   // the HUD may not exist yet: look again in a second
	for (TObjectIterator<UCPP_UW_IHPCTimeline> It; It; ++It)
	{
		if (It->GetWorld() == GetWorld() && !It->IsTemplate())
		{
			Timeline = *It;
			bTimelineWasRecording = It->IsRecording();
			return *It;
		}
	}
	return nullptr;
}

void UPoseEstimationComp::FollowTimelineRecording()
{
	if (!bRecordWithTimeline)
	{
		return;
	}
	UCPP_UW_IHPCTimeline* TimelineWidget = FindTimeline();
	if (!TimelineWidget)
	{
		return;
	}
	const bool bTimelineRecording = TimelineWidget->IsRecording();
	if (bTimelineRecording != bTimelineWasRecording)
	{
		bTimelineWasRecording = bTimelineRecording;
		if (bTimelineRecording)
		{
			StartRecording();
		}
		else
		{
			StopRecording();
		}
	}
}

// ---------------- Overlay ----------------

void UPoseEstimationComp::EnsureOverlay()
{
	if (OverlayWidget || !OverlayWidgetClass)
	{
		return;
	}
	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC && GetWorld())
	{
		PC = GetWorld()->GetFirstPlayerController();
	}
	if (PC && PC->IsLocalController())
	{
		OverlayWidget = CreateWidget<UCPP_UW_PoseOverlay>(PC, OverlayWidgetClass);
		if (OverlayWidget)
		{
			OverlayWidget->AddToViewport(OverlayZOrder);
		}
	}
}

void UPoseEstimationComp::RefreshOverlay()
{
	const bool bShow = bEnabled && bOverlayVisible;
	if (bShow)
	{
		EnsureOverlay();
	}
	if (OverlayWidget)
	{
		OverlayWidget->SetVisibility(bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
		if (!bShow)
		{
			OverlayWidget->ClearPose();
		}
	}
}

FViewport* UPoseEstimationComp::GetGameViewport() const
{
	const UWorld* World = GetWorld();
	UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
	return ViewportClient ? ViewportClient->Viewport : nullptr;
}
