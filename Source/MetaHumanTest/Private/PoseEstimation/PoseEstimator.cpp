#include "PoseEstimation/PoseEstimator.h"

#include "PoseEstimation/PoseTypes.h"
#include "HAL/PlatformTime.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "NNE.h"
#include "NNEModelData.h"
#include "NNERuntimeCPU.h"
#include "NNERuntimeRunSync.h"
#include "NNEStatus.h"
#include "NNETypes.h"
#include "UObject/Package.h"

#include <cmath>

namespace PoseEstimatorPrivate
{
	const TCHAR* const RuntimeName = TEXT("NNERuntimeORTCpu");

	constexpr int32 NumKeypoints = PoseKeypoints::Num;
	constexpr int32 MaxDetections = 1000;            // the detector keeps far fewer after its built-in NMS
	constexpr float DetectorScoreThreshold = 0.3f;   // yolox.py postprocess (end2end model branch)
	constexpr double BoxPadding = 1.25;              // rtmpose.py preprocess: bbox_xyxy2cs(bbox, padding=1.25)
	constexpr double SimccSplitRatio = 2.0;          // rtmpose.py postprocess
	constexpr uint8 LetterboxGrey = 114;             // yolox.py preprocess
	const double PoseMean[3] = { 123.675, 116.28, 103.53 };   // rtmpose.py defaults, applied to B, G, R in that order
	const double PoseStd[3] = { 58.395, 57.12, 57.375 };

	/** cvRound / np.rint: round half to even. */
	inline int32 RoundHalfEven(float Value) { return static_cast<int32>(std::nearbyint(Value)); }
	inline uint8 SaturateU8(int32 Value) { return static_cast<uint8>(FMath::Clamp(Value, 0, 255)); }
	inline int32 SaturateS16(int32 Value) { return FMath::Clamp(Value, -32768, 32767); }

	double MsSince(double StartSeconds) { return (FPlatformTime::Seconds() - StartSeconds) * 1000.0; }

	/** rtmlib _get_3rd_point: b + (a - b) rotated 90 degrees anticlockwise (float32 like the numpy arrays). */
	FVector2f ThirdPoint(const FVector2f& A, const FVector2f& B)
	{
		const FVector2f Direction = A - B;
		return B + FVector2f(-Direction.Y, Direction.X);
	}

	/** cv2.getAffineTransform: the 2x3 matrix mapping 3 source points onto 3 destination points. */
	bool GetAffineTransform(const FVector2f Src[3], const FVector2f Dst[3], double OutM[6])
	{
		const double X0 = Src[0].X, Y0 = Src[0].Y, X1 = Src[1].X, Y1 = Src[1].Y, X2 = Src[2].X, Y2 = Src[2].Y;
		const double Det = X0 * (Y1 - Y2) - Y0 * (X1 - X2) + (X1 * Y2 - X2 * Y1);
		if (FMath::Abs(Det) < 1e-12)
		{
			return false;
		}
		// Cramer's rule for a*x + b*y + c = u, once for the x row and once for the y row.
		auto Solve = [&](double U0, double U1, double U2, double* Row)
		{
			Row[0] = (U0 * (Y1 - Y2) - Y0 * (U1 - U2) + (U1 * Y2 - U2 * Y1)) / Det;
			Row[1] = (X0 * (U1 - U2) - U0 * (X1 - X2) + (X1 * U2 - X2 * U1)) / Det;
			Row[2] = (X0 * (Y1 * U2 - Y2 * U1) - Y0 * (X1 * U2 - X2 * U1) + U0 * (X1 * Y2 - X2 * Y1)) / Det;
		};
		Solve(Dst[0].X, Dst[1].X, Dst[2].X, OutM);
		Solve(Dst[0].Y, Dst[1].Y, Dst[2].Y, OutM + 3);
		return true;
	}

	UE::NNE::FTensorShape MakeShape(uint32 D0, uint32 D1, uint32 D2, uint32 D3)
	{
		const uint32 Dims[4] = { D0, D1, D2, D3 };
		return UE::NNE::FTensorShape::Make(TConstArrayView<uint32>(Dims, 4));
	}

	/** Dimension Index of a model input/output, or Fallback when it is symbolic (e.g. "batch"). */
	int32 ConcreteDim(const UE::NNE::FTensorDesc& Desc, int32 Index, int32 Fallback)
	{
		const TConstArrayView<int32> Dims = Desc.GetShape().GetData();
		return (Dims.IsValidIndex(Index) && Dims[Index] > 0) ? Dims[Index] : Fallback;
	}
}

using namespace PoseEstimatorPrivate;

// ------------------------------------------------------------------------------------------------
// Model creation
// ------------------------------------------------------------------------------------------------

