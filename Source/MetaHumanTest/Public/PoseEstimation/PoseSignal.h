// Sports2D's post-processing of a time series (one angle, or one keypoint coordinate), ported from Sports2D process.py
// and Pose2Sim (common.py, triangulation.py, filtering.py) with scipy / numpy / pandas behaviour reproduced:
//
//   unwrap (angles only)  ->  fill gaps up to 100 frames by straight lines (interpolate_zeros_nans)
//   ->  keep from the first to the last run of >= 10 valid frames, fill larger gaps with the last value
//   ->  replace outliers by the median of a 7-frame window (Hampel, 2 sigma)
//   ->  zero-phase Butterworth low-pass (scipy butter(order / 2) + filtfilt), 6 Hz, 4th order
//
// Missing values are NaN. Everything is double precision, like numpy.

#pragma once

#include "CoreMinimal.h"

namespace PoseSignal
{
	/** Sports2D [post-processing] settings (Config_demo.toml defaults). */
	struct FSettings
	{
		bool bInterpolate = true;
		int32 InterpolateGapSmallerThan = 100;   // frames
		bool bFillLargeGapsWithLastValue = true; // fill_large_gaps_with = 'last_value'
		int32 MinChunkSize = 10;                 // frames
		bool bRejectOutliers = true;             // Hampel
		bool bFilter = true;                     // Butterworth
		int32 ButterworthOrder = 4;
		double ButterworthCutoffHz = 6.0;
	};

	/** numpy.unwrap(Values, period) on the valid values, then Sports2D's shift of the mean into [-180, 180]. */
	METAHUMANTEST_API void UnwrapAngles(TArray<double>& Values, double Period = 180.0);

	/** Pose2Sim interpolate_zeros_nans(col, MaxGap, 'linear'): zeros and NaNs are missing; gaps of at most MaxGap
	 *  frames are filled linearly; nothing before the first / after the last valid value. Unchanged if <= 4 valid. */
	METAHUMANTEST_API TArray<double> InterpolateZerosNans(const TArray<double>& Values, int32 MaxGap);

	/** Pose2Sim indices_of_first_last_non_nan_chunks(..., chunk_choice_method='all'): start of the first and end
	 *  (exclusive) of the last run of at least MinChunkSize valid values; (0, 0) if there is none. */
	METAHUMANTEST_API void FirstLastValidChunks(const TArray<double>& Values, int32 MinChunkSize, int32& OutStart, int32& OutEnd);

	/** Sports2D fill_large_gaps_with = 'last_value' for one column, including pandas' inclusive .loc slices. */
	METAHUMANTEST_API void FillLargeGapsWithLastValue(TArray<double>& Values, int32 Start, int32 End);

	/** Pose2Sim hampel_filter: a value further than NSigma (modified z-score) from its window's median is replaced
	 *  by the median. Windows containing NaN leave the value alone, as in numpy. */
	METAHUMANTEST_API TArray<double> HampelFilter(const TArray<double>& Values, int32 WindowSize = 7, double NSigma = 2.0);

	/** scipy.signal.butter(Order, Wn, 'low') digital coefficients (Wn = cutoff / Nyquist, 0 < Wn < 1). */
	METAHUMANTEST_API bool ButterLowpass(double Order, double Wn, TArray<double>& OutB, TArray<double>& OutA);

	/** scipy.signal.filtfilt(B, A, X) with its defaults (odd padding, padlen = 3 * max(len(A), len(B))). */
	METAHUMANTEST_API TArray<double> FiltFilt(const TArray<double>& B, const TArray<double>& A, const TArray<double>& X);

	/** Pose2Sim butterworth_filter_1d: butter(order / 2, int(cutoff) / (fps / 2)) + filtfilt on every run of valid
	 *  (non-NaN, non-zero) values longer than the padding. */
	METAHUMANTEST_API TArray<double> ButterworthFilter1D(const TArray<double>& Values, int32 Order, double CutoffHz, double FrameRate);

	/** Sports2D's cut-off for live input (its webcam rule): if CutoffHz is not below the Nyquist frequency, use
	 *  FrameRate / 2.001 instead. */
	METAHUMANTEST_API double LiveCutoffHz(double CutoffHz, double FrameRate);

	/** The whole chain for one column (after UnwrapAngles for angles). HampelWindow: 7 for angles,
	 *  round(7 * fps / 30) for keypoints, as in Sports2D. Returns all NaN if no run of MinChunkSize valid frames. */
	METAHUMANTEST_API TArray<double> ProcessColumn(const TArray<double>& Values, double FrameRate, int32 HampelWindow, const FSettings& Settings);
}
