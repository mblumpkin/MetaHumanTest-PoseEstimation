// A thread of its own for running the pose models, one job at a time.
//
// Unreal's shared task threads also run editor / engine work (shader jobs, DDC, asset builds...). A 30-200 ms model
// run queued behind that work can wait for seconds, so the models get a dedicated thread instead: a job starts the
// moment it is submitted.

#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"

#include <atomic>

class FEvent;
class FRunnableThread;

class FPoseInferenceThread final : public FRunnable
{
public:
	FPoseInferenceThread();
	virtual ~FPoseInferenceThread() override;   // stops the thread (waits for the running job)

	/** Runs Job on the thread. False if a job is still running (one at a time). Game thread. */
	bool Submit(TUniqueFunction<void()>&& Job);

	/** A job is queued or running. */
	bool IsBusy() const { return bBusy.load(); }

	/** Blocks until the current job (if any) has finished. */
	void WaitUntilIdle() const;

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	FRunnableThread* Thread = nullptr;
	FEvent* WakeEvent = nullptr;
	FEvent* IdleEvent = nullptr;
	FCriticalSection JobLock;
	TUniqueFunction<void()> PendingJob;
	std::atomic<bool> bBusy{ false };
	std::atomic<bool> bStopping{ false };
};
