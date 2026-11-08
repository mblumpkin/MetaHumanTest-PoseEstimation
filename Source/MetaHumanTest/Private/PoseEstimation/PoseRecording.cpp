#include "PoseEstimation/PoseRecording.h"

#include "PoseEstimation/PoseSports2D.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cmath>
#include <limits>

namespace PoseRecordingPrivate
{
	double NaN() { return std::numeric_limits<double>::quiet_NaN(); }

	/** Values sampled at Times -> one value per step of the even grid Start + k / Rate (k = 0..Count-1): the value of
	 *  the nearest sample (NaN if that sample is missing). The grid has as many steps as there are samples, so every
	 *  analysed frame becomes one "video frame" for Sports2D's chain, moved by less than half a step in time. (Straight
	 *  lines between neighbours would lose every value next to a missing one, e.g. a knee that flickers in and out.) */
	TArray<double> Resample(const TArray<double>& Times, const TArray<double>& Values, double Start, double Rate, int32 Count)
	{
		TArray<double> Out;
		Out.Init(NaN(), Count);
		int32 I = 0;
		for (int32 K = 0; K < Count; ++K)
		{
			const double T = Start + K / Rate;
			while (I + 1 < Times.Num() && FMath::Abs(Times[I + 1] - T) <= FMath::Abs(Times[I] - T))
			{
				++I;
			}
			Out[K] = Values[I];
		}
		return Out;
	}

	FString Number(double Value)
	{
		return std::isnan(Value) ? FString() : FString::Printf(TEXT("%.6f"), Value);
	}
}

using namespace PoseRecordingPrivate;

TConstArrayView<EPoseKeypoint> FPoseRecording::TrcMarkerOrder()
{
	using E = EPoseKeypoint;
	static const EPoseKeypoint Order[] =
	{
		E::Hip, E::RHip, E::RKnee, E::RAnkle, E::RBigToe, E::RSmallToe, E::RHeel,
		E::LHip, E::LKnee, E::LAnkle, E::LBigToe, E::LSmallToe, E::LHeel,
		E::Neck, E::Head, E::Nose, E::RShoulder, E::RElbow, E::RWrist, E::LShoulder, E::LElbow, E::LWrist,
	};
	return Order;
}

void FPoseRecording::AddFrame(const FPoseFrameResult& Result)
{
	FPoseRecordingSample& Sample = Samples.AddDefaulted_GetRef();
	Sample.Time = Result.WorldTime;
	const int32 NumAngles = PoseSports2D::GetAngleDefinitions().Num();
	Sample.Angles.Init(NaN(), NumAngles);
	if (const FPosePerson* Person = Result.GetPrimary())
	{
		Sample.bHasPerson = true;
		for (const FPoseKeypoint& Keypoint : Person->Keypoints)
		{
			Sample.Keypoints.Add(Keypoint.ImagePosition);
			Sample.KeypointValid.Add(Keypoint.bValid);
		}
		for (int32 A = 0; A < Person->Angles.Num() && A < NumAngles; ++A)
		{
			if (Person->Angles[A].bValid)
			{
				Sample.Angles[A] = Person->Angles[A].Degrees;
			}
		}
	}
}

bool FPoseRecording::Process(const PoseSignal::FSettings& Settings, FPoseProcessedRecording& Out) const
{
	Out = FPoseProcessedRecording();
	Out.NumSamples = Samples.Num();
	if (Samples.Num() < 2)
	{
		return false;
	}
	const double Start = Samples[0].Time;
	const double Duration = Samples.Last().Time - Start;
	if (Duration <= 0.0)
	{
		return false;
	}

	// The even grid: the samples' average rate, from the first to the last sample.
	Out.FrameRate = (Samples.Num() - 1) / Duration;
	const int32 Count = FMath::FloorToInt32(Duration * Out.FrameRate + 1e-6) + 1;
	Out.Time.SetNum(Count);
	for (int32 K = 0; K < Count; ++K)
	{
		Out.Time[K] = K / Out.FrameRate;
	}
	Out.CutoffHz = std::trunc(PoseSignal::LiveCutoffHz(Settings.ButterworthCutoffHz, Out.FrameRate));   // Pose2Sim uses int(cutoff)

	TArray<double> Times;
	for (const FPoseRecordingSample& Sample : Samples)
	{
		Times.Add(Sample.Time);
		Out.NumSamplesWithPerson += Sample.bHasPerson ? 1 : 0;
	}

	// Angles: even grid first, so that what follows is exactly Sports2D on a video at that rate: unwrap (Sports2D does
	// it before anything else), then the chain.
	const TConstArrayView<PoseSports2D::FAngleDefinition> Definitions = PoseSports2D::GetAngleDefinitions();
	for (int32 A = 0; A < Definitions.Num(); ++A)
	{
		TArray<double> Values;
		for (const FPoseRecordingSample& Sample : Samples)
		{
			Values.Add(Sample.Angles.IsValidIndex(A) ? Sample.Angles[A] : NaN());
		}
		TArray<double> Grid = Resample(Times, Values, Start, Out.FrameRate, Count);
		PoseSignal::UnwrapAngles(Grid, 180.0);
		Out.AngleNames.Add(Definitions[A].Name);
		Out.AnglesRaw.Add(MoveTemp(Grid));
		Out.Angles.Add(PoseSignal::ProcessColumn(Out.AnglesRaw.Last(), Out.FrameRate, /*HampelWindow*/ 7, Settings));
	}

	// Keypoints: Sports2D uses a Hampel window of round(7 * fps / 30) for coordinates.
	const int32 KeypointHampelWindow = static_cast<int32>(std::nearbyint(7.0 * Out.FrameRate / 30.0));
	for (int32 K = 0; K < PoseKeypoints::Num; ++K)
	{
		TArray<double> X, Y;
		for (const FPoseRecordingSample& Sample : Samples)
		{
			const bool bValid = Sample.bHasPerson && Sample.KeypointValid.IsValidIndex(K) && Sample.KeypointValid[K];
			X.Add(bValid ? Sample.Keypoints[K].X : NaN());
			Y.Add(bValid ? Sample.Keypoints[K].Y : NaN());
		}
		Out.KeypointX.Add(PoseSignal::ProcessColumn(Resample(Times, X, Start, Out.FrameRate, Count), Out.FrameRate, KeypointHampelWindow, Settings));
		Out.KeypointY.Add(PoseSignal::ProcessColumn(Resample(Times, Y, Start, Out.FrameRate, Count), Out.FrameRate, KeypointHampelWindow, Settings));
	}
	return true;
}