FPoseEstimator::FPoseEstimator() = default;
FPoseEstimator::~FPoseEstimator() = default;

TSharedPtr<UE::NNE::IModelCPU> FPoseEstimator::CreateModelFromOnnx(TConstArrayView64<uint8> OnnxBytes, FString& OutError)
{
	check(IsInGameThread());

	TWeakInterfacePtr<INNERuntimeCPU> Runtime = UE::NNE::GetRuntime<INNERuntimeCPU>(RuntimeName);
	if (!Runtime.IsValid())
	{
		OutError = FString::Printf(TEXT("The NNE runtime '%s' was not found. Is the NNERuntimeORT plugin enabled?"), RuntimeName);
		return nullptr;
	}
	if (OnnxBytes.IsEmpty())
	{
		OutError = TEXT("The ONNX model file is empty.");
		return nullptr;
	}

	// The runtime only takes model data through a UNNEModelData object. We create one just for this call: the model
	// keeps its own copy of the bytes, so the object (and its ~100 MB copy) is thrown away straight after.
	UNNEModelData* ModelData = NewObject<UNNEModelData>(GetTransientPackage(), NAME_None, RF_Transient);
	ModelData->Init(TEXT("onnx"), OnnxBytes);
	TSharedPtr<UE::NNE::IModelCPU> Model = Runtime->CreateModelCPU(ModelData);
	ModelData->ClearFileDataAndFileType();
	ModelData->ClearModelData();
	ModelData->MarkAsGarbage();

	if (!Model.IsValid())
	{
		OutError = TEXT("NNERuntimeORTCpu could not create a model from the ONNX file (see LogNNE / LogNNERuntimeORT).");
	}
	return Model;
}

bool FPoseEstimator::Initialize(const TSharedPtr<UE::NNE::IModelCPU>& DetectorModel, const TSharedPtr<UE::NNE::IModelCPU>& PoseModelCPU, FString& OutError)
{
	using namespace UE::NNE;

	if (!DetectorModel.IsValid() || !PoseModelCPU.IsValid())
	{
		OutError = TEXT("Missing model.");
		return false;
	}

	TSharedPtr<IModelInstanceCPU> NewDetector = DetectorModel->CreateModelInstanceCPU();
	TSharedPtr<IModelInstanceCPU> NewPose = PoseModelCPU->CreateModelInstanceCPU();
	if (!NewDetector.IsValid() || !NewPose.IsValid())
	{
		OutError = TEXT("Could not create the ONNX Runtime sessions (see LogNNERuntimeORT).");
		return false;
	}

	// Detector: input [1, 3, 640, 640], outputs dets [1, N, 5] and labels [1, N].
	const TConstArrayView<FTensorDesc> DetectorIn = NewDetector->GetInputTensorDescs();
	if (DetectorIn.Num() != 1 || DetectorIn[0].GetShape().Rank() != 4 || NewDetector->GetOutputTensorDescs().Num() != 2)
	{
		OutError = TEXT("The detector model does not look like YOLOX end2end (1 image input, dets + labels outputs).");
		return false;
	}
	DetectorHeight = ConcreteDim(DetectorIn[0], 2, 640);
	DetectorWidth = ConcreteDim(DetectorIn[0], 3, 640);

	// Pose model: input [batch, 3, 256, 192], outputs simcc_x [batch, 26, 384] and simcc_y [batch, 26, 512].
	const TConstArrayView<FTensorDesc> PoseIn = NewPose->GetInputTensorDescs();
	const TConstArrayView<FTensorDesc> PoseOut = NewPose->GetOutputTensorDescs();
	if (PoseIn.Num() != 1 || PoseIn[0].GetShape().Rank() != 4 || PoseOut.Num() != 2)
	{
		OutError = TEXT("The pose model does not look like RTMPose (1 image input, simcc_x + simcc_y outputs).");
		return false;
	}
	PoseHeight = ConcreteDim(PoseIn[0], 2, 256);
	PoseWidth = ConcreteDim(PoseIn[0], 3, 192);
	SimccXLength = ConcreteDim(PoseOut[0], 2, FMath::RoundToInt32(PoseWidth * SimccSplitRatio));
	SimccYLength = ConcreteDim(PoseOut[1], 2, FMath::RoundToInt32(PoseHeight * SimccSplitRatio));

	const FTensorShape DetectorShape = MakeShape(1, 3, DetectorHeight, DetectorWidth);
	const FTensorShape PoseShape = MakeShape(1, 3, PoseHeight, PoseWidth);
	if (NewDetector->SetInputTensorShapes(TConstArrayView<FTensorShape>(&DetectorShape, 1)) != EResultStatus::Ok ||
		NewPose->SetInputTensorShapes(TConstArrayView<FTensorShape>(&PoseShape, 1)) != EResultStatus::Ok)
	{
		OutError = TEXT("The models refused the input sizes.");
		return false;
	}

	DetectorInput.SetNumZeroed(3 * DetectorHeight * DetectorWidth);
	DetectorBoxes.SetNumZeroed(MaxDetections * 5);
	DetectorLabels.SetNumZeroed(MaxDetections);
	PoseInput.SetNumZeroed(3 * PoseHeight * PoseWidth);
	SimccX.SetNumZeroed(NumKeypoints * SimccXLength);
	SimccY.SetNumZeroed(NumKeypoints * SimccYLength);

	Detector = NewDetector;
	PoseModel = NewPose;
	ResetTracking();

	UE_LOG(LogPoseEstimation, Log, TEXT("Pose models ready: detector %dx%d, pose %dx%d (SimCC %d / %d)."),
		DetectorWidth, DetectorHeight, PoseWidth, PoseHeight, SimccXLength, SimccYLength);
	return true;
}

TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> FPoseEstimator::CreateBlocking(const FString& DetectorPath, const FString& PoseModelPath, FString& OutError)
{
	TArray64<uint8> DetectorBytes;
	TArray64<uint8> PoseBytes;
	if (!FFileHelper::LoadFileToArray(DetectorBytes, *DetectorPath))
	{
		OutError = FString::Printf(TEXT("Could not read %s"), *DetectorPath);
		return nullptr;
	}
	if (!FFileHelper::LoadFileToArray(PoseBytes, *PoseModelPath))
	{
		OutError = FString::Printf(TEXT("Could not read %s"), *PoseModelPath);
		return nullptr;
	}

	const TSharedPtr<UE::NNE::IModelCPU> DetectorModel = CreateModelFromOnnx(DetectorBytes, OutError);
	if (!DetectorModel.IsValid())
	{
		return nullptr;
	}
	const TSharedPtr<UE::NNE::IModelCPU> PoseModelCPU = CreateModelFromOnnx(PoseBytes, OutError);
	if (!PoseModelCPU.IsValid())
	{
		return nullptr;
	}

	TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> Estimator = MakeShared<FPoseEstimator, ESPMode::ThreadSafe>();
	if (!Estimator->Initialize(DetectorModel, PoseModelCPU, OutError))
	{
		return nullptr;
	}
	return Estimator;
}

// ------------------------------------------------------------------------------------------------
// Per frame (rtmlib PoseTracker.__call__ with tracking=False)
// ------------------------------------------------------------------------------------------------

void FPoseEstimator::ResetTracking()
{
	FrameCount = 0;
	BoxesLastFrame.Reset();
}

bool FPoseEstimator::ProcessFrame(const FPoseImage& Image, int32 DetFrequency, FPoseRawFrame& Out)
{
	Out = FPoseRawFrame();
	if (!IsInitialized() || !Image.IsValid())
	{
		return false;
	}
	DetFrequency = FMath::Max(1, DetFrequency);

	TArray<FPoseBox> Boxes;
	if (FrameCount % DetFrequency == 0)
	{
		const double Start = FPlatformTime::Seconds();
		if (!Detect(Image, Boxes))
		{
			return false;   // like RTMLib: no people, and the frame isn't counted
		}
		Out.DetectorMs = MsSince(Start);
		Out.bDetectorRan = true;
		Out.Detections = Boxes;
	}
	else
	{
		Boxes = BoxesLastFrame;
	}

	// rtmpose.py: with no box at all, the pose model looks at the whole image.
	if (Boxes.IsEmpty())
	{
		Boxes.Add(FPoseBox{ 0.0, 0.0, static_cast<double>(Image.Width), static_cast<double>(Image.Height) });
	}

	const double PoseStart = FPlatformTime::Seconds();
	for (const FPoseBox& Box : Boxes)
	{
		if (!EstimatePose(Image, Box, Out.Persons.AddDefaulted_GetRef()))
		{
			return false;
		}
	}
	Out.PoseMs = MsSince(PoseStart);

	// tracking=False: next frame's boxes are this frame's keypoints, grown by 1.25.
	BoxesLastFrame.Reset();
	for (const FPoseRawPerson& Person : Out.Persons)
	{
		BoxesLastFrame.Add(PoseToBox(Person.Keypoints));
	}
	++FrameCount;
	return true;
}

