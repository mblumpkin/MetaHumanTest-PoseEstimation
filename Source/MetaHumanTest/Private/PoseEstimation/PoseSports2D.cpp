#include "PoseEstimation/PoseSports2D.h"

#include "PoseEstimation/PoseEstimator.h"

#include <limits>

namespace PoseSports2D
{
	static constexpr int32 KeypointId(EPoseKeypoint Keypoint) { return static_cast<int32>(Keypoint); }
	static constexpr int32 MissingKeypoint = -1;   // RIndex / LIndex: not in HALPE_26

	/** Python's % (floor modulo: the result has the sign of the divisor). */
	static double PyFloorMod(double Value, double Divisor)
	{
		return Value - Divisor * FMath::Floor(Value / Divisor);
	}

	static bool KeypointNameStartsWith(int32 Keypoint, TCHAR Side)
	{
		return PoseKeypoints::GetName(Keypoint)[0] == Side;
	}

	/** rtmlib nms (Sports2D uses it at pose level): areas with +1, a box survives while its overlap <= threshold. */
	static TArray<int32> PoseLevelNms(const TArray<FPoseBox>& Boxes, const TArray<double>& Scores, double Threshold)
	{
		TArray<int32> Order;
		for (int32 Index = 0; Index < Boxes.Num(); ++Index)
		{
			Order.Add(Index);
		}
		Order.StableSort([&Scores](int32 A, int32 B) { return Scores[A] > Scores[B]; });

		auto Area = [](const FPoseBox& Box) { return (Box.X2 - Box.X1 + 1.0) * (Box.Y2 - Box.Y1 + 1.0); };
		TArray<int32> Keep;
		while (Order.Num() > 0)
		{
			const int32 Best = Order[0];
			Keep.Add(Best);
			TArray<int32> Remaining;
			for (int32 I = 1; I < Order.Num(); ++I)
			{
				const FPoseBox& A = Boxes[Best];
				const FPoseBox& B = Boxes[Order[I]];
				const double W = FMath::Max(0.0, FMath::Min(A.X2, B.X2) - FMath::Max(A.X1, B.X1) + 1.0);
				const double H = FMath::Max(0.0, FMath::Min(A.Y2, B.Y2) - FMath::Max(A.Y1, B.Y1) + 1.0);
				const double Inter = W * H;
				if (Inter / (Area(A) + Area(B) - Inter) <= Threshold)
				{
					Remaining.Add(Order[I]);
				}
			}
			Order = MoveTemp(Remaining);
		}
		return Keep;
	}
}