TArray<FString> FPoseRecording::Export(const FPoseProcessedRecording& Recording, const FString& Folder, const FString& Name)
{
	TArray<FString> Written;
	IFileManager::Get().MakeDirectory(*Folder, /*Tree*/ true);
	const int32 Count = Recording.Time.Num();

	// --- .mot (Sports2D make_mot_with_angles): angles that are not entirely missing ---
	{
		TArray<int32> Columns;
		for (int32 A = 0; A < Recording.Angles.Num(); ++A)
		{
			for (double V : Recording.Angles[A])
			{
				if (!std::isnan(V))
				{
					Columns.Add(A);
					break;
				}
			}
		}
		TArray<FString> Lines =
		{
			TEXT("Coordinates"), TEXT("version=1"), FString::Printf(TEXT("nRows=%d"), Count), FString::Printf(TEXT("nColumns=%d"), Columns.Num()),
			TEXT("inDegrees=yes"), TEXT(""), TEXT("Units are S.I. units (second, meters, Newtons, ...)"),
			TEXT("If the header above contains a line with 'inDegrees', this indicates whether rotational values are in degrees (yes) or radians (no)."),
			TEXT(""), TEXT("endheader"),
		};
		FString Header = TEXT("time");
		for (int32 A : Columns)
		{
			Header += TEXT("\t") + Recording.AngleNames[A];
		}
		Lines.Add(Header);
		for (int32 Step = 0; Step < Count; ++Step)
		{
			FString Row = Number(Recording.Time[Step]);
			for (int32 A : Columns)
			{
				Row += TEXT("\t") + Number(Recording.Angles[A][Step]);
			}
			Lines.Add(Row);
		}
		const FString Path = Folder / (Name + TEXT("_Sports2D_angles_person00.mot"));
		if (FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *Path))
		{
			Written.Add(Path);
		}
	}

	// --- .trc in pixels (Sports2D make_trc_with_trc_data; Sports2D writes "m" as the unit even for pixels) ---
	{
		const TConstArrayView<EPoseKeypoint> Markers = TrcMarkerOrder();
		const FString Path = Folder / (Name + TEXT("_Sports2D_px_person00.trc"));
		const FString Rate = FString::Printf(TEXT("%g"), Recording.FrameRate);
		TArray<FString> Names;
		TArray<FString> Axes;
		for (int32 M = 0; M < Markers.Num(); ++M)
		{
			Names.Add(PoseKeypoints::GetName(static_cast<int32>(Markers[M])));
			Axes.Add(FString::Printf(TEXT("X%d\tY%d\tZ%d"), M + 1, M + 1, M + 1));
		}
		TArray<FString> Lines =
		{
			TEXT("PathFileType\t4\t(X/Y/Z)\t") + FPaths::ConvertRelativePathToFull(Path),
			TEXT("DataRate\tCameraRate\tNumFrames\tNumMarkers\tUnits\tOrigDataRate\tOrigDataStartFrame\tOrigNumFrames"),
			FString::Printf(TEXT("%s\t%s\t%d\t%d\tm\t%s\t0\t%d"), *Rate, *Rate, Count, Markers.Num(), *Rate, Count),
			TEXT("Frame#\tTime\t") + FString::Join(Names, TEXT("\t\t\t")) + TEXT("\t\t\t"),
			TEXT("\t\t") + FString::Join(Axes, TEXT("\t")),
		};
		for (int32 Step = 0; Step < Count; ++Step)
		{
			FString Row = FString::Printf(TEXT("%d\t%s"), Step, *Number(Recording.Time[Step]));
			for (EPoseKeypoint Marker : Markers)
			{
				const int32 K = static_cast<int32>(Marker);
				Row += TEXT("\t") + Number(Recording.KeypointX[K][Step]) + TEXT("\t") + Number(Recording.KeypointY[K][Step]) + TEXT("\t0.0");
			}
			Lines.Add(Row);
		}
		if (FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *Path))
		{
			Written.Add(Path);
		}
	}

	// --- .csv for spreadsheets: time, then every angle filtered and raw ---
	{
		FString Header = TEXT("time");
		for (const FString& AngleName : Recording.AngleNames)
		{
			Header += FString::Printf(TEXT(",%s,%s (raw)"), *AngleName, *AngleName);
		}
		TArray<FString> Lines = { Header };
		for (int32 Step = 0; Step < Count; ++Step)
		{
			FString Row = Number(Recording.Time[Step]);
			for (int32 A = 0; A < Recording.Angles.Num(); ++A)
			{
				Row += TEXT(",") + Number(Recording.Angles[A][Step]) + TEXT(",") + Number(Recording.AnglesRaw[A][Step]);
			}
			Lines.Add(Row);
		}
		const FString Path = Folder / (Name + TEXT("_angles.csv"));
		if (FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *Path))
		{
			Written.Add(Path);
		}
	}
	return Written;
}
