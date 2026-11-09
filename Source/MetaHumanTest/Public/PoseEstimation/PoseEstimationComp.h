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
// Recording (phase 3): follows the timeline's Record button. While recording, the main person's keypoints and angles
// are kept; on Stop they go through Sports2D's post-processing (PoseSignal) and are saved as Sports2D's .mot / .trc files
// and a .csv in Saved/PoseEstimation/Recordings/<name>/ (FPoseRecording). Estimation is switched on for the recording
// if it was off (without the overlay), so every recording has pose data.
//
// On screen (phase 4): two round buttons on the right (UCPP_UW_PoseButtons): Pose Overlay = SetEstimationEnabled,
// Pose Data = SetDataPanelOpen. Everything a coach needs is reachable from them and the timeline's Record button.
//
// Console (also in the editor's Cmd box during Play):  Pose.Enable [0|1]   Pose.Overlay [0|1]   Pose.Stats
//                                                      Pose.Data [0|1]   Pose.Dump   Pose.Record   Pose.TestImage <png> [out.json]

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "PoseEstimation/PoseEstimator.h"
#include "PoseEstimation/PoseRecording.h"
#include "PoseEstimation/PoseTypes.h"
#include "Tasks/Task.h"

#include <atomic>
#include "PoseEstimationComp.generated.h"