FPoseBox FPoseEstimator::PoseToBox(TConstArrayView<FVector2D> Keypoints, double Expansion)
{
	if (Keypoints.IsEmpty())
	{
		return FPoseBox();
	}
	double MinX = Keypoints[0].X, MaxX = Keypoints[0].X, MinY = Keypoints[0].Y, MaxY = Keypoints[0].Y;
	for (const FVector2D& P : Keypoints)
	{
		MinX = FMath::Min(MinX, P.X);
		MaxX = FMath::Max(MaxX, P.X);
		MinY = FMath::Min(MinY, P.Y);
		MaxY = FMath::Max(MaxY, P.Y);
	}
	const double Cx = (MinX + MaxX) / 2.0;
	const double Cy = (MinY + MaxY) / 2.0;
	return FPoseBox{ Cx - (Cx - MinX) * Expansion, Cy - (Cy - MinY) * Expansion, Cx + (MaxX - Cx) * Expansion, Cy + (MaxY - Cy) * Expansion };
}

// ------------------------------------------------------------------------------------------------
// Detector (rtmlib YOLOX)
// ------------------------------------------------------------------------------------------------

bool FPoseEstimator::Detect(const FPoseImage& Image, TArray<FPoseBox>& OutBoxes)
{
	OutBoxes.Reset();
	if (!Detector.IsValid() || !Image.IsValid())
	{
		return false;
	}

	// Letterbox: scale to fit 640x640 (aspect kept), top-left aligned, rest filled with grey 114.
	double Ratio = 1.0;
	if (Image.Width == DetectorWidth && Image.Height == DetectorHeight)
	{
		Letterbox = Image;
	}
	else
	{
		Letterbox.Init(DetectorWidth, DetectorHeight);
		FMemory::Memset(Letterbox.BGR.GetData(), LetterboxGrey, Letterbox.BGR.Num());

		Ratio = FMath::Min(static_cast<double>(DetectorHeight) / Image.Height, static_cast<double>(DetectorWidth) / Image.Width);
		const int32 ResizedWidth = FMath::Min(DetectorWidth, static_cast<int32>(Image.Width * Ratio));    // Python int(): truncates
		const int32 ResizedHeight = FMath::Min(DetectorHeight, static_cast<int32>(Image.Height * Ratio));
		if (ResizedWidth < 1 || ResizedHeight < 1)
		{
			return false;
		}
		ResizeLinear(Image, ResizedWidth, ResizedHeight, Resized);
		for (int32 Y = 0; Y < ResizedHeight; ++Y)
		{
			FMemory::Memcpy(&Letterbox.BGR[Y * DetectorWidth * 3], &Resized.BGR[Y * ResizedWidth * 3], ResizedWidth * 3);
		}
	}

	int32 NumDetections = 0;
	if (!RunDetector(Letterbox, NumDetections))
	{
		return false;
	}

	// The end2end model already did NMS: keep score > 0.3 and undo the letterbox scale (float32, like numpy).
	const float RatioF = static_cast<float>(Ratio);
	for (int32 Index = 0; Index < NumDetections; ++Index)
	{
		const float* Det = &DetectorBoxes[Index * 5];
		if (Det[4] > DetectorScoreThreshold)
		{
			OutBoxes.Add(FPoseBox{ Det[0] / RatioF, Det[1] / RatioF, Det[2] / RatioF, Det[3] / RatioF });
		}
	}
	return true;
}

bool FPoseEstimator::RunDetector(const FPoseImage& Input, int32& OutNumDetections)
{
	using namespace UE::NNE;
	OutNumDetections = 0;

	// HWC uint8 -> CHW float, no normalisation, channels stay B, G, R (rtmlib base.py inference).
	const int32 Plane = DetectorWidth * DetectorHeight;
	const uint8* Src = Input.BGR.GetData();
	float* Dst = DetectorInput.GetData();
	for (int32 Index = 0; Index < Plane; ++Index)
	{
		Dst[Index] = Src[Index * 3 + 0];
		Dst[Plane + Index] = Src[Index * 3 + 1];
		Dst[2 * Plane + Index] = Src[Index * 3 + 2];
	}

	// The outputs change size every run (number of people). NNE's ORT runtime keeps the previous run's output
	// shapes until SetInputTensorShapes is called again and would bind our buffers with that stale shape
	// (an ORT error; a crash in packaged builds, which have no try/catch). Resetting the shapes avoids that.
	const FTensorShape Shape = MakeShape(1, 3, DetectorHeight, DetectorWidth);
	if (Detector->SetInputTensorShapes(TConstArrayView<FTensorShape>(&Shape, 1)) != EResultStatus::Ok)
	{
		return false;
	}

	const FTensorBindingCPU InputBinding{ DetectorInput.GetData(), static_cast<uint64>(DetectorInput.Num()) * sizeof(float) };
	const FTensorBindingCPU OutputBindings[2] =
	{
		{ DetectorBoxes.GetData(), static_cast<uint64>(DetectorBoxes.Num()) * sizeof(float) },
		{ DetectorLabels.GetData(), static_cast<uint64>(DetectorLabels.Num()) * sizeof(int64) },
	};
	if (Detector->RunSync(TConstArrayView<FTensorBindingCPU>(&InputBinding, 1), TConstArrayView<FTensorBindingCPU>(OutputBindings, 2)) != EResultStatus::Ok)
	{
		UE_LOG(LogPoseEstimation, Warning, TEXT("Detector RunSync failed."));
		return false;
	}

	const TConstArrayView<FTensorShape> OutShapes = Detector->GetOutputTensorShapes();
	if (OutShapes.IsEmpty() || OutShapes[0].Rank() != 3 || OutShapes[0].GetData()[2] != 5)
	{
		UE_LOG(LogPoseEstimation, Warning, TEXT("Detector returned an unexpected output shape."));
		return false;
	}
	const uint32 Count = OutShapes[0].GetData()[1];
	if (Count > static_cast<uint32>(MaxDetections))
	{
		UE_LOG(LogPoseEstimation, Warning, TEXT("Detector returned %u boxes (more than %d)."), Count, MaxDetections);
		return false;
	}
	OutNumDetections = static_cast<int32>(Count);
	return true;
}

