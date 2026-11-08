#include "PoseEstimation/PoseEstimationCommandlet.h"

#include "PoseEstimation/PoseEstimationComp.h"
#include "PoseEstimation/PoseEstimator.h"
#include "PoseEstimation/PoseSports2D.h"
#include "PoseEstimation/PoseTypes.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

UPoseEstimationCommandlet::UPoseEstimationCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;   // with a plain game engine, editor-only plugins (MetaHuman Capture Manager) crash at startup
	LogToConsole = true;
}

int32 UPoseEstimationCommandlet::Main(const FString& Params)
{
	FString Images;
	FString Out;
	int32 DetFrequency = 4;
	FParse::Value(*Params, TEXT("Images="), Images);
	FParse::Value(*Params, TEXT("Out="), Out);
	FParse::Value(*Params, TEXT("DetFrequency="), DetFrequency);
	if (Images.IsEmpty() || Out.IsEmpty())
	{
		UE_LOG(LogPoseEstimation, Error, TEXT("Usage: -run=PoseEstimation -Images=<image or folder> -Out=<file.json> [-DetFrequency=4]"));
		return 1;
	}

	TArray<FString> Files;
	const bool bFolder = IFileManager::Get().DirectoryExists(*Images);
	if (bFolder)
	{
		IFileManager::Get().FindFiles(Files, *(Images / TEXT("*.png")), true, false);
		Files.Sort();
		for (FString& File : Files)
		{
			File = Images / File;
		}
	}
	else
	{
		Files.Add(Images);
	}
	if (Files.IsEmpty())
	{
		UE_LOG(LogPoseEstimation, Error, TEXT("No images found in %s"), *Images);
		return 1;
	}

	const UPoseEstimationComp* Defaults = GetDefault<UPoseEstimationComp>();
	FString Error;
	const double LoadStart = FPlatformTime::Seconds();
	TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> Estimator = FPoseEstimator::CreateBlocking(
		UPoseEstimationComp::ResolveModelPath(Defaults->DetectorModelFile), UPoseEstimationComp::ResolveModelPath(Defaults->PoseModelFile), Error);
	if (!Estimator.IsValid())
	{
		UE_LOG(LogPoseEstimation, Error, TEXT("Could not load the pose models: %s"), *Error);
		return 1;
	}
	UE_LOG(LogPoseEstimation, Display, TEXT("Models loaded in %.1f s."), FPlatformTime::Seconds() - LoadStart);

	PoseSports2D::FSettings Sports2DSettings;
	Sports2DSettings.KeypointLikelihoodThreshold = Defaults->KeypointLikelihoodThreshold;
	Sports2DSettings.AverageLikelihoodThreshold = Defaults->AverageLikelihoodThreshold;
	Sports2DSettings.KeypointNumberThreshold = Defaults->KeypointNumberThreshold;
	Sports2DSettings.VisibleSide = Defaults->VisibleSide;

	FString Json = TEXT("[\n");
	FString Sports2DJson = TEXT("[\n");
	double DetectorTotal = 0.0, PoseTotal = 0.0;
	int32 DetectorRuns = 0;
	for (int32 Index = 0; Index < Files.Num(); ++Index)
	{
		FPoseImage Image;
		if (!FPoseEstimator::LoadImageFile(Files[Index], Image))
		{
			UE_LOG(LogPoseEstimation, Error, TEXT("Could not read %s"), *Files[Index]);
			return 1;
		}
		FPoseRawFrame Frame;
		if (!Estimator->ProcessFrame(Image, bFolder ? DetFrequency : 1, Frame))
		{
			UE_LOG(LogPoseEstimation, Error, TEXT("Pose estimation failed on %s"), *Files[Index]);
			return 1;
		}
		DetectorTotal += Frame.DetectorMs;
		PoseTotal += Frame.PoseMs;
		DetectorRuns += Frame.bDetectorRan ? 1 : 0;
		Json += (Index ? TEXT(",\n") : TEXT("")) + FPoseEstimator::ToJson(Frame, Image.Width, Image.Height, FPaths::GetCleanFilename(Files[Index]));

		// The people Sports2D keeps and their angles.
		FPoseFrameResult Result;
		PoseSports2D::BuildFrameResult(Frame, FIntPoint(Image.Width, Image.Height), FVector2D::ZeroVector, FVector2D(1.0, 1.0), Sports2DSettings, Result);
		Sports2DJson += (Index ? TEXT(",\n") : TEXT("")) + PoseSports2D::ResultToJson(Result, FPaths::GetCleanFilename(Files[Index]));
		UE_LOG(LogPoseEstimation, Display, TEXT("%s: %d detection(s), %d person(s)%s"), *FPaths::GetCleanFilename(Files[Index]),
			Frame.Detections.Num(), Frame.Persons.Num(), Frame.bDetectorRan ? TEXT(" [detector]") : TEXT(""));
	}
	Json += TEXT("\n]\n");
	Sports2DJson += TEXT("\n]\n");
	FFileHelper::SaveStringToFile(Sports2DJson, *FPaths::ChangeExtension(Out, TEXT("sports2d.json")));

	if (!FFileHelper::SaveStringToFile(Json, *Out))
	{
		UE_LOG(LogPoseEstimation, Error, TEXT("Could not write %s"), *Out);
		return 1;
	}
	UE_LOG(LogPoseEstimation, Display, TEXT("%d image(s) -> %s. Detector %.1f ms average (%d runs), pose %.1f ms average."),
		Files.Num(), *Out, DetectorRuns ? DetectorTotal / DetectorRuns : 0.0, DetectorRuns, PoseTotal / Files.Num());
	return 0;
}
