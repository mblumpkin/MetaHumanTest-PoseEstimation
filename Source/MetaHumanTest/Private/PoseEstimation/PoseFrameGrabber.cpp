#include "PoseFrameGrabber.h"

#include "PoseEstimation/PoseEstimator.h"
#include "PoseEstimation/PoseTypes.h"
#include "HAL/PlatformTime.h"
#include "Math/Float16.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "UnrealClient.h"

namespace PoseFrameGrabberPrivate
{
	constexpr double TimeoutSeconds = 2.0;   // give up on a request the viewport never rendered (minimised, closed...)

	bool IsSupportedFormat(EPixelFormat Format)
	{
		return Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8 || Format == PF_A2B10G10R10 || Format == PF_FloatRGBA;
	}
}

using namespace PoseFrameGrabberPrivate;

FPoseFrameGrabber::FPoseFrameGrabber(const FAutoRegister& AutoRegister)
	: FSceneViewExtensionBase(AutoRegister)
{
}

FPoseFrameGrabber::~FPoseFrameGrabber() = default;

bool FPoseFrameGrabber::RequestFrame()
{
	check(IsInGameThread());
	EStage Expected = EStage::Idle;
	if (!TargetViewport || Stage.load() != EStage::Idle)
	{
		return false;
	}
	RequestTime.store(FPlatformTime::Seconds());
	return Stage.compare_exchange_strong(Expected, EStage::Requested);
}

const TCHAR* FPoseFrameGrabber::GetStageName() const
{
	switch (Stage.load())
	{
	case EStage::Requested: return TEXT("requested (waiting for the viewport to render)");
	case EStage::Copying:   return TEXT("copying (waiting for the GPU)");
	case EStage::Ready:     return TEXT("ready (waiting to be analysed)");
	default:                return TEXT("idle");
	}
}

double FPoseFrameGrabber::GetSecondsSinceRequest() const
{
	return FPlatformTime::Seconds() - RequestTime.load();
}

void FPoseFrameGrabber::Cancel()
{
	Stage.store(EStage::Idle);
}

void FPoseFrameGrabber::TickGameThread()
{
	check(IsInGameThread());
	const EStage Current = Stage.load();

	if ((Current == EStage::Requested || Current == EStage::Copying) && FPlatformTime::Seconds() - RequestTime.load() > TimeoutSeconds)
	{
		const int32 Count = ++NumTimeouts;
		UE_LOG(LogPoseEstimation, Display, TEXT("Frame copy timed out (%s, %d so far); asking again."),
			Current == EStage::Requested ? TEXT("the viewport didn't render") : TEXT("the GPU copy never finished"), Count);
		Cancel();
		return;
	}

	// Check the GPU copy on the render thread (mapping a staging texture has to happen there).
	if (Current == EStage::Copying && !bPollQueued.exchange(true))
	{
		TSharedRef<FPoseFrameGrabber, ESPMode::ThreadSafe> Self = StaticCastSharedRef<FPoseFrameGrabber>(AsShared());
		ENQUEUE_RENDER_COMMAND(PoseFrameGrabberPoll)([Self](FRHICommandListImmediate&)
		{
			Self->PollRenderThread();
			Self->bPollQueued.store(false);
		});
	}
}

bool FPoseFrameGrabber::TakeFrame(FPoseCapturedFrame& OutFrame)
{
	check(IsInGameThread());
	if (Stage.load() != EStage::Ready)
	{
		return false;
	}
	{
		FScopeLock Lock(&ReadyLock);
		OutFrame = MoveTemp(ReadyFrame);
		ReadyFrame = FPoseCapturedFrame();
	}
	Stage.store(EStage::Idle);
	return true;
}

void FPoseFrameGrabber::Shutdown()
{
	Cancel();
	TargetViewport = nullptr;
	TSharedRef<FPoseFrameGrabber, ESPMode::ThreadSafe> Self = StaticCastSharedRef<FPoseFrameGrabber>(AsShared());
	ENQUEUE_RENDER_COMMAND(PoseFrameGrabberShutdown)([Self](FRHICommandListImmediate&)
	{
		Self->Readback.Reset();
	});
}

bool FPoseFrameGrabber::IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const
{
	// Only the game viewport, and only when a frame was asked for: costs nothing the rest of the time.
	return TargetViewport != nullptr && Context.Viewport == TargetViewport && Stage.load() == EStage::Requested;
}