namespace PoseSports2D
{
	TConstArrayView<FAngleDefinition> GetAngleDefinitions()
	{
		using E = EPoseKeypoint;
		constexpr int32 Missing = MissingKeypoint;
		// Pose2Sim common.py angle_dict, in the order of Sports2D's default joint_angles + segment_angles.
		// Joint angles: 4 points = between segments (Knee->Ankle) and (Toe->Heel), etc.; 3 points = around the middle point.
		static const FAngleDefinition Definitions[] =
		{
			{ TEXT("right ankle"),    { KeypointId(E::RKnee), KeypointId(E::RAnkle), KeypointId(E::RBigToe), KeypointId(E::RHeel) }, 4, 90.0, 1.0, true },
			{ TEXT("left ankle"),     { KeypointId(E::LKnee), KeypointId(E::LAnkle), KeypointId(E::LBigToe), KeypointId(E::LHeel) }, 4, 90.0, 1.0, true },
			{ TEXT("right knee"),     { KeypointId(E::RAnkle), KeypointId(E::RKnee), KeypointId(E::RHip), Missing }, 3, -180.0, 1.0, true },
			{ TEXT("left knee"),      { KeypointId(E::LAnkle), KeypointId(E::LKnee), KeypointId(E::LHip), Missing }, 3, -180.0, 1.0, true },
			{ TEXT("right hip"),      { KeypointId(E::RKnee), KeypointId(E::RHip), KeypointId(E::Hip), KeypointId(E::Neck) }, 4, 0.0, -1.0, true },
			{ TEXT("left hip"),       { KeypointId(E::LKnee), KeypointId(E::LHip), KeypointId(E::Hip), KeypointId(E::Neck) }, 4, 0.0, -1.0, true },
			{ TEXT("right shoulder"), { KeypointId(E::RElbow), KeypointId(E::RShoulder), KeypointId(E::Hip), KeypointId(E::Neck) }, 4, 0.0, -1.0, true },
			{ TEXT("left shoulder"),  { KeypointId(E::LElbow), KeypointId(E::LShoulder), KeypointId(E::Hip), KeypointId(E::Neck) }, 4, 0.0, -1.0, true },
			{ TEXT("right elbow"),    { KeypointId(E::RWrist), KeypointId(E::RElbow), KeypointId(E::RShoulder), Missing }, 3, 180.0, -1.0, true },
			{ TEXT("left elbow"),     { KeypointId(E::LWrist), KeypointId(E::LElbow), KeypointId(E::LShoulder), Missing }, 3, 180.0, -1.0, true },
			{ TEXT("right wrist"),    { KeypointId(E::RElbow), KeypointId(E::RWrist), Missing, Missing }, 3, -180.0, 1.0, true },   // needs RIndex
			{ TEXT("left wrist"),     { KeypointId(E::LElbow), Missing, KeypointId(E::LWrist), Missing }, 3, -180.0, 1.0, true },   // needs LIndex
			{ TEXT("right foot"),     { KeypointId(E::RBigToe), KeypointId(E::RHeel), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("left foot"),      { KeypointId(E::LBigToe), KeypointId(E::LHeel), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("right shank"),    { KeypointId(E::RAnkle), KeypointId(E::RKnee), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("left shank"),     { KeypointId(E::LAnkle), KeypointId(E::LKnee), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("right thigh"),    { KeypointId(E::RKnee), KeypointId(E::RHip), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("left thigh"),     { KeypointId(E::LKnee), KeypointId(E::LHip), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("pelvis"),         { KeypointId(E::LHip), KeypointId(E::RHip), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("trunk"),          { KeypointId(E::Neck), KeypointId(E::Hip), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("shoulders"),      { KeypointId(E::LShoulder), KeypointId(E::RShoulder), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("head"),           { KeypointId(E::Head), KeypointId(E::Neck), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("right arm"),      { KeypointId(E::RElbow), KeypointId(E::RShoulder), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("left arm"),       { KeypointId(E::LElbow), KeypointId(E::LShoulder), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("right forearm"),  { KeypointId(E::RWrist), KeypointId(E::RElbow), Missing, Missing }, 2, 0.0, -1.0, false },
			{ TEXT("left forearm"),   { KeypointId(E::LWrist), KeypointId(E::LElbow), Missing, Missing }, 2, 0.0, -1.0, false },
		};
		return Definitions;
	}

	double ComputeAngle(const FAngleDefinition& Definition, TConstArrayView<FVector2D> Points, TConstArrayView<bool> Valid)
	{
		FVector2D P[4];
		for (int32 I = 0; I < Definition.NumKeypoints; ++I)
		{
			const int32 Keypoint = Definition.Keypoints[I];
			if (Keypoint < 0 || !Points.IsValidIndex(Keypoint) || !Valid[Keypoint])
			{
				return std::numeric_limits<double>::quiet_NaN();   // a missing / low-confidence keypoint (Sports2D: NaN)
			}
			P[I] = Points[Keypoint];
		}

		// points_to_angles (2D)
		double Degrees;
		if (Definition.NumKeypoints == 2)
		{
			const FVector2D U = P[0] - P[1];
			Degrees = FMath::RadiansToDegrees(FMath::Atan2(U.Y, U.X));   // clockwise vs. horizontal (image y points down)
		}
		else
		{
			const FVector2D U = Definition.NumKeypoints == 3 ? P[0] - P[1] : P[1] - P[0];
			const FVector2D V = Definition.NumKeypoints == 3 ? P[2] - P[1] : P[3] - P[2];
			const double Cross = U.X * V.Y - U.Y * V.X;
			const double Dot = U.X * V.X + U.Y * V.Y;
			Degrees = FMath::RadiansToDegrees(-FMath::Atan2(Cross, Dot));
		}

		// fixed_angles: offset, sign, then wrap to (-180, 180] (pelvis / shoulders: (-90, 90])
		Degrees = (Degrees + Definition.Offset) * Definition.Sign;
		const bool bHalfTurn = FCString::Strcmp(Definition.Name, TEXT("pelvis")) == 0 || FCString::Strcmp(Definition.Name, TEXT("shoulders")) == 0;
		return bHalfTurn ? PyFloorMod(Degrees + 90.0, 180.0) - 90.0 : PyFloorMod(Degrees + 180.0, 360.0) - 180.0;
	}

	void ComputeAngles(FPosePerson& Person, EPoseVisibleSide VisibleSide)
	{
		const int32 Num = Person.Keypoints.Num();
		TArray<FVector2D> Points;
		TArray<bool> Valid;
		Points.SetNum(Num);
		Valid.SetNum(Num);
		for (int32 I = 0; I < Num; ++I)
		{
			Points[I] = Person.Keypoints[I].ImagePosition;
			Valid[I] = Person.Keypoints[I].bValid;
		}

		// compute_angles_for_person: "auto" = right if the feet point right (sum of heel -> big toe x), else left.
		// A missing foot keypoint makes the sum NaN, and NaN >= 0 is false in Python: Sports2D then says "left".
		using E = EPoseKeypoint;
		EPoseVisibleSide Side = VisibleSide;
		if (Side == EPoseVisibleSide::Auto)
		{
			const int32 Feet[4] = { KeypointId(E::RBigToe), KeypointId(E::RHeel), KeypointId(E::LBigToe), KeypointId(E::LHeel) };
			bool bFeetValid = Num > KeypointId(E::RHeel);
			for (int32 Keypoint : Feet)
			{
				bFeetValid = bFeetValid && Valid[Keypoint];
			}
			const double Orientation = bFeetValid
				? (Points[KeypointId(E::RBigToe)].X - Points[KeypointId(E::RHeel)].X) + (Points[KeypointId(E::LBigToe)].X - Points[KeypointId(E::LHeel)].X)
				: -1.0;
			Side = Orientation >= 0.0 ? EPoseVisibleSide::Right : EPoseVisibleSide::Left;
		}
		Person.VisibleSide = Side;

		// Flip x so the angles keep the same meaning whichever way the person faces.
		for (int32 I = 0; I < Num; ++I)
		{
			const bool bFlip = Side == EPoseVisibleSide::Left
				|| (Side == EPoseVisibleSide::Front && KeypointNameStartsWith(I, TEXT('R')))
				|| (Side == EPoseVisibleSide::Back && KeypointNameStartsWith(I, TEXT('L')));
			if (bFlip)
			{
				Points[I].X = -Points[I].X;
			}
		}

		Person.Angles.Reset();
		for (const FAngleDefinition& Definition : GetAngleDefinitions())
		{
			const double Degrees = ComputeAngle(Definition, Points, Valid);
			FPoseAngle& Angle = Person.Angles.AddDefaulted_GetRef();
			Angle.Name = FName(Definition.Name);
			Angle.bJointAngle = Definition.bJoint;
			Angle.bValid = !FMath::IsNaN(Degrees);
			Angle.Degrees = Angle.bValid ? static_cast<float>(Degrees) : 0.f;
		}
	}

	void BuildFrameResult(const FPoseRawFrame& Raw, const FIntPoint& ImageSize, const FVector2D& ImageToScreenOffset,
		const FVector2D& ImageToScreenScale, const FSettings& Settings, FPoseFrameResult& Out)
	{
		Out.ImageSize = ImageSize;
		Out.ImageToScreenOffset = ImageToScreenOffset;
		Out.ImageToScreenScale = ImageToScreenScale;
		Out.bDetectorRan = Raw.bDetectorRan;
		Out.DetectorMs = static_cast<float>(Raw.DetectorMs);
		Out.PoseMs = static_cast<float>(Raw.PoseMs);
		Out.Persons.Reset();
		Out.PrimaryPerson = INDEX_NONE;

		// "Non maximum suppression (at pose level, not detection, and only using likely keypoints)":
		// people whose mean score is > 0.2, boxed by their keypoints (clipped to the image), NMS at 0.45.
		TArray<int32> Candidates;
		TArray<FPoseBox> CandidateBoxes;
		TArray<double> CandidateScores;
		for (int32 PersonIndex = 0; PersonIndex < Raw.Persons.Num(); ++PersonIndex)
		{
			const FPoseRawPerson& RawPerson = Raw.Persons[PersonIndex];
			if (RawPerson.Scores.IsEmpty())
			{
				continue;
			}
			double Mean = 0.0;
			for (float Score : RawPerson.Scores)
			{
				Mean += Score;
			}
			Mean /= RawPerson.Scores.Num();
			if (Mean <= 0.2)
			{
				continue;
			}
			FPoseBox Box{ TNumericLimits<double>::Max(), TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest(), TNumericLimits<double>::Lowest() };
			for (const FVector2D& P : RawPerson.Keypoints)
			{
				Box.X1 = FMath::Min(Box.X1, P.X);
				Box.Y1 = FMath::Min(Box.Y1, P.Y);
				Box.X2 = FMath::Max(Box.X2, P.X);
				Box.Y2 = FMath::Max(Box.Y2, P.Y);
			}
			Box.X1 = FMath::Clamp(Box.X1, 0.0, static_cast<double>(ImageSize.X));
			Box.X2 = FMath::Clamp(Box.X2, 0.0, static_cast<double>(ImageSize.X));
			Box.Y1 = FMath::Clamp(Box.Y1, 0.0, static_cast<double>(ImageSize.Y));
			Box.Y2 = FMath::Clamp(Box.Y2, 0.0, static_cast<double>(ImageSize.Y));
			Candidates.Add(PersonIndex);
			CandidateBoxes.Add(Box);
			CandidateScores.Add(Mean);
		}

		for (int32 Kept : PoseLevelNms(CandidateBoxes, CandidateScores, 0.45))
		{
			const FPoseRawPerson& RawPerson = Raw.Persons[Candidates[Kept]];
			FPosePerson& Person = Out.Persons.AddDefaulted_GetRef();
			Person.BoxMin = FVector2D(RawPerson.Box.X1, RawPerson.Box.Y1);
			Person.BoxMax = FVector2D(RawPerson.Box.X2, RawPerson.Box.Y2);
			Person.Keypoints.SetNum(RawPerson.Keypoints.Num());
			double ValidScoreSum = 0.0;
			for (int32 Index = 0; Index < RawPerson.Keypoints.Num(); ++Index)
			{
				FPoseKeypoint& Keypoint = Person.Keypoints[Index];
				Keypoint.ImagePosition = RawPerson.Keypoints[Index];
				Keypoint.ScreenUV = Out.ImageToScreenUV(Keypoint.ImagePosition);
				Keypoint.Score = RawPerson.Scores[Index];
				Keypoint.bValid = Keypoint.Score >= Settings.KeypointLikelihoodThreshold;
				if (Keypoint.bValid)
				{
					++Person.NumValidKeypoints;
					ValidScoreSum += Keypoint.Score;
				}
			}
			Person.AverageScore = Person.NumValidKeypoints > 0 ? static_cast<float>(ValidScoreSum / Person.NumValidKeypoints) : 0.f;
			Person.bValid = Person.NumValidKeypoints >= Person.Keypoints.Num() * Settings.KeypointNumberThreshold
				&& Person.AverageScore >= Settings.AverageLikelihoodThreshold;
			if (Person.bValid)
			{
				ComputeAngles(Person, Settings.VisibleSide);
				if (Out.PrimaryPerson == INDEX_NONE)
				{
					Out.PrimaryPerson = Out.Persons.Num() - 1;
				}
			}
		}
	}

	FString ResultToJson(const FPoseFrameResult& Result, const FString& Name)
	{
		static const TCHAR* SideNames[] = { TEXT("auto"), TEXT("right"), TEXT("left"), TEXT("front"), TEXT("back"), TEXT("none") };
		FString Json = FString::Printf(TEXT("{\"name\": \"%s\", \"angle_names\": ["), *Name.ReplaceCharWithEscapedChar());
		const TConstArrayView<FAngleDefinition> Definitions = GetAngleDefinitions();
		for (int32 I = 0; I < Definitions.Num(); ++I)
		{
			Json += FString::Printf(TEXT("%s\"%s\""), I ? TEXT(", ") : TEXT(""), Definitions[I].Name);
		}
		Json += TEXT("], \"persons\": [");
		for (int32 P = 0; P < Result.Persons.Num(); ++P)
		{
			const FPosePerson& Person = Result.Persons[P];
			Json += FString::Printf(TEXT("%s{\"valid\": %s, \"visible_side\": \"%s\", \"angles\": ["), P ? TEXT(", ") : TEXT(""),
				Person.bValid ? TEXT("true") : TEXT("false"), Person.bValid ? SideNames[static_cast<int32>(Person.VisibleSide)] : TEXT(""));
			for (int32 I = 0; I < Person.Angles.Num(); ++I)
			{
				const FPoseAngle& Angle = Person.Angles[I];
				Json += (I ? TEXT(", ") : TEXT("")) + (Angle.bValid ? FString::Printf(TEXT("%.6f"), Angle.Degrees) : FString(TEXT("null")));
			}
			Json += TEXT("]}");
		}
		Json += TEXT("]}");
		return Json;
	}
}
