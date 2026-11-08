// Sports2D-style pose estimation on the live game view. Lives on the player controller (ACPP_PlayerController
// creates it), so it keeps running when the possessed pawn changes.
//
// While enabled, it loops:  copy the frame on screen (FPoseFrameGrabber)  ->  run the detector + pose model on its
// own thread (FPoseInferenceThread, FPoseEstimator)  ->  apply Sports2D's rules and compute the angles (PoseSports2D)
// ->  update the overlay and fire OnPoseEstimated. One frame is analysed at a time; the next frame is copied while the
// models work, timed to arrive as they finish. The rate is whatever the CPU manages (see Pose.Stats).
//
// The models load in the background the first time it is enabled (a second or two).
//
// Console (also in the editor's Cmd box during Play):  Pose.Enable [0|1]   Pose.Overlay [0|1]   Pose.Stats
//                                                      Pose.Dump   Pose.TestImage <png> [out.json]

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PoseEstimation/PoseEstimator.h"
#include "PoseEstimation/PoseTypes.h"
#include "Tasks/Task.h"

#include <atomic>
#include "PoseEstimationComp.generated.h"

class FPoseFrameGrabber;
class FPoseInferenceThread;
class FViewport;
class UCPP_UW_PoseOverlay;
struct FPoseCapturedFrame;

UENUM(BlueprintType)
enum class EPoseEstimationStatus : uint8
{
	Off,
	LoadingModels,
	Running,
	Error,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPoseEstimated, const FPoseFrameResult&, Result);

/** One analysed frame's timings, in milliseconds (for the averages in Pose.Stats). */
struct FPoseStatSample
{
	double Time = 0.0;          // when the result was published (FPlatformTime::Seconds)
	bool bDetectorFrame = false;
	float DetectorMs = 0.f;     // inside the detector (detector frames only)
	float PoseMs = 0.f;         // inside the pose model
	float CopyMs = 0.f;         // frame asked for -> pixels on the CPU
	float WaitMs = 0.f;         // pixels on the CPU -> handed to the inference thread (waiting for the previous frame)
	float StartMs = 0.f;        // handed over -> the inference thread starts it
	float AnalysisMs = 0.f;     // the whole analysis (conversion, models, Sports2D rules)
	float LatencyMs = 0.f;      // frame asked for -> result ready
};

/** What a worker thread hands back for one frame. */
struct FPoseAnalysisOutput
{
	bool bOk = false;
	FString Error;
	FPoseFrameResult Result;
	FPoseRawFrame Raw;
	FPoseImage Image;   // only kept when the frame is being dumped
	double RequestTime = 0.0, ReadyTime = 0.0, LaunchTime = 0.0, StartTime = 0.0, EndTime = 0.0;   // FPlatformTime::Seconds
	std::atomic<int32> Step{ 0 };   // diagnostics: 0 queued, 1 converting, 2 models, 3 Sports2D rules, 4 done
};

