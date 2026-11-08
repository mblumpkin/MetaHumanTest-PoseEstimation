#include "PoseEstimation/PoseSignal.h"

#include "Algo/Reverse.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace PoseSignal
{
	static double NaN() { return std::numeric_limits<double>::quiet_NaN(); }

	/** numpy.mod / Python %: the result has the sign of the divisor. */
	static double PyMod(double Value, double Divisor)
	{
		return Value - Divisor * std::floor(Value / Divisor);
	}

	/** numpy.median of a window (NaN if the window contains NaN). */
	static double Median(TArray<double> Window)
	{
		for (double V : Window)
		{
			if (std::isnan(V))
			{
				return NaN();
			}
		}
		std::sort(Window.GetData(), Window.GetData() + Window.Num());
		const int32 N = Window.Num();
		return (N % 2) ? Window[N / 2] : 0.5 * (Window[N / 2 - 1] + Window[N / 2]);
	}

	void UnwrapAngles(TArray<double>& Values, double Period)
	{
		// numpy.unwrap on the valid values only (Sports2D: np.unwrap(angles[valid_mask], period=180)).
		TArray<int32> ValidIndices;
		for (int32 I = 0; I < Values.Num(); ++I)
		{
			if (!std::isnan(Values[I]))
			{
				ValidIndices.Add(I);
			}
		}
		if (ValidIndices.IsEmpty())
		{
			return;
		}
		const double Discont = Period / 2.0;
		const double High = Period / 2.0;
		const double Low = -High;
		double Correction = 0.0;
		TArray<double> Unwrapped;
		Unwrapped.Add(Values[ValidIndices[0]]);
		for (int32 K = 1; K < ValidIndices.Num(); ++K)
		{
			const double Delta = Values[ValidIndices[K]] - Values[ValidIndices[K - 1]];
			double DeltaMod = PyMod(Delta - Low, Period) + Low;
			if (DeltaMod == Low && Delta > 0.0)
			{
				DeltaMod = High;   // numpy: boundary_ambiguous
			}
			const double PhaseCorrect = std::abs(Delta) < Discont ? 0.0 : DeltaMod - Delta;
			Correction += PhaseCorrect;
			Unwrapped.Add(Values[ValidIndices[K]] + Correction);
		}
		// Sports2D: ang = ang - 360 if mean > 180; ang = ang + 360 if mean < -180 (each once).
		double Mean = 0.0;
		for (double V : Unwrapped)
		{
			Mean += V;
		}
		Mean /= Unwrapped.Num();
		double Shift = Mean > 180.0 ? -360.0 : 0.0;
		Mean += Shift;
		Shift += Mean < -180.0 ? 360.0 : 0.0;
		for (int32 K = 0; K < ValidIndices.Num(); ++K)
		{
			Values[ValidIndices[K]] = Unwrapped[K] + Shift;
		}
	}

	TArray<double> InterpolateZerosNans(const TArray<double>& Values, int32 MaxGap)
	{
		const int32 N = Values.Num();
		TArray<int32> Good;
		for (int32 I = 0; I < N; ++I)
		{
			if (!std::isnan(Values[I]) && Values[I] != 0.0)
			{
				Good.Add(I);
			}
		}
		if (Good.Num() <= 4)
		{
			return Values;
		}

		// scipy interp1d(idx_good, col[idx_good], kind='linear', bounds_error=False): NaN outside the valid range.
		TArray<double> Out = Values;
		int32 Segment = 0;
		for (int32 I = 0; I < N; ++I)
		{
			const bool bGood = !std::isnan(Values[I]) && Values[I] != 0.0;
			if (bGood)
			{
				continue;
			}
			if (I < Good[0] || I > Good.Last())
			{
				Out[I] = NaN();
				continue;
			}
			while (Segment + 1 < Good.Num() && Good[Segment + 1] < I)
			{
				++Segment;
			}
			const int32 X0 = Good[Segment];
			const int32 X1 = Good[Segment + 1];
			const double T = static_cast<double>(I - X0) / static_cast<double>(X1 - X0);
			Out[I] = Values[X0] + T * (Values[X1] - Values[X0]);
		}

		// Gaps of more than MaxGap missing values go back to NaN.
		int32 Run = 0;
		for (int32 I = 0; I <= N; ++I)
		{
			const bool bMissing = I < N && (std::isnan(Values[I]) || Values[I] == 0.0);
			if (bMissing)
			{
				++Run;
				continue;
			}
			if (Run > MaxGap)
			{
				for (int32 J = I - Run; J < I; ++J)
				{
					Out[J] = NaN();
				}
			}
			Run = 0;
		}
		return Out;
	}

	void FirstLastValidChunks(const TArray<double>& Values, int32 MinChunkSize, int32& OutStart, int32& OutEnd)
	{
		OutStart = 0;
		OutEnd = 0;
		bool bFound = false;
		int32 RunStart = INDEX_NONE;
		const int32 N = Values.Num();
		for (int32 I = 0; I <= N; ++I)
		{
			const bool bValid = I < N && !std::isnan(Values[I]);
			if (bValid && RunStart == INDEX_NONE)
			{
				RunStart = I;
			}
			else if (!bValid && RunStart != INDEX_NONE)
			{
				if (I - RunStart >= MinChunkSize)
				{
					if (!bFound)
					{
						OutStart = RunStart;
						bFound = true;
					}
					OutEnd = I;
				}
				RunStart = INDEX_NONE;
			}
		}
	}

	void FillLargeGapsWithLastValue(TArray<double>& Values, int32 Start, int32 End)
	{
		// Sports2D: loc[:start] = nan; loc[end:] = nan; loc[start:end] = loc[start:end].ffill().bfill()
		// pandas .loc slices include both ends, so the value at Start becomes NaN and is then back-filled.
		const int32 N = Values.Num();
		for (int32 I = 0; I <= FMath::Min(Start, N - 1); ++I)
		{
			Values[I] = NaN();
		}
		for (int32 I = FMath::Max(End, 0); I < N; ++I)
		{
			Values[I] = NaN();
		}
		const int32 Last = FMath::Min(End, N - 1);
		double Previous = NaN();
		for (int32 I = Start; I <= Last; ++I)
		{
			if (std::isnan(Values[I]))
			{
				Values[I] = Previous;
			}
			else
			{
				Previous = Values[I];
			}
		}
		double Next = NaN();
		for (int32 I = Last; I >= Start; --I)
		{
			if (std::isnan(Values[I]))
			{
				Values[I] = Next;
			}
			else
			{
				Next = Values[I];
			}
		}
	}

	TArray<double> HampelFilter(const TArray<double>& Values, int32 WindowSize, double NSigma)
	{
		TArray<double> Out = Values;
		const int32 Half = WindowSize / 2;
		for (int32 I = Half; I < Values.Num() - Half; ++I)
		{
			TArray<double> Window;
			for (int32 J = I - Half; J <= I + Half; ++J)
			{
				Window.Add(Values[J]);
			}
			const double Med = Median(Window);
			TArray<double> Deviations;
			for (double V : Window)
			{
				Deviations.Add(std::abs(V - Med));
			}
			const double Mad = Median(Deviations);
			if (Mad != 0.0)   // NaN != 0 is true in numpy too; the comparison below is then false
			{
				const double Z = 0.6745 * (Values[I] - Med) / Mad;
				if (std::abs(Z) > NSigma)
				{
					Out[I] = Med;
				}
			}
		}
		return Out;
	}

	bool ButterLowpass(double Order, double Wn, TArray<double>& OutB, TArray<double>& OutA)
	{
		using FComplex = std::complex<double>;
		if (!(Wn > 0.0 && Wn < 1.0) || Order < 1.0)
		{
			return false;
		}
		// buttap: poles -exp(j*pi*m/(2N)), m = -N+1, -N+3, ..., N-1
		TArray<FComplex> Poles;
		for (double M = -Order + 1.0; M < Order; M += 2.0)
		{
			Poles.Add(-std::exp(FComplex(0.0, PI * M / (2.0 * Order))));
		}
		// Pre-warp (fs = 2), lp2lp_zpk, bilinear_zpk.
		const double Fs = 2.0;
		const double Warped = 2.0 * Fs * std::tan(PI * Wn / Fs);
		double Gain = std::pow(Warped, static_cast<double>(Poles.Num()));
		const double Fs2 = 2.0 * Fs;
		FComplex Denominator(1.0, 0.0);
		TArray<FComplex> DigitalPoles;
		for (const FComplex& P : Poles)
		{
			const FComplex Scaled = P * Warped;
			Denominator *= (Fs2 - Scaled);
			DigitalPoles.Add((Fs2 + Scaled) / (Fs2 - Scaled));
		}
		Gain = Gain * (1.0 / Denominator).real();

		// zpk2tf: zeros all at -1 (one per pole), b = gain * poly(zeros), a = real(poly(poles)).
		auto Poly = [](const TArray<FComplex>& Roots)
		{
			TArray<FComplex> C;
			C.Add(FComplex(1.0, 0.0));
			for (const FComplex& R : Roots)
			{
				C.Add(FComplex(0.0, 0.0));
				for (int32 I = C.Num() - 1; I >= 1; --I)
				{
					C[I] -= R * C[I - 1];
				}
			}
			return C;
		};
		TArray<FComplex> Zeros;
		Zeros.Init(FComplex(-1.0, 0.0), DigitalPoles.Num());
		const TArray<FComplex> BC = Poly(Zeros);
		const TArray<FComplex> AC = Poly(DigitalPoles);
		OutB.SetNum(BC.Num());
		OutA.SetNum(AC.Num());
		for (int32 I = 0; I < BC.Num(); ++I)
		{
			OutB[I] = Gain * BC[I].real();
			OutA[I] = AC[I].real();
		}
		return true;
	}

	/** scipy lfilter (direct form II transposed) with initial state Zi. */
	static TArray<double> LFilter(const TArray<double>& B, const TArray<double>& A, const TArray<double>& X, TArray<double> Z)
	{
		const int32 N = A.Num();
		TArray<double> Y;
		Y.SetNumUninitialized(X.Num());
		for (int32 I = 0; I < X.Num(); ++I)
		{
			const double Xi = X[I];
			const double Yi = B[0] * Xi + (N > 1 ? Z[0] : 0.0);
			for (int32 J = 0; J < N - 2; ++J)
			{
				Z[J] = B[J + 1] * Xi - A[J + 1] * Yi + Z[J + 1];
			}
			if (N > 1)
			{
				Z[N - 2] = B[N - 1] * Xi - A[N - 1] * Yi;
			}
			Y[I] = Yi;
		}
		return Y;
	}

	/** scipy lfilter_zi: steady-state initial conditions for a step input. */
	static TArray<double> LFilterZi(const TArray<double>& B, const TArray<double>& A)
	{
		const int32 N = A.Num();
		const int32 M = N - 1;
		// (I - companion(a).T) zi = b[1:] - a[1:] * b[0], solved by Gaussian elimination.
		TArray<double> Mat;
		Mat.Init(0.0, M * M);
		TArray<double> Rhs;
		Rhs.SetNum(M);
		for (int32 R = 0; R < M; ++R)
		{
			for (int32 C = 0; C < M; ++C)
			{
				// companion(a): first row -a[1:]/a[0], ones on the sub-diagonal; transposed.
				const double CompanionT = (C == 0) ? -A[R + 1] / A[0] : ((R == C - 1) ? 1.0 : 0.0);
				Mat[R * M + C] = (R == C ? 1.0 : 0.0) - CompanionT;
			}
			Rhs[R] = B[R + 1] - A[R + 1] * B[0];
		}
		for (int32 Col = 0; Col < M; ++Col)
		{
			int32 Pivot = Col;
			for (int32 R = Col + 1; R < M; ++R)
			{
				if (std::abs(Mat[R * M + Col]) > std::abs(Mat[Pivot * M + Col]))
				{
					Pivot = R;
				}
			}
			for (int32 C = 0; C < M; ++C)
			{
				Swap(Mat[Col * M + C], Mat[Pivot * M + C]);
			}
			Swap(Rhs[Col], Rhs[Pivot]);
			for (int32 R = Col + 1; R < M; ++R)
			{
				const double F = Mat[R * M + Col] / Mat[Col * M + Col];
				for (int32 C = Col; C < M; ++C)
				{
					Mat[R * M + C] -= F * Mat[Col * M + C];
				}
				Rhs[R] -= F * Rhs[Col];
			}
		}
		TArray<double> Zi;
		Zi.SetNum(M);
		for (int32 R = M - 1; R >= 0; --R)
		{
			double Sum = Rhs[R];
			for (int32 C = R + 1; C < M; ++C)
			{
				Sum -= Mat[R * M + C] * Zi[C];
			}
			Zi[R] = Sum / Mat[R * M + R];
		}
		return Zi;
	}

	TArray<double> FiltFilt(const TArray<double>& BIn, const TArray<double>& AIn, const TArray<double>& X)
	{
		// Normalise by a[0] and pad b / a to the same length, as scipy does.
		const int32 N = FMath::Max(BIn.Num(), AIn.Num());
		TArray<double> B, A;
		B.Init(0.0, N);
		A.Init(0.0, N);
		for (int32 I = 0; I < BIn.Num(); ++I)
		{
			B[I] = BIn[I] / AIn[0];
		}
		for (int32 I = 0; I < AIn.Num(); ++I)
		{
			A[I] = AIn[I] / AIn[0];
		}
		const int32 PadLen = 3 * N;
		if (X.Num() <= PadLen)
		{
			return X;   // scipy raises; Pose2Sim never calls it on such short runs
		}

		// Odd extension: 2*x[0] - x[padlen:0:-1], x, 2*x[-1] - x[-2:-padlen-2:-1]
		TArray<double> Ext;
		Ext.Reserve(X.Num() + 2 * PadLen);
		for (int32 I = PadLen; I >= 1; --I)
		{
			Ext.Add(2.0 * X[0] - X[I]);
		}
		Ext.Append(X);
		const int32 Last = X.Num() - 1;
		for (int32 I = 1; I <= PadLen; ++I)
		{
			Ext.Add(2.0 * X[Last] - X[Last - I]);
		}

		const TArray<double> Zi = LFilterZi(B, A);
		auto Scaled = [&Zi](double Factor)
		{
			TArray<double> Z = Zi;
			for (double& V : Z)
			{
				V *= Factor;
			}
			return Z;
		};
		TArray<double> Forward = LFilter(B, A, Ext, Scaled(Ext[0]));
		Algo::Reverse(Forward);
		TArray<double> Backward = LFilter(B, A, Forward, Scaled(Forward[0]));
		Algo::Reverse(Backward);
		return TArray<double>(Backward.GetData() + PadLen, X.Num());
	}

	double LiveCutoffHz(double CutoffHz, double FrameRate)
	{
		return (FrameRate > 0.0 && CutoffHz / (FrameRate / 2.0) >= 1.0) ? FrameRate / (2.0 + 0.001) : CutoffHz;
	}

	TArray<double> ButterworthFilter1D(const TArray<double>& Values, int32 Order, double CutoffHz, double FrameRate)
	{
		TArray<double> B, A;
		const double IntCutoff = std::trunc(CutoffHz);   // Pose2Sim: int(cut_off_frequency)
		if (FrameRate <= 0.0 || !ButterLowpass(Order / 2.0, IntCutoff / (FrameRate / 2.0), B, A))
		{
			return Values;
		}
		const int32 PadLen = 3 * FMath::Max(A.Num(), B.Num());

		// Runs of consecutive valid (non-NaN, non-zero) values longer than the padding.
		TArray<double> Out = Values;
		int32 RunStart = INDEX_NONE;
		for (int32 I = 0; I <= Values.Num(); ++I)
		{
			const bool bValid = I < Values.Num() && !std::isnan(Values[I]) && Values[I] != 0.0;
			if (bValid && RunStart == INDEX_NONE)
			{
				RunStart = I;
			}
			else if (!bValid && RunStart != INDEX_NONE)
			{
				const int32 Length = I - RunStart;
				if (Length > PadLen)
				{
					const TArray<double> Run(Values.GetData() + RunStart, Length);
					const TArray<double> Filtered = FiltFilt(B, A, Run);
					for (int32 J = 0; J < Length; ++J)
					{
						Out[RunStart + J] = Filtered[J];
					}
				}
				RunStart = INDEX_NONE;
			}
		}
		return Out;
	}

	TArray<double> ProcessColumn(const TArray<double>& Values, double FrameRate, int32 HampelWindow, const FSettings& Settings)
	{
		TArray<double> Column = Settings.bInterpolate ? InterpolateZerosNans(Values, Settings.InterpolateGapSmallerThan) : Values;

		int32 Start = 0, End = 0;
		FirstLastValidChunks(Column, Settings.MinChunkSize, Start, End);
		if (Start == 0 && End == 0)
		{
			TArray<double> Empty;
			Empty.Init(NaN(), Values.Num());
			return Empty;
		}
		if (Settings.bFillLargeGapsWithLastValue)
		{
			FillLargeGapsWithLastValue(Column, Start, End);
		}
		if (Settings.bRejectOutliers)
		{
			Column = HampelFilter(Column, HampelWindow, 2.0);
		}
		if (Settings.bFilter)
		{
			Column = ButterworthFilter1D(Column, Settings.ButterworthOrder, LiveCutoffHz(Settings.ButterworthCutoffHz, FrameRate), FrameRate);
		}
		return Column;
	}
}