class FPoseFrameGrabber;
class FPoseInferenceThread;
class FViewport;
class UCPP_UW_PoseOverlay;
class UCPP_UW_PoseDataPanel;
class UCPP_UW_PoseButtons;
class UCPP_UW_IHPCTimeline;
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
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPoseRecordingSaved, const FString&, Folder);

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

	// ---------------- Sports2D post-processing of recordings (Config_demo.toml [post-processing]) ----------------

	/** Fill gaps of up to InterpolateGapSmallerThan frames by straight lines (interpolate). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing")
	bool bInterpolate = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing", meta=(ClampMin="1"))
	int32 InterpolateGapSmallerThan = 100;

	/** Data is kept from the first to the last run of at least this many valid frames (min_chunk_size); larger gaps
	 *  inside are filled with the last value (fill_large_gaps_with = 'last_value'). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing", meta=(ClampMin="1"))
	int32 MinChunkSize = 10;

	/** Hampel filter: replace outliers by the median of a 7-frame window (reject_outliers). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing")
	bool bRejectOutliers = true;

	/** Zero-phase Butterworth low-pass filter (filter, filter_type = 'butterworth'). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing")
	bool bFilter = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing", meta=(ClampMin="2"))
	int32 ButterworthOrder = 4;

	/** Hz. Lowered to just under half the frame rate when the frames come slower than twice this (Sports2D's live rule). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Sports2D post-processing", meta=(ClampMin="0.5"))
	float ButterworthCutoffHz = 6.f;

	// ---------------- Recording ----------------

	/** Start / stop the pose recording with the timeline's Record button. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Recording")
	bool bRecordWithTimeline = true;

	/** Seconds of live history kept for the data panel's graphs. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Recording", meta=(ClampMin="1"))
	float LiveHistorySeconds = 10.f;

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

	/** The Pose Data panel (graphs, numbers, recording); the C++ class works on its own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Data Panel")
	TSubclassOf<UCPP_UW_PoseDataPanel> DataPanelWidgetClass;

	/** Draw order: above the HUD (10), below the muscle popup (20). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Data Panel")
	int32 DataPanelZOrder = 11;

	/** Show the two round buttons on the right of the screen (Pose Overlay, Pose Data) when Play starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Buttons")
	bool bShowButtons = true;

	/** The buttons; the C++ class works on its own, a Widget Blueprint child can change their look or place. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Buttons")
	TSubclassOf<UCPP_UW_PoseButtons> ButtonsWidgetClass;

	/** Draw order: above the HUD (10), below the muscle popup (20). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Pose Estimation|Buttons")
	int32 ButtonsZOrder = 11;

	// ---------------- Control ----------------

	/** Start / stop analysing the view. The first start loads the models in the background. */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation")
	void SetEstimationEnabled(bool bEnable);

	UFUNCTION(BlueprintPure, Category="Pose Estimation")
	bool IsEstimationEnabled() const { return bEnabled; }

	/** Open / close the Pose Data panel. While it is open, estimation runs (the overlay only shows if enabled). */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation|Data Panel")
	void SetDataPanelOpen(bool bOpen);

	UFUNCTION(BlueprintPure, Category="Pose Estimation|Data Panel")
	bool IsDataPanelOpen() const { return bDataPanelOpen; }

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

	// ---------------- Recording ----------------

	/** Start / stop recording. With bRecordWithTimeline this toggles the timeline's Record (so both stay in step). */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation|Recording")
	void ToggleRecording();

	/** Start / stop the pose recording itself (the timeline isn't touched). Stop processes and saves it. */
	UFUNCTION(BlueprintCallable, Category="Pose Estimation|Recording")
	void StartRecording();

	UFUNCTION(BlueprintCallable, Category="Pose Estimation|Recording")
	void StopRecording();

	UFUNCTION(BlueprintPure, Category="Pose Estimation|Recording")
	bool IsRecording() const { return bRecording; }

	/** Seconds since the recording started (0 when not recording). */
	UFUNCTION(BlueprintPure, Category="Pose Estimation|Recording")
	float GetRecordingSeconds() const;

	/** Folder of the last saved recording (empty if none yet). */
	UFUNCTION(BlueprintPure, Category="Pose Estimation|Recording")
	FString GetLastRecordingFolder() const { return LastRecordingFolder; }

	/** Fires after a recording was processed and saved. */
	UPROPERTY(BlueprintAssignable, Category="Pose Estimation|Recording")
	FOnPoseRecordingSaved OnRecordingSaved;

	/** The last saved recording (post-processed), or null. */
	const FPoseProcessedRecording* GetLastRecording() const { return bHasLastRecording ? &LastRecording : nullptr; }

	/** The last LiveHistorySeconds of analysed frames (raw angles, for live graphs). */
	const FPoseRecording& GetLiveHistory() const { return LiveHistory; }

	/** Running = analysing frames: enabled (overlay), the data panel is open, or a recording is in progress. */
	bool IsRunning() const { return bEnabled || bDataPanelOpen || bRunForRecording; }

	PoseSignal::FSettings GetPostProcessingSettings() const;

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
	/** Creates the two round buttons once the local player's viewport exists. */
	void EnsureButtons();
	FViewport* GetGameViewport() const;

	/** Starts / stops the pipeline when IsRunning() changed. */
	void OnRunningChanged(bool bWasRunning);

	/** Starts / stops the pose recording when the timeline's Record state changes. */
	void FollowTimelineRecording();
	UCPP_UW_IHPCTimeline* FindTimeline();

	bool bRunForRecording = false;   // running because of a recording
	bool bDataPanelOpen = false;

	UPROPERTY(Transient)
	TObjectPtr<UCPP_UW_PoseDataPanel> DataPanelWidget = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UCPP_UW_PoseButtons> ButtonsWidget = nullptr;
	bool bRecording = false;
	double RecordingStartTime = 0.0;
	FString RecordingName;
	FPoseRecording Recording;
	FPoseRecording LiveHistory;
	FPoseProcessedRecording LastRecording;
	bool bHasLastRecording = false;
	FString LastRecordingFolder;
	TWeakObjectPtr<UCPP_UW_IHPCTimeline> Timeline;
	double NextTimelineSearchTime = 0.0;
	bool bTimelineWasRecording = false;

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
	float AutoRecordAfterSeconds = 0.f;   // -PoseRecordAfter=5 -PoseRecordFor=30: press Record at 5 s, again 30 s later
	float AutoRecordForSeconds = 0.f;
	int32 AutoRecordStep = 0;
	float AutoScreenshotEverySeconds = 0.f;   // -PoseScreenshotEvery=10: screenshot (with UI) into Saved/PoseEstimation/Screenshots
	double NextAutoScreenshotTime = 0.0;
	int32 AutoScreenshotCount = 0;
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
