// A pose recording: the main person's keypoints and angles for every analysed frame between Start and Stop, then
// Sports2D's post-processing (PoseSignal) and its output files.
//
// Live frames arrive ~11 times a second at slightly uneven times, while Sports2D works on video frames at a fixed rate.
// So the samples are first put on an even time grid at their average rate, one step per sample (each step takes the
// nearest sample, so every analysed frame is kept), then each angle / coordinate goes through Sports2D's chain exactly
// as it would for a video at that rate.
//
// Files (Sports2D's formats and names, person 00 = the main person):
//   <Name>_Sports2D_angles_person00.mot   filtered angles (OpenSim motion file, degrees)
//   <Name>_Sports2D_px_person00.trc        filtered keypoints in pixels (22 markers, Sports2D's skeleton order)
//   <Name>_angles.csv                      time, then each angle filtered and raw (for Excel)

#pragma once

#include "CoreMinimal.h"
#include "PoseEstimation/PoseSignal.h"
#include "PoseEstimation/PoseTypes.h"

/** One analysed frame of the main person. */
struct FPoseRecordingSample
{
	double Time = 0.0;              // seconds (world time of the analysed frame)
	bool bHasPerson = false;
	TArray<FVector2D> Keypoints;    // 26, image pixels
	TArray<bool> KeypointValid;     // 26
	TArray<double> Angles;          // Sports2D's 26 default angles, NaN when missing
};

/** The processed recording. Every column has one value per time step (NaN = missing). */
struct FPoseProcessedRecording
{
	double FrameRate = 0.0;             // of the even time grid
	double CutoffHz = 0.0;              // Butterworth cut-off actually used (Sports2D's live rule, then Pose2Sim's int())
	TArray<double> Time;                // seconds from the start of the recording
	TArray<FString> AngleNames;         // 26, Sports2D's names
	TArray<TArray<double>> AnglesRaw;   // [angle][step], on the even grid and unwrapped, before the rest of the post-processing
	TArray<TArray<double>> Angles;      // [angle][step], post-processed
	TArray<TArray<double>> KeypointX;   // [keypoint][step], post-processed, image pixels (26, EPoseKeypoint order)
	TArray<TArray<double>> KeypointY;
	int32 NumSamples = 0;               // analysed frames recorded
	int32 NumSamplesWithPerson = 0;
};

class METAHUMANTEST_API FPoseRecording
{
public:
	void Reset() { Samples.Reset(); }

	/** Adds the main person of a result (or an empty sample when nobody passes Sports2D's filter). */
	void AddFrame(const FPoseFrameResult& Result);

	/** Drops the samples older than Time (keeps a rolling window for live graphs). */
	void TrimBefore(double Time)
	{
		int32 Count = 0;
		while (Count < Samples.Num() && Samples[Count].Time < Time)
		{
			++Count;
		}
		Samples.RemoveAt(0, Count);
	}

	int32 Num() const { return Samples.Num(); }
	double Duration() const { return Samples.Num() > 1 ? Samples.Last().Time - Samples[0].Time : 0.0; }
	const TArray<FPoseRecordingSample>& GetSamples() const { return Samples; }

	/** Puts the samples on an even time grid and runs Sports2D's post-processing. False if fewer than 2 samples. */
	bool Process(const PoseSignal::FSettings& Settings, FPoseProcessedRecording& Out) const;

	/** Writes the .mot, .trc and .csv files into Folder. Returns the paths written. */
	static TArray<FString> Export(const FPoseProcessedRecording& Recording, const FString& Folder, const FString& Name);

	/** Sports2D's .trc marker order (its HALPE_26 skeleton tree, without eyes / ears). */
	static TConstArrayView<EPoseKeypoint> TrcMarkerOrder();

private:
	TArray<FPoseRecordingSample> Samples;
};
