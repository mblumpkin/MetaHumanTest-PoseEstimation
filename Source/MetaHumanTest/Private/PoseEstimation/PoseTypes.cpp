#include "PoseEstimation/PoseTypes.h"

DEFINE_LOG_CATEGORY(LogPoseEstimation);

namespace PoseKeypoints
{
	const TCHAR* GetName(int32 Index)
	{
		static const TCHAR* Names[Num] =
		{
			TEXT("Nose"), TEXT("LEye"), TEXT("REye"), TEXT("LEar"), TEXT("REar"),
			TEXT("LShoulder"), TEXT("RShoulder"), TEXT("LElbow"), TEXT("RElbow"), TEXT("LWrist"), TEXT("RWrist"),
			TEXT("LHip"), TEXT("RHip"), TEXT("LKnee"), TEXT("RKnee"), TEXT("LAnkle"), TEXT("RAnkle"),
			TEXT("Head"), TEXT("Neck"), TEXT("Hip"),
			TEXT("LBigToe"), TEXT("RBigToe"), TEXT("LSmallToe"), TEXT("RSmallToe"), TEXT("LHeel"), TEXT("RHeel"),
		};
		return (Index >= 0 && Index < Num) ? Names[Index] : TEXT("?");
	}

	TConstArrayView<TPair<int32, int32>> GetBones()
	{
		// Pose2Sim skeletons.py, HALPE_26 tree (Hip is the root). Eyes and ears are not part of it.
		using E = EPoseKeypoint;
		auto B = [](E Parent, E Child) { return TPair<int32, int32>(static_cast<int32>(Parent), static_cast<int32>(Child)); };
		static const TPair<int32, int32> Bones[] =
		{
			B(E::Hip, E::RHip), B(E::RHip, E::RKnee), B(E::RKnee, E::RAnkle),
			B(E::RAnkle, E::RBigToe), B(E::RBigToe, E::RSmallToe), B(E::RAnkle, E::RHeel),
			B(E::Hip, E::LHip), B(E::LHip, E::LKnee), B(E::LKnee, E::LAnkle),
			B(E::LAnkle, E::LBigToe), B(E::LBigToe, E::LSmallToe), B(E::LAnkle, E::LHeel),
			B(E::Hip, E::Neck), B(E::Neck, E::Head), B(E::Head, E::Nose),
			B(E::Neck, E::RShoulder), B(E::RShoulder, E::RElbow), B(E::RElbow, E::RWrist),
			B(E::Neck, E::LShoulder), B(E::LShoulder, E::LElbow), B(E::LElbow, E::LWrist),
		};
		return Bones;
	}
}
