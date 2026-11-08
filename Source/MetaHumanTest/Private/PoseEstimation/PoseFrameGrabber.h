// Copies the game viewport's finished 3D image (what the player sees, before the UMG HUD is drawn on top) back to
// the CPU, one frame at a time, without stalling the game thread:
//
//   game thread    RequestFrame()  -> the extension becomes active for the next frame of the game viewport
//   render thread  PostRenderViewFamily_RenderThread: queue a GPU copy of the view rectangle into a staging texture
//   render thread  (polled from TickGameThread) when the GPU copy is done: map it and copy the pixels out
//   game thread    TakeFrame() hands the pixels to whoever runs the pose models
//
// Nothing extra is rendered (unlike a scene capture), and the image is exactly the one on screen: same camera,
// exposure, anti-aliasing and MetaHuman hair. The overlay we draw is UMG, so the models never see it.

#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "SceneViewExtension.h"

#include <atomic>

class FRHIGPUTextureReadback;
class FViewport;
struct FPoseImage;

/** One copied frame, in the viewport render target's own pixel format. */
struct FPoseCapturedFrame
{
	TArray<uint8> Pixels;                          // Width * Height * bytes per pixel, rows packed
	int32 Width = 0;
	int32 Height = 0;
	EPixelFormat Format = PF_Unknown;
	FIntPoint ViewRectMin = FIntPoint::ZeroValue;  // where the image sits inside the viewport render target
	FIntPoint TargetSize = FIntPoint::ZeroValue;   // size of the viewport render target
	double RequestTime = 0.0;                      // FPlatformTime::Seconds() when the frame was asked for
	double ReadyTime = 0.0;                        // ... and when its pixels were back on the CPU

	/** Converts to 8-bit BGR (the models' input). Handles 8-bit BGRA / RGBA, 10-bit and half-float targets. */
	bool ConvertToBGR(FPoseImage& Out) const;
};

class FPoseFrameGrabber : public FSceneViewExtensionBase
{
public:
	explicit FPoseFrameGrabber(const FAutoRegister& AutoRegister);
	virtual ~FPoseFrameGrabber() override;

	// ---- Game thread ----

	/** The viewport to copy (the game / PIE viewport). Re-set every tick: it changes when PIE windows are recreated. */
	void SetTargetViewport(FViewport* InViewport) { TargetViewport = InViewport; }

	/** Ask for the next rendered frame. False if a copy is already in progress. */
	bool RequestFrame();

	/** Moves the copy along (checks whether the GPU finished). Call every tick. */
	void TickGameThread();

	/** The copied frame, once ready. */
	bool TakeFrame(FPoseCapturedFrame& OutFrame);

	/** A frame was requested and hasn't been taken yet. */
	bool IsBusy() const { return Stage.load() != EStage::Idle; }

	/** Forget the request in progress (if any). */
	void Cancel();

	/** How many requests were given up because the copy didn't arrive within 2 s. */
	int32 GetNumTimeouts() const { return NumTimeouts.load(); }

	/** For diagnostics: "idle", "requested", "copying" or "ready", and seconds since the last request. */
	const TCHAR* GetStageName() const;
	double GetSecondsSinceRequest() const;

	/** Frees the GPU staging texture (on the render thread). Call before dropping the grabber. */
	void Shutdown();

	// ---- ISceneViewExtension ----
	virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override;

protected:
	virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override;

private:
	enum class EStage : uint8
	{
		Idle,        // nothing requested
		Requested,   // waiting for the next render of the target viewport
		Copying,     // GPU copy queued, waiting for it to finish
		Ready,       // pixels are in ReadyFrame
	};

	void PollRenderThread();

	std::atomic<EStage> Stage{ EStage::Idle };
	std::atomic<bool> bPollQueued{ false };
	std::atomic<int32> NumTimeouts{ 0 };
	std::atomic<double> RequestTime{ 0.0 };   // written by the game thread before Stage becomes Requested

	// Game thread.
	FViewport* TargetViewport = nullptr;

	// Render thread.
	TUniquePtr<FRHIGPUTextureReadback> Readback;
	FIntPoint ReadbackSize = FIntPoint::ZeroValue;
	EPixelFormat ReadbackFormat = PF_Unknown;
	FPoseCapturedFrame InFlight;   // size / format / placement of the copy in progress (no pixels)

	// Render thread -> game thread.
	FCriticalSection ReadyLock;
	FPoseCapturedFrame ReadyFrame;
};