UCLASS(ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class METAHUMANTEST_API UPoseEstimationComp : public UActorComponent
{
	GENERATED_BODY()

public:
	UPoseEstimationComp();

	// ---------------- Models ----------------

	/** Person detector (.onnx), relative to the project folder. Sports2D "balanced" mode: YOLOX-m. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Pose Estimation|Models")
	FString DetectorModelFile = TEXT("PoseModels/yolox_m_8xb8-300e_humanart-c2c7a14a.onnx");

	/** Pose model (.onnx), relative to the project folder. Sports2D "balanced" mode: RTMPose-m, HALPE_26. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Pose Estimation|Models")
	FString PoseModelFile = TEXT("PoseModels/rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192-4d3e73dd_20230605.onnx");

	// ---------------- Sports2D settings (Config_demo.toml defaults) ----------------

	/** Run the person detector every N analysed frames; in between, follow the box from the previous keypoints. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D", meta=(ClampMin="1"))
	int32 DetectionFrequency = 4;

	/** Keypoints with a lower score are ignored (Sports2D keypoint_likelihood_threshold). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D", meta=(ClampMin="0", ClampMax="1"))
	float KeypointLikelihoodThreshold = 0.3f;

	/** A person is ignored if the average score of their valid keypoints is lower (average_likelihood_threshold). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D", meta=(ClampMin="0", ClampMax="1"))
	float AverageLikelihoodThreshold = 0.5f;

	/** A person is ignored if fewer than this fraction of keypoints are valid (keypoint_number_threshold). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D", meta=(ClampMin="0", ClampMax="1"))
	float KeypointNumberThreshold = 0.3f;

	/** Which side of the person faces the camera (Sports2D visible_side). Auto: decided every frame from the direction
	 *  the feet point. The angles are flipped so they mean the same thing whichever way the person faces. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D")
	EPoseVisibleSide VisibleSide = EPoseVisibleSide::Auto;

	// ---------------- Rate / overlay ----------------

	/** Upper limit on analysed frames per second; 0 = as many as the CPU manages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation", meta=(ClampMin="0"))
	float MaxAnalysesPerSecond = 0.f;

	/** Copy the next frame while the models are busy with the current one: more frames analysed per second, at the
	 *  cost of each result being a little older (the dots trail movement slightly more). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation")
	bool bCopyNextFrameDuringAnalysis = true;

	/** Start estimating as soon as play begins (otherwise: Pose.Enable 1, or SetEstimationEnabled). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation")
	bool bEnableOnBeginPlay = false;

	/** Overlay widget; the C++ class works on its own, a Widget Blueprint child can restyle it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Overlay")
	TSubclassOf<UCPP_UW_PoseOverlay> OverlayWidgetClass;

	/** Draw order: above the 3D view and the old widgets (0), below the IHPC HUD (10) and the muscle popup (20). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Overlay")
	int32 OverlayZOrder = 5;

	// ---------------- Control ----------------

	/** Start / stop analysing the view. The first start loads the models in the background. */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation")
	void SetEstimationEnabled(bool bEnable);

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	bool IsEstimationEnabled() const { return bEnabled; }

	/** Show / hide the overlay (estimation keeps running for the data panel). */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation")
	void SetOverlayVisible(bool bVisible);

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	bool IsOverlayVisible() const { return bOverlayVisible; }

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	EPoseEstimationStatus GetStatus() const;

	/** Why loading failed (empty otherwise). */
	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	FString GetLastError() const { return LastError; }

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	const FPoseFrameResult& GetLatestResult() const { return LatestResult; }

	/** Analysed frames per second over the last two seconds. */
	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	float GetAnalysesPerSecond() const;

	/** One-line summary: status, rate, timings, people found. */
	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	FString GetStatsText() const;

	/** Save the next analysed frame (PNG) and its keypoints (JSON) in Saved/PoseEstimation. */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation")
	void DumpNextFrame();

	/** Fires on the game thread after every analysed frame. */
	UPROPERTY(BlueprintAssignable, Category="Pose Estimation")
	FOnPoseEstimated OnPoseEstimated;

	/** Absolute path of a model file setting. */
	static FString ResolveModelPath(const FString& File);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void StartLoadingModels();
	void OnModelFilesLoaded(TArray64<uint8>&& DetectorBytes, TArray64<uint8>&& PoseBytes, const FString& Error);
	void OnEstimatorInitialized(const TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe>& NewEstimator, const FString& Error);
	void FailLoading(const FString& Error);

	void LaunchAnalysis(FPoseCapturedFrame&& Frame);
	void HandleAnalysisOutput(FPoseAnalysisOutput& Output);
	void SaveDump(const FPoseAnalysisOutput& Output) const;

	void EnsureOverlay();
	void RefreshOverlay();
	FViewport* GetGameViewport() const;

	bool bEnabled = false;
	bool bOverlayVisible = true;
	bool bModelsLoading = false;
	bool bResetTracking = true;
	bool bDumpNextFrame = false;
	FString LastError;

	TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> Estimator;
	TSharedPtr<FPoseFrameGrabber, ESPMode::ThreadSafe> Grabber;

	UE::Tasks::FTask LoadTask;
	TSharedPtr<FPoseInferenceThread> InferenceThread;
	TSharedPtr<FPoseAnalysisOutput, ESPMode::ThreadSafe> AnalysisOutput;   // valid while a frame is being analysed
	double AnalysisLaunchTime = 0.0;
	bool bInFlightIsDetectorFrame = false;

	/** Watchdog: logs what every stage is doing when no result arrives for a while (see TickComponent). */
	void CheckForStall(double Now);
	FString DescribePipeline(double Now) const;
	double LastResultTime = 0.0;
	double LastEnableTime = 0.0;
	double StallStartTime = 0.0;
	bool bStallReported = false;

	/** Hands-free test switches (command line, e.g. "UnrealEditor.exe <project> -game -PoseAutoEnable -PoseLogStats=5
	 *  -PoseQuitAfter=60"): switch on at Begin Play, log Pose.Stats every N s, quit after N s. Off unless given. */
	float AutoLogStatsSeconds = 0.f;
	float AutoQuitAfterSeconds = 0.f;
	double AutoStartTime = 0.0;
	double NextAutoLogTime = 0.0;

	/** Average analysis time of recent detector / other frames (0 if none yet), and of the frame copy (ms). */
	double AverageAnalysisMs(bool bDetectorFrames) const;
	double AverageCopyMs() const;

	double LastRequestRealTime = 0.0;
	double LastRequestWorldTime = 0.0;
	int32 NextFrameIndex = 0;
	int32 ConsecutiveFailures = 0;
	/** Recent results, for the rate and average timings in GetStatsText. */
	TArray<FPoseStatSample> RecentResults;
	float SmoothedFrameSeconds = 0.f;

	UPROPERTY(Transient)
	TObjectPtr<UCPP_UW_PoseOverlay> OverlayWidget = nullptr;

	UPROPERTY(Transient)
	FPoseFrameResult LatestResult;
};