// ------------------------------------------------------------------------------------------------
// Pose model (rtmlib RTMPose)
// ------------------------------------------------------------------------------------------------

bool FPoseEstimator::EstimatePose(const FPoseImage& Image, const FPoseBox& Box, FPoseRawPerson& Out)
{
	using namespace UE::NNE;

	Out.Box = Box;
	Out.Keypoints.SetNumZeroed(NumKeypoints);
	Out.Scores.SetNumZeroed(NumKeypoints);
	if (!PoseModel.IsValid() || !Image.IsValid())
	{
		return false;
	}

	// bbox_xyxy2cs(padding=1.25), then top_down_affine: widen or heighten the box to the model's 3:4 aspect.
	const double CenterX = (Box.X1 + Box.X2) * 0.5;
	const double CenterY = (Box.Y1 + Box.Y2) * 0.5;
	double ScaleW = (Box.X2 - Box.X1) * BoxPadding;
	double ScaleH = (Box.Y2 - Box.Y1) * BoxPadding;
	const double Aspect = static_cast<double>(PoseWidth) / PoseHeight;
	if (ScaleW > ScaleH * Aspect)
	{
		ScaleH = ScaleW / Aspect;
	}
	else
	{
		ScaleW = ScaleH * Aspect;
	}

	// get_warp_matrix(center, scale, rot=0, output_size=(192, 256)): 3 point pairs stored as float32, then cv2.getAffineTransform.
	FVector2f Src[3];
	FVector2f Dst[3];
	Src[0] = FVector2f(static_cast<float>(CenterX), static_cast<float>(CenterY));
	Src[1] = FVector2f(static_cast<float>(CenterX), static_cast<float>(CenterY - ScaleW * 0.5));
	Src[2] = ThirdPoint(Src[0], Src[1]);
	Dst[0] = FVector2f(PoseWidth * 0.5f, PoseHeight * 0.5f);
	Dst[1] = FVector2f(PoseWidth * 0.5f, PoseHeight * 0.5f - PoseWidth * 0.5f);
	Dst[2] = ThirdPoint(Dst[0], Dst[1]);
	double WarpMatrix[6];
	if (!GetAffineTransform(Src, Dst, WarpMatrix))
	{
		return false;
	}
	WarpAffineLinear(Image, WarpMatrix, PoseWidth, PoseHeight, Crop);

	// (img - mean) / std per channel (B, G, R order, as rtmlib applies them), HWC -> CHW float.
	const int32 Plane = PoseWidth * PoseHeight;
	const uint8* CropPixels = Crop.BGR.GetData();
	for (int32 Index = 0; Index < Plane; ++Index)
	{
		for (int32 Channel = 0; Channel < 3; ++Channel)
		{
			PoseInput[Channel * Plane + Index] = static_cast<float>((CropPixels[Index * 3 + Channel] - PoseMean[Channel]) / PoseStd[Channel]);
		}
	}

	const FTensorShape Shape = MakeShape(1, 3, PoseHeight, PoseWidth);
	if (PoseModel->SetInputTensorShapes(TConstArrayView<FTensorShape>(&Shape, 1)) != EResultStatus::Ok)
	{
		return false;
	}
	const FTensorBindingCPU InputBinding{ PoseInput.GetData(), static_cast<uint64>(PoseInput.Num()) * sizeof(float) };
	const FTensorBindingCPU OutputBindings[2] =
	{
		{ SimccX.GetData(), static_cast<uint64>(SimccX.Num()) * sizeof(float) },
		{ SimccY.GetData(), static_cast<uint64>(SimccY.Num()) * sizeof(float) },
	};
	if (PoseModel->RunSync(TConstArrayView<FTensorBindingCPU>(&InputBinding, 1), TConstArrayView<FTensorBindingCPU>(OutputBindings, 2)) != EResultStatus::Ok)
	{
		UE_LOG(LogPoseEstimation, Warning, TEXT("Pose model RunSync failed."));
		return false;
	}
	const TConstArrayView<FTensorShape> OutShapes = PoseModel->GetOutputTensorShapes();
	if (OutShapes.Num() != 2 || OutShapes[0].Volume() != static_cast<uint64>(SimccX.Num()) || OutShapes[1].Volume() != static_cast<uint64>(SimccY.Num()))
	{
		UE_LOG(LogPoseEstimation, Warning, TEXT("Pose model returned unexpected output shapes."));
		return false;
	}

	// get_simcc_maximum: argmax per axis (first maximum), score = mean of the two maxima, location -1 when score <= 0.
	// Then keypoints = locs / 2 / input_size * scale + center - scale / 2.
	for (int32 Keypoint = 0; Keypoint < NumKeypoints; ++Keypoint)
	{
		const float* RowX = &SimccX[Keypoint * SimccXLength];
		const float* RowY = &SimccY[Keypoint * SimccYLength];
		int32 BestX = 0;
		int32 BestY = 0;
		for (int32 I = 1; I < SimccXLength; ++I)
		{
			if (RowX[I] > RowX[BestX])
			{
				BestX = I;
			}
		}
		for (int32 I = 1; I < SimccYLength; ++I)
		{
			if (RowY[I] > RowY[BestY])
			{
				BestY = I;
			}
		}
		const float Score = 0.5f * (RowX[BestX] + RowY[BestY]);
		float LocX = static_cast<float>(BestX);
		float LocY = static_cast<float>(BestY);
		if (Score <= 0.f)
		{
			LocX = -1.f;
			LocY = -1.f;
		}
		const double X = static_cast<double>(LocX / static_cast<float>(SimccSplitRatio)) / PoseWidth * ScaleW + CenterX - ScaleW / 2.0;
		const double Y = static_cast<double>(LocY / static_cast<float>(SimccSplitRatio)) / PoseHeight * ScaleH + CenterY - ScaleH / 2.0;
		Out.Keypoints[Keypoint] = FVector2D(X, Y);
		Out.Scores[Keypoint] = Score;
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
// Image operations (OpenCV equivalents)
// ------------------------------------------------------------------------------------------------

void FPoseEstimator::ResizeLinear(const FPoseImage& Src, int32 DstWidth, int32 DstHeight, FPoseImage& Dst)
{
	// OpenCV resize, INTER_LINEAR, 8-bit: 11-bit fixed-point coefficients; horizontal pass into ints, then the
	// vectorised vertical pass (VResizeLinearVec_32s8u). Checked bit-exact against cv2 5.0 on Sports2D's demo video.
	Dst.Init(DstWidth, DstHeight);
	const int32 SrcWidth = Src.Width;
	const int32 SrcHeight = Src.Height;
	constexpr int32 CoefScale = 2048;
	const double ScaleX = 1.0 / (static_cast<double>(DstWidth) / SrcWidth);
	const double ScaleY = 1.0 / (static_cast<double>(DstHeight) / SrcHeight);

	TArray<int32> XOffsets;
	TArray<int32> XAlpha;
	XOffsets.SetNumUninitialized(DstWidth);
	XAlpha.SetNumUninitialized(DstWidth * 2);
	for (int32 Dx = 0; Dx < DstWidth; ++Dx)
	{
		float Fx = static_cast<float>((Dx + 0.5) * ScaleX - 0.5);
		int32 Sx = FMath::FloorToInt32(Fx);
		Fx -= static_cast<float>(Sx);
		if (Sx < 0)
		{
			Fx = 0.f;
			Sx = 0;
		}
		if (Sx >= SrcWidth - 1)
		{
			Fx = 0.f;
			Sx = SrcWidth - 1;
		}
		XOffsets[Dx] = Sx;
		XAlpha[Dx * 2 + 0] = RoundHalfEven((1.f - Fx) * CoefScale);
		XAlpha[Dx * 2 + 1] = RoundHalfEven(Fx * CoefScale);
	}

	// Horizontal pass on every source row that the vertical pass will read.
	TArray<int32> RowOf;   // source row -> index in Horizontal, or -1
	RowOf.Init(-1, SrcHeight);
	TArray<int32> YOffsets;
	TArray<int32> YBeta;
	YOffsets.SetNumUninitialized(DstHeight);
	YBeta.SetNumUninitialized(DstHeight * 2);
	int32 NumRows = 0;
	for (int32 Dy = 0; Dy < DstHeight; ++Dy)
	{
		float Fy = static_cast<float>((Dy + 0.5) * ScaleY - 0.5);
		const int32 Sy = FMath::FloorToInt32(Fy);
		Fy -= static_cast<float>(Sy);
		YOffsets[Dy] = Sy;
		YBeta[Dy * 2 + 0] = RoundHalfEven((1.f - Fy) * CoefScale);
		YBeta[Dy * 2 + 1] = RoundHalfEven(Fy * CoefScale);
		for (int32 K = 0; K < 2; ++K)
		{
			const int32 Row = FMath::Clamp(Sy + K, 0, SrcHeight - 1);
			if (RowOf[Row] < 0)
			{
				RowOf[Row] = NumRows++;
			}
		}
	}

	const int32 RowLength = DstWidth * 3;
	TArray<int32> Horizontal;
	Horizontal.SetNumUninitialized(NumRows * RowLength);
	for (int32 Row = 0; Row < SrcHeight; ++Row)
	{
		if (RowOf[Row] < 0)
		{
			continue;
		}
		const uint8* S = &Src.BGR[Row * SrcWidth * 3];
		int32* D = &Horizontal[RowOf[Row] * RowLength];
		for (int32 Dx = 0; Dx < DstWidth; ++Dx)
		{
			const int32 Sx0 = XOffsets[Dx] * 3;
			const int32 Sx1 = FMath::Min(XOffsets[Dx] + 1, SrcWidth - 1) * 3;
			const int32 A0 = XAlpha[Dx * 2 + 0];
			const int32 A1 = XAlpha[Dx * 2 + 1];
			for (int32 C = 0; C < 3; ++C)
			{
				D[Dx * 3 + C] = S[Sx0 + C] * A0 + S[Sx1 + C] * A1;
			}
		}
	}

	for (int32 Dy = 0; Dy < DstHeight; ++Dy)
	{
		const int32* S0 = &Horizontal[RowOf[FMath::Clamp(YOffsets[Dy], 0, SrcHeight - 1)] * RowLength];
		const int32* S1 = &Horizontal[RowOf[FMath::Clamp(YOffsets[Dy] + 1, 0, SrcHeight - 1)] * RowLength];
		const int32 B0 = YBeta[Dy * 2 + 0];
		const int32 B1 = YBeta[Dy * 2 + 1];
		uint8* D = &Dst.BGR[Dy * RowLength];
		for (int32 I = 0; I < RowLength; ++I)
		{
			// (S >> 4) * b >> 16, summed, then rounding shift by 2: 4 + 16 + 2 = the 22 bits of the coefficients.
			const int32 Sum = ((SaturateS16(S0[I] >> 4) * B0) >> 16) + ((SaturateS16(S1[I] >> 4) * B1) >> 16);
			D[I] = SaturateU8((Sum + 2) >> 2);
		}
	}
}

void FPoseEstimator::WarpAffineLinear(const FPoseImage& Src, const double M[6], int32 DstWidth, int32 DstHeight, FPoseImage& Dst)
{
	Dst.Init(DstWidth, DstHeight);

	// cv2.warpAffine inverts M (destination -> source) unless WARP_INVERSE_MAP is set.
	double Det = M[0] * M[4] - M[1] * M[3];
	Det = Det != 0.0 ? 1.0 / Det : 0.0;
	const double A11 = M[4] * Det;
	const double A22 = M[0] * Det;
	const double A12 = -M[1] * Det;
	const double A21 = -M[3] * Det;
	const double B1 = -A11 * M[2] - A12 * M[5];
	const double B2 = -A21 * M[2] - A22 * M[5];
	const float I0 = static_cast<float>(A11), I1 = static_cast<float>(A12), I2 = static_cast<float>(B1);
	const float I3 = static_cast<float>(A21), I4 = static_cast<float>(A22), I5 = static_cast<float>(B2);

	const int32 SrcWidth = Src.Width;
	const int32 SrcHeight = Src.Height;
	const uint8* Pixels = Src.BGR.GetData();
	static const uint8 Black[3] = { 0, 0, 0 };
	auto Fetch = [&](int32 X, int32 Y) -> const uint8*
	{
		return (X >= 0 && X < SrcWidth && Y >= 0 && Y < SrcHeight) ? &Pixels[(Y * SrcWidth + X) * 3] : Black;
	};

	for (int32 Y = 0; Y < DstHeight; ++Y)
	{
		uint8* Out = &Dst.BGR[Y * DstWidth * 3];
		const float Fy = static_cast<float>(Y);
		for (int32 X = 0; X < DstWidth; ++X)
		{
			const float Fx = static_cast<float>(X);
			const float Sx = I0 * Fx + I1 * Fy + I2;
			const float Sy = I3 * Fx + I4 * Fy + I5;
			const int32 X0 = FMath::FloorToInt32(Sx);
			const int32 Y0 = FMath::FloorToInt32(Sy);
			const float Ax = Sx - static_cast<float>(X0);
			const float Ay = Sy - static_cast<float>(Y0);
			const float W00 = (1.f - Ax) * (1.f - Ay);
			const float W01 = Ax * (1.f - Ay);
			const float W10 = (1.f - Ax) * Ay;
			const float W11 = Ax * Ay;
			const uint8* P00 = Fetch(X0, Y0);
			const uint8* P01 = Fetch(X0 + 1, Y0);
			const uint8* P10 = Fetch(X0, Y0 + 1);
			const uint8* P11 = Fetch(X0 + 1, Y0 + 1);
			for (int32 C = 0; C < 3; ++C)
			{
				const float Value = P00[C] * W00 + P01[C] * W01 + P10[C] * W10 + P11[C] * W11;
				Out[X * 3 + C] = SaturateU8(RoundHalfEven(Value));
			}
		}
	}
}

// ------------------------------------------------------------------------------------------------
// Files
// ------------------------------------------------------------------------------------------------

bool FPoseEstimator::LoadImageFile(const FString& Path, FPoseImage& OutImage)
{
	FImage Loaded;
	if (!FImageUtils::LoadImage(*Path, Loaded))
	{
		return false;
	}
	FImage Bgra;
	Loaded.CopyTo(Bgra, ERawImageFormat::BGRA8, EGammaSpace::sRGB);

	OutImage.Init(Bgra.SizeX, Bgra.SizeY);
	const uint8* Src = Bgra.RawData.GetData();
	const int32 NumPixels = Bgra.SizeX * Bgra.SizeY;
	for (int32 Index = 0; Index < NumPixels; ++Index)
	{
		OutImage.BGR[Index * 3 + 0] = Src[Index * 4 + 0];
		OutImage.BGR[Index * 3 + 1] = Src[Index * 4 + 1];
		OutImage.BGR[Index * 3 + 2] = Src[Index * 4 + 2];
	}
	return true;
}

bool FPoseEstimator::SaveImageFile(const FString& Path, const FPoseImage& Image)
{
	if (!Image.IsValid())
	{
		return false;
	}
	TArray<FColor> Colors;
	Colors.SetNumUninitialized(Image.Width * Image.Height);
	for (int32 Index = 0; Index < Colors.Num(); ++Index)
	{
		Colors[Index] = FColor(Image.BGR[Index * 3 + 2], Image.BGR[Index * 3 + 1], Image.BGR[Index * 3 + 0], 255);
	}
	return FImageUtils::SaveImageByExtension(*Path, FImageView(Colors.GetData(), Image.Width, Image.Height, EGammaSpace::sRGB));
}

FString FPoseEstimator::ToJson(const FPoseRawFrame& Frame, int32 ImageWidth, int32 ImageHeight, const FString& Name)
{
	auto BoxJson = [](const FPoseBox& Box)
	{
		return FString::Printf(TEXT("[%.6f, %.6f, %.6f, %.6f]"), Box.X1, Box.Y1, Box.X2, Box.Y2);
	};

	FString Json = FString::Printf(TEXT("{\"name\": \"%s\", \"width\": %d, \"height\": %d, \"detector_ran\": %s, \"detector_ms\": %.2f, \"pose_ms\": %.2f, \"detections\": ["),
		*Name.ReplaceCharWithEscapedChar(), ImageWidth, ImageHeight, Frame.bDetectorRan ? TEXT("true") : TEXT("false"), Frame.DetectorMs, Frame.PoseMs);
	for (int32 Index = 0; Index < Frame.Detections.Num(); ++Index)
	{
		Json += (Index ? TEXT(", ") : TEXT("")) + BoxJson(Frame.Detections[Index]);
	}
	Json += TEXT("], \"persons\": [");
	for (int32 PersonIndex = 0; PersonIndex < Frame.Persons.Num(); ++PersonIndex)
	{
		const FPoseRawPerson& Person = Frame.Persons[PersonIndex];
		Json += FString::Printf(TEXT("%s{\"box\": %s, \"keypoints\": ["), PersonIndex ? TEXT(", ") : TEXT(""), *BoxJson(Person.Box));
		for (int32 K = 0; K < Person.Keypoints.Num(); ++K)
		{
			Json += FString::Printf(TEXT("%s[%.6f, %.6f]"), K ? TEXT(", ") : TEXT(""), Person.Keypoints[K].X, Person.Keypoints[K].Y);
		}
		Json += TEXT("], \"scores\": [");
		for (int32 K = 0; K < Person.Scores.Num(); ++K)
		{
			Json += FString::Printf(TEXT("%s%.6f"), K ? TEXT(", ") : TEXT(""), Person.Scores[K]);
		}
		Json += TEXT("]}");
	}
	Json += TEXT("]}");
	return Json;
}
