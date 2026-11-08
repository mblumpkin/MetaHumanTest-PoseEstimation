// Runs Sports2D's two ONNX models inside Unreal (Neural Network Engine, ONNX Runtime CPU) and reproduces what
// RTMLib does around them, step for step, so the keypoints match what real Sports2D gets from the same image:
//
//   1. YOLOX-m person detector ("yolox_m_8xb8-300e_humanart"): letterbox the image to 640x640, boxes with score > 0.3.
//   2. RTMPose-m ("rtmpose-m_simcc-body7_pt-body7-halpe26_700e-256x192"): crop each box (x1.25, 192x256 affine warp,
//      ImageNet mean/std), decode the SimCC output into 26 keypoints + scores.
//   3. PoseTracker(det_frequency=N, tracking=False), which is how Sports2D sets RTMLib up: the detector only runs
//      every Nth frame; in between, each person's box is grown (x1.25) from their keypoints in the previous frame.
//
// Not a UObject: it lives on a worker thread. Only CreateModelFromOnnx (and CreateBlocking) must run on the game thread.
// One frame at a time: don't call ProcessFrame / Detect / EstimatePose from two threads at once.

#pragma once

#include "CoreMinimal.h"

namespace UE::NNE
{
	class IModelCPU;
	class IModelInstanceCPU;
}

/** 8-bit BGR image, rows top to bottom (OpenCV channel order: what RTMLib feeds its models). */
struct FPoseImage
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<uint8> BGR;   // Width * Height * 3

	void Init(int32 InWidth, int32 InHeight)
	{
		Width = InWidth;
		Height = InHeight;
		BGR.SetNumUninitialized(InWidth * InHeight * 3);
	}

	bool IsValid() const { return Width > 0 && Height > 0 && BGR.Num() == Width * Height * 3; }
};

/** Box in image pixels: left, top, right, bottom (RTMLib's xyxy). */
struct FPoseBox
{
	double X1 = 0.0;
	double Y1 = 0.0;
	double X2 = 0.0;
	double Y2 = 0.0;
};

/** RTMPose output for one box. */
struct FPoseRawPerson
{
	FPoseBox Box;                 // the box the model looked at
	TArray<FVector2D> Keypoints;  // 26, image pixels
	TArray<float> Scores;         // 26
};

/** Output of one ProcessFrame call. */
struct FPoseRawFrame
{
	bool bDetectorRan = false;
	TArray<FPoseBox> Detections;      // only filled when the detector ran
	TArray<FPoseRawPerson> Persons;   // one per box (RTMLib's raw output, before Sports2D's filtering)
	double DetectorMs = 0.0;
	double PoseMs = 0.0;
};

class METAHUMANTEST_API FPoseEstimator
{
public:
	FPoseEstimator();
	~FPoseEstimator();

	/** Game thread only: turns the bytes of an .onnx file into a CPU model of the "NNERuntimeORTCpu" runtime.
	 *  (Needs a short-lived UNNEModelData object, which is why it can't run on a worker thread.) */
	static TSharedPtr<UE::NNE::IModelCPU> CreateModelFromOnnx(TConstArrayView64<uint8> OnnxBytes, FString& OutError);

	/** Any thread: creates the inference sessions. Slow (about a second): don't call it on the game thread. */
	bool Initialize(const TSharedPtr<UE::NNE::IModelCPU>& DetectorModel, const TSharedPtr<UE::NNE::IModelCPU>& PoseModel, FString& OutError);

	/** Game thread, blocking (loads both files and initializes). For tests and the commandlet. */
	static TSharedPtr<FPoseEstimator, ESPMode::ThreadSafe> CreateBlocking(const FString& DetectorPath, const FString& PoseModelPath, FString& OutError);

	bool IsInitialized() const { return Detector.IsValid() && PoseModel.IsValid(); }

	/** One video frame, the way Sports2D drives RTMLib (PoseTracker with det_frequency = DetFrequency, tracking off). */
	bool ProcessFrame(const FPoseImage& Image, int32 DetFrequency, FPoseRawFrame& Out);

	/** Forget the boxes from the previous frame: the next ProcessFrame runs the detector. */
	void ResetTracking();

	/** YOLOX: person boxes (score > 0.3), image pixels. */
	bool Detect(const FPoseImage& Image, TArray<FPoseBox>& OutBoxes);

	/** RTMPose on one box: 26 keypoints + scores. */
	bool EstimatePose(const FPoseImage& Image, const FPoseBox& Box, FPoseRawPerson& Out);

	// ---- Building blocks, public so the parity test can check them on their own ----

	/** cv2.resize(..., interpolation=INTER_LINEAR) for 8-bit images, same fixed-point arithmetic (bit-exact). */
	static void ResizeLinear(const FPoseImage& Src, int32 DstWidth, int32 DstHeight, FPoseImage& Dst);

	/** cv2.warpAffine(..., flags=INTER_LINEAR), black border. M maps source -> destination (2x3, row-major).
	 *  Matches OpenCV 5's float bilinear to within 1 grey level on a few pixels. */
	static void WarpAffineLinear(const FPoseImage& Src, const double M[6], int32 DstWidth, int32 DstHeight, FPoseImage& Dst);

	/** rtmlib pose_to_bbox: the keypoints' bounding box, grown by Expansion around its centre. */
	static FPoseBox PoseToBox(TConstArrayView<FVector2D> Keypoints, double Expansion = 1.25);

	// ---- Test / export helpers ----

	/** Loads a PNG / JPG / ... into a BGR image. Use PNG when comparing with Sports2D: JPEG decoders differ slightly. */
	static bool LoadImageFile(const FString& Path, FPoseImage& OutImage);

	/** Saves a BGR image (format from the extension, e.g. .png). */
	static bool SaveImageFile(const FString& Path, const FPoseImage& Image);

	/** RTMLib-level output of one frame as JSON (boxes, 26 keypoints and scores per person), for comparing with Python. */
	static FString ToJson(const FPoseRawFrame& Frame, int32 ImageWidth, int32 ImageHeight, const FString& Name);

private:
	bool RunDetector(const FPoseImage& Letterboxed, int32& OutNumDetections);

	TSharedPtr<UE::NNE::IModelInstanceCPU> Detector;
	TSharedPtr<UE::NNE::IModelInstanceCPU> PoseModel;

	// Model input sizes (read from the models: 640x640 and 192x256).
	int32 DetectorWidth = 640;
	int32 DetectorHeight = 640;
	int32 PoseWidth = 192;
	int32 PoseHeight = 256;
	int32 SimccXLength = 384;
	int32 SimccYLength = 512;

	// Reused buffers.
	FPoseImage Letterbox;
	FPoseImage Resized;
	FPoseImage Crop;
	TArray<float> DetectorInput;
	TArray<float> DetectorBoxes;     // dets: N x 5 (x1, y1, x2, y2, score)
	TArray<int64> DetectorLabels;    // labels: N
	TArray<float> PoseInput;
	TArray<float> SimccX;
	TArray<float> SimccY;

	// PoseTracker state.
	int64 FrameCount = 0;
	TArray<FPoseBox> BoxesLastFrame;
};
