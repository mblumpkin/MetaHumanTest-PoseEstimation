#include "PoseInferenceThread.h"

#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"

FPoseInferenceThread::FPoseInferenceThread()
{
	WakeEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
	IdleEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ true);
	IdleEvent->Trigger();
	Thread = FRunnableThread::Create(this, TEXT("PoseEstimationInference"), 0, TPri_Normal);
}

FPoseInferenceThread::~FPoseInferenceThread()
{
	if (Thread)
	{
		Thread->Kill(/*bShouldWait*/ true);   // calls Stop(), then waits for Run() to return
		delete Thread;
		Thread = nullptr;
	}
	FPlatformProcess::ReturnSynchEventToPool(WakeEvent);
	FPlatformProcess::ReturnSynchEventToPool(IdleEvent);
}

bool FPoseInferenceThread::Submit(TUniqueFunction<void()>&& Job)
{
	bool bExpected = false;
	if (!Thread || bStopping.load() || !bBusy.compare_exchange_strong(bExpected, true))
	{
		return false;
	}
	{
		FScopeLock Lock(&JobLock);
		PendingJob = MoveTemp(Job);
	}
	IdleEvent->Reset();
	WakeEvent->Trigger();
	return true;
}

void FPoseInferenceThread::WaitUntilIdle() const
{
	if (bBusy.load())
	{
		IdleEvent->Wait();
	}
}

uint32 FPoseInferenceThread::Run()
{
	while (!bStopping.load())
	{
		WakeEvent->Wait();
		TUniqueFunction<void()> Job;
		{
			FScopeLock Lock(&JobLock);
			Job = MoveTemp(PendingJob);
		}
		if (Job && !bStopping.load())
		{
			Job();
		}
		if (Job || bStopping.load())
		{
			bBusy.store(false);
			IdleEvent->Trigger();
		}
	}
	return 0;
}

void FPoseInferenceThread::Stop()
{
	bStopping.store(true);
	WakeEvent->Trigger();
}