void FPoseFrameGrabber::PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily)
{
	if (Stage.load() != EStage::Requested || !InViewFamily.RenderTarget || InViewFamily.Views.IsEmpty())
	{
		return;
	}
	const FSceneView* View = InViewFamily.Views[0];
	if (!View || View->bIsSceneCapture || View->bIsReflectionCapture || View->bIsPlanarReflection)
	{
		return;
	}
	FRHITexture* TargetTexture = InViewFamily.RenderTarget->GetRenderTargetTexture();
	if (!TargetTexture)
	{
		return;
	}

	const FIntPoint TargetSize(static_cast<int32>(TargetTexture->GetSizeX()), static_cast<int32>(TargetTexture->GetSizeY()));
	FIntRect Rect = View->UnscaledViewRect;   // the 3D view inside the target (smaller when the camera forces an aspect ratio)
	Rect.Clip(FIntRect(FIntPoint::ZeroValue, TargetSize));
	if (Rect.Width() <= 0 || Rect.Height() <= 0)
	{
		return;
	}

	const EPixelFormat Format = TargetTexture->GetFormat();
	if (!IsSupportedFormat(Format))
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogPoseEstimation, Warning, TEXT("The viewport uses pixel format %s, which pose estimation can't read."), GPixelFormats[Format].Name);
		}
		return;
	}

	// The staging texture is created at the first copy's size: start a new one when the viewport is resized.
	if (!Readback || ReadbackSize != Rect.Size() || ReadbackFormat != Format)
	{
		Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("PoseEstimationFrame"));
		ReadbackSize = Rect.Size();
		ReadbackFormat = Format;
	}

	FRDGTextureRef Texture = RegisterExternalTexture(GraphBuilder, TargetTexture, TEXT("PoseEstimationViewFamilyTexture"));
	AddEnqueueCopyPass(GraphBuilder, Readback.Get(), Texture, FResolveRect(Rect.Min.X, Rect.Min.Y, Rect.Max.X, Rect.Max.Y));

	InFlight = FPoseCapturedFrame();
	InFlight.Width = Rect.Width();
	InFlight.Height = Rect.Height();
	InFlight.Format = Format;
	InFlight.ViewRectMin = Rect.Min;
	InFlight.TargetSize = TargetSize;
	InFlight.RequestTime = RequestTime.load();

	EStage Expected = EStage::Requested;
	Stage.compare_exchange_strong(Expected, EStage::Copying);
}

void FPoseFrameGrabber::PollRenderThread()
{
	check(IsInRenderingThread());
	if (Stage.load() != EStage::Copying || !Readback || !Readback->IsReady())
	{
		return;
	}

	int32 RowPitchInPixels = 0;
	int32 BufferHeight = 0;
	const uint8* Data = static_cast<const uint8*>(Readback->Lock(RowPitchInPixels, &BufferHeight));
	if (!Data)
	{
		Readback->Unlock();
		Stage.store(EStage::Idle);
		return;
	}

	FPoseCapturedFrame Frame = InFlight;
	Frame.ReadyTime = FPlatformTime::Seconds();
	const int32 BytesPerPixel = GPixelFormats[Frame.Format].BlockBytes;
	const int32 RowBytes = Frame.Width * BytesPerPixel;
	const int32 Rows = BufferHeight > 0 ? FMath::Min(Frame.Height, BufferHeight) : Frame.Height;
	Frame.Pixels.SetNumZeroed(RowBytes * Frame.Height);
	for (int32 Row = 0; Row < Rows; ++Row)
	{
		FMemory::Memcpy(&Frame.Pixels[Row * RowBytes], Data + static_cast<int64>(Row) * RowPitchInPixels * BytesPerPixel, RowBytes);
	}
	Readback->Unlock();

	{
		FScopeLock Lock(&ReadyLock);
		ReadyFrame = MoveTemp(Frame);
	}
	EStage Expected = EStage::Copying;
	Stage.compare_exchange_strong(Expected, EStage::Ready);
}

bool FPoseCapturedFrame::ConvertToBGR(FPoseImage& Out) const
{
	if (Width <= 0 || Height <= 0)
	{
		return false;
	}
	const int32 NumPixels = Width * Height;
	const int32 BytesPerPixel = GPixelFormats[Format].BlockBytes;
	if (Pixels.Num() < NumPixels * BytesPerPixel)
	{
		return false;
	}
	Out.Init(Width, Height);
	uint8* Dst = Out.BGR.GetData();
	const uint8* Src = Pixels.GetData();

	switch (Format)
	{
	case PF_B8G8R8A8:
		for (int32 I = 0; I < NumPixels; ++I)
		{
			Dst[I * 3 + 0] = Src[I * 4 + 0];
			Dst[I * 3 + 1] = Src[I * 4 + 1];
			Dst[I * 3 + 2] = Src[I * 4 + 2];
		}
		return true;

	case PF_R8G8B8A8:
		for (int32 I = 0; I < NumPixels; ++I)
		{
			Dst[I * 3 + 0] = Src[I * 4 + 2];
			Dst[I * 3 + 1] = Src[I * 4 + 1];
			Dst[I * 3 + 2] = Src[I * 4 + 0];
		}
		return true;

	case PF_A2B10G10R10:   // R in the low 10 bits (DXGI R10G10B10A2)
		for (int32 I = 0; I < NumPixels; ++I)
		{
			uint32 Packed;
			FMemory::Memcpy(&Packed, Src + I * 4, 4);
			auto To8 = [](uint32 V10) { return static_cast<uint8>((V10 * 255 + 511) / 1023); };
			Dst[I * 3 + 2] = To8(Packed & 0x3FF);
			Dst[I * 3 + 1] = To8((Packed >> 10) & 0x3FF);
			Dst[I * 3 + 0] = To8((Packed >> 20) & 0x3FF);
		}
		return true;

	case PF_FloatRGBA:     // half floats; the tonemapper already wrote display values in 0..1 (SDR output)
		for (int32 I = 0; I < NumPixels; ++I)
		{
			FFloat16 Channels[4];
			FMemory::Memcpy(Channels, Src + I * 8, 8);
			auto To8 = [](const FFloat16& V) { return static_cast<uint8>(FMath::Clamp(FMath::RoundToInt32(V.GetFloat() * 255.f), 0, 255)); };
			Dst[I * 3 + 2] = To8(Channels[0]);
			Dst[I * 3 + 1] = To8(Channels[1]);
			Dst[I * 3 + 0] = To8(Channels[2]);
		}
		return true;

	default:
		return false;
	}
}
