#include "PoseEstimation/CPP_UW_PoseOverlay.h"

#include "PoseEstimation/PoseSports2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SWidget.h"

namespace PoseOverlayPrivate
{
	// Sizes from Sports2D / Pose2Sim, in image pixels.
	constexpr double LineThickness = 2.0;        // Pose2Sim common.py "thickness": skeleton, angle lines and arcs
	constexpr double BoxThickness = 1.0;         // process.py: thickness = 1 if fontSize < 0.8 (boxes, text)
	constexpr double KeypointRadius = 6.0;       // draw_keypts: thickness + 4
	constexpr double BoxMargin = 25.0;           // draw_bounding_box
	constexpr double SegmentLength = 20.0;       // draw_segment_angle
	constexpr double JointLength = 40.0;         // draw_joint_angle
	constexpr double ArcRadius = 20.0;
	constexpr double DotGap = 7.0;               // draw_dotted_line
	constexpr double DotLength = 3.0;

	// OpenCV's Hershey font is ~26.7 px tall (capitals) per unit of fontScale (getTextSize: 8 px at 0.3, 14 at 0.5).
	// The overlay uses the engine's Roboto: capital height 0.711 em, 1 pt = 4/3 px -> 0.948 px per pt.
	constexpr double HersheyCapPerScale = 26.7;
	constexpr double RobotoCapPerPoint = 0.948;

	FLinearColor FromBGR(uint8 B, uint8 G, uint8 R, float Alpha = 1.f)
	{
		FLinearColor Color = FLinearColor::FromSRGBColor(FColor(R, G, B));
		Color.A = Alpha;
		return Color;
	}

	/** np.int32 / int(): truncation toward zero, per axis. */
	FVector2D Trunc(const FVector2D& V)
	{
		return FVector2D(FMath::TruncToDouble(V.X), FMath::TruncToDouble(V.Y));
	}

	/** Everything needed to draw in image pixels and land on the right screen spot. */
	struct FPainter
	{
		const FGeometry& Geometry;
		FSlateWindowElementList& Elements;
		FVector2f Size;              // widget size (local units)
		FVector2D UVOffset;          // image pixel -> fraction of the viewport
		FVector2D UVScale;
		double LocalPerPixel;        // widget units per image pixel
		double Scale;                // SizeScale
		FLinearColor Tint;
		const FSlateBrush* DotBrush;
		const FSlateBrush* SolidBrush;

		FVector2f ToLocal(const FVector2D& ImagePixel) const
		{
			return FVector2f(UVOffset + ImagePixel * UVScale) * Size;
		}

		float Thickness(double ImagePixels) const
		{
			return static_cast<float>(FMath::Max(1.0, ImagePixels * LocalPerPixel * Scale));
		}

		void Line(int32 Layer, const FVector2f& A, const FVector2f& B, const FLinearColor& Color, double ThicknessPixels) const
		{
			TArray<FVector2f> Points = { A, B };
			FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color * Tint, true, Thickness(ThicknessPixels));
		}

		void Polyline(int32 Layer, const TArray<FVector2f>& Points, const FLinearColor& Color, double ThicknessPixels) const
		{
			FSlateDrawElement::MakeLines(Elements, Layer, Geometry.ToPaintGeometry(), Points, ESlateDrawEffect::None, Color * Tint, true, Thickness(ThicknessPixels));
		}

		void Dot(int32 Layer, const FVector2f& Center, double RadiusPixels, const FLinearColor& Color) const
		{
			const float Radius = static_cast<float>(RadiusPixels * LocalPerPixel * Scale);
			const FVector2f DotSize(Radius * 2.f, Radius * 2.f);
			FSlateDrawElement::MakeBox(Elements, Layer, Geometry.ToPaintGeometry(DotSize, FSlateLayoutTransform(Center - DotSize * 0.5f)),
				DotBrush, ESlateDrawEffect::None, Color * Tint);
		}

		void Rect(int32 Layer, const FVector2f& Min, const FVector2f& Max, const FLinearColor& Color) const
		{
			const FVector2f TopLeft(FMath::Min(Min.X, Max.X), FMath::Min(Min.Y, Max.Y));
			const FVector2f RectSize(FMath::Abs(Max.X - Min.X), FMath::Abs(Max.Y - Min.Y));
			if (RectSize.X > 0.f && RectSize.Y > 0.f)
			{
				FSlateDrawElement::MakeBox(Elements, Layer, Geometry.ToPaintGeometry(RectSize, FSlateLayoutTransform(TopLeft)),
					SolidBrush, ESlateDrawEffect::None, Color * Tint);
			}
		}

		/** cv2.putText: Origin is the left end of the text's baseline. Outline = Sports2D's darker copy drawn underneath. */
		void Text(int32 Layer, const FString& String, const FVector2f& Origin, double FontScale, const FLinearColor& Color,
			const FLinearColor* OutlineColor, bool bBold = false) const
		{
			const float Points = static_cast<float>(HersheyCapPerScale * FontScale * LocalPerPixel * Scale / RobotoCapPerPoint);
			FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), FMath::Max(4.f, Points));
			if (OutlineColor)
			{
				Font.OutlineSettings.OutlineSize = 1;
				Font.OutlineSettings.OutlineColor = *OutlineColor * Tint;
			}
			const TSharedRef<FSlateFontMeasure> Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
			const FVector2f TextSize = FVector2f(Measure->Measure(String, Font));
			const float BaselineFromTop = static_cast<float>(Measure->GetMaxCharacterHeight(Font) + Measure->GetBaseline(Font));
			FSlateDrawElement::MakeText(Elements, Layer, Geometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(FVector2f(Origin.X, Origin.Y - BaselineFromTop))),
				String, Font, ESlateDrawEffect::None, Color * Tint);
		}
	};

	/** draw_angles' flip: which way the horizontal reference points, from the visible side. */
	double AngleFlip(EPoseVisibleSide Side, const FString& AngleName)
	{
		if (AngleName == TEXT("pelvis") || AngleName == TEXT("trunk") || AngleName == TEXT("shoulders") || AngleName == TEXT("head"))
		{
			return 1.0;
		}
		switch (Side)
		{
		case EPoseVisibleSide::Left:  return -1.0;
		case EPoseVisibleSide::Front: return AngleName.StartsWith(TEXT("right")) ? -1.0 : 1.0;
		case EPoseVisibleSide::Back:  return AngleName.StartsWith(TEXT("left")) ? -1.0 : 1.0;
		default:                      return 1.0;
		}
	}
}

using namespace PoseOverlayPrivate;

void UCPP_UW_PoseOverlay::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	SetVisibility(ESlateVisibility::HitTestInvisible);   // never block orbit / pan / muscle picking

	// A white rounded box whose corner radius is half its height = a circle of any size; tinted per keypoint.
	DotBrush.DrawAs = ESlateBrushDrawType::RoundedBox;
	DotBrush.TintColor = FSlateColor(FLinearColor::White);
	DotBrush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
	DotBrush.OutlineSettings.Width = 0.f;

	SolidBrush.DrawAs = ESlateBrushDrawType::Box;
	SolidBrush.TintColor = FSlateColor(FLinearColor::White);
}

void UCPP_UW_PoseOverlay::RequestRepaint()
{
	if (const TSharedPtr<SWidget> SlateWidget = GetCachedWidget())
	{
		SlateWidget->Invalidate(EInvalidateWidgetReason::Paint);
	}
}

void UCPP_UW_PoseOverlay::SetPoseResult(const FPoseFrameResult& InResult)
{
	Result = InResult;
	bHasResult = true;
	RequestRepaint();
}

void UCPP_UW_PoseOverlay::ClearPose()
{
	Result = FPoseFrameResult();
	bHasResult = false;
	RequestRepaint();
}

FLinearColor UCPP_UW_PoseOverlay::ScoreColor(float Score)
{
	// matplotlib RdYlGn (ColorBrewer, 11 colours, sRGB), linearly interpolated. Sports2D draw_keypts clips at 0.99.
	static const FColor Stops[] =
	{
		FColor(165, 0, 38), FColor(215, 48, 39), FColor(244, 109, 67), FColor(253, 174, 97), FColor(254, 224, 139),
		FColor(255, 255, 191), FColor(217, 239, 139), FColor(166, 217, 106), FColor(102, 189, 99), FColor(26, 152, 80),
		FColor(0, 104, 55),
	};
	constexpr int32 NumStops = UE_ARRAY_COUNT(Stops);
	const float T = FMath::Clamp(FMath::IsNaN(Score) ? 0.f : Score, 0.f, 0.99f) * (NumStops - 1);
	const int32 Index = FMath::Min(FMath::FloorToInt32(T), NumStops - 2);
	const float Alpha = T - Index;
	const FColor& A = Stops[Index];
	const FColor& B = Stops[Index + 1];
	const FColor Mixed(
		static_cast<uint8>(FMath::RoundToInt32(FMath::Lerp<float>(A.R, B.R, Alpha))),
		static_cast<uint8>(FMath::RoundToInt32(FMath::Lerp<float>(A.G, B.G, Alpha))),
		static_cast<uint8>(FMath::RoundToInt32(FMath::Lerp<float>(A.B, B.B, Alpha))));
	return FLinearColor::FromSRGBColor(Mixed);
}

FLinearColor UCPP_UW_PoseOverlay::PersonColor(int32 PersonIndex)
{
	// Pose2Sim common.py "colors" (OpenCV order: blue, green, red).
	static const uint8 ColorsBGR[][3] =
	{
		{255, 0, 0}, {0, 0, 255}, {255, 255, 0}, {255, 0, 255}, {0, 255, 255}, {0, 0, 0}, {255, 255, 255},
		{125, 0, 0}, {0, 125, 0}, {0, 0, 125}, {125, 125, 0}, {125, 0, 125}, {0, 125, 125},
		{255, 125, 125}, {125, 255, 125}, {125, 125, 255}, {255, 255, 125}, {255, 125, 255}, {125, 255, 255}, {125, 125, 125},
		{255, 0, 125}, {255, 125, 0}, {0, 125, 255}, {0, 255, 125}, {125, 0, 255}, {125, 255, 0}, {0, 255, 0},
	};
	const uint8* C = ColorsBGR[FMath::Max(0, PersonIndex) % UE_ARRAY_COUNT(ColorsBGR)];
	return FromBGR(C[0], C[1], C[2]);
}

int32 UCPP_UW_PoseOverlay::NativePaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	LayerId = Super::NativePaint(Args, AllottedGeometry, MyCullingRect, OutDrawElements, LayerId, InWidgetStyle, bParentEnabled);
	const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());
	if (!bHasResult || Result.ImageSize.X <= 0 || Result.ImageSize.Y <= 0 || Result.ImageToScreenScale.X <= 0.0 || Size.X <= 0.f)
	{
		return LayerId;
	}

	const FPainter P{ AllottedGeometry, OutDrawElements, Size, Result.ImageToScreenOffset, Result.ImageToScreenScale,
		Size.X * Result.ImageToScreenScale.X, SizeScale, InWidgetStyle.GetColorAndOpacityTint(), &DotBrush, &SolidBrush };
	const double S = SizeScale;   // Sports2D lengths are multiplied by this

	// Sports2D's drawing order: boxes, keypoints, skeleton, angles (later = on top).
	const int32 BoxLayer = LayerId + 1;
	const int32 KeypointLayer = LayerId + 2;
	const int32 SkeletonLayer = LayerId + 3;
	const int32 AngleLayer = LayerId + 4;
	const int32 TextLayer = LayerId + 5;

	const FLinearColor White = FLinearColor::White;
	const FLinearColor Black = FLinearColor::Black;
	const FLinearColor Green = FromBGR(0, 255, 0);
	const TConstArrayView<PoseSports2D::FAngleDefinition> Definitions = PoseSports2D::GetAngleDefinitions();

	for (int32 PersonIndex = 0; PersonIndex < Result.Persons.Num(); ++PersonIndex)
	{
		const FPosePerson& Person = Result.Persons[PersonIndex];
		const TArray<FPoseKeypoint>& K = Person.Keypoints;

		if (!Person.bValid)
		{
			// Sports2D sets rejected people to NaN: nothing is drawn. Optionally show their keypoints, faded.
			if (bShowRejectedPeople)
			{
				for (const FPoseKeypoint& Keypoint : K)
				{
					FLinearColor Color = ScoreColor(Keypoint.Score);
					Color.A = 0.35f;
					P.Dot(KeypointLayer, P.ToLocal(Trunc(Keypoint.ImagePosition)), KeypointRadius, Color);
				}
			}
			continue;
		}

		if (bShowModelBox)
		{
			const FVector2f Min = P.ToLocal(Person.BoxMin);
			const FVector2f Max = P.ToLocal(Person.BoxMax);
			P.Polyline(BoxLayer, { Min, FVector2f(Max.X, Min.Y), Max, FVector2f(Min.X, Max.Y), Min }, ModelBoxColor, 1.0);
		}

		// 1. draw_bounding_box: around the valid keypoints, 25 px margin, person number above it.
		if (bShowBoundingBoxes)
		{
			double MinX = TNumericLimits<double>::Max(), MinY = TNumericLimits<double>::Max();
			double MaxX = TNumericLimits<double>::Lowest(), MaxY = TNumericLimits<double>::Lowest();
			for (const FPoseKeypoint& Keypoint : K)
			{
				if (Keypoint.bValid)
				{
					MinX = FMath::Min(MinX, Keypoint.ImagePosition.X);
					MinY = FMath::Min(MinY, Keypoint.ImagePosition.Y);
					MaxX = FMath::Max(MaxX, Keypoint.ImagePosition.X);
					MaxY = FMath::Max(MaxY, Keypoint.ImagePosition.Y);
				}
			}
			const double X0 = FMath::Max(0.0, FMath::TruncToDouble(MinX));
			const double Y0 = FMath::Max(0.0, FMath::TruncToDouble(MinY));
			const double X1 = FMath::Min(static_cast<double>(Result.ImageSize.X), FMath::TruncToDouble(MaxX));
			const double Y1 = FMath::Min(static_cast<double>(Result.ImageSize.Y), FMath::TruncToDouble(MaxY));
			const double Margin = BoxMargin * S;
			const FVector2f A = P.ToLocal(FVector2D(X0 - Margin, Y0 - Margin));
			const FVector2f B = P.ToLocal(FVector2D(X1 + Margin, Y1 + Margin));
			const FLinearColor Color = PersonColor(PersonIndex);
			P.Polyline(BoxLayer, { A, FVector2f(B.X, A.Y), B, FVector2f(A.X, B.Y), A }, Color, BoxThickness);
			P.Text(TextLayer, FString::FromInt(PersonIndex), P.ToLocal(FVector2D(X0 - 30.0 * S, Y0 - 30.0 * S)), FontSize, Color, nullptr, true);
		}

		// 2. draw_keypts: filled circles coloured by score.
		if (bShowKeypoints)
		{
			for (const FPoseKeypoint& Keypoint : K)
			{
				if (Keypoint.bValid)
				{
					P.Dot(KeypointLayer, P.ToLocal(Trunc(Keypoint.ImagePosition)), KeypointRadius, ScoreColor(Keypoint.Score));
				}
			}
		}

		// 3. draw_skel: right-side bones blue, left-side green, the rest orange.
		if (bShowSkeleton)
		{
			for (const TPair<int32, int32>& Bone : PoseKeypoints::GetBones())
			{
				if (!K.IsValidIndex(Bone.Key) || !K.IsValidIndex(Bone.Value) || !K[Bone.Key].bValid || !K[Bone.Value].bValid)
				{
					continue;
				}
				const TCHAR A = PoseKeypoints::GetName(Bone.Key)[0];
				const TCHAR B = PoseKeypoints::GetName(Bone.Value)[0];
				const bool bRight = (A == TEXT('R') || B == TEXT('R')) && A != TEXT('L') && B != TEXT('L');
				const bool bLeft = (A == TEXT('L') || B == TEXT('L')) && A != TEXT('R') && B != TEXT('R');
				const FLinearColor Color = bRight ? FromBGR(255, 128, 0) : bLeft ? FromBGR(0, 255, 0) : FromBGR(51, 153, 255);
				P.Line(SkeletonLayer, P.ToLocal(Trunc(K[Bone.Key].ImagePosition)), P.ToLocal(Trunc(K[Bone.Value].ImagePosition)), Color, LineThickness);
			}
		}

		// 4. draw_angles
		if (!bShowAngles && !bAngleValuesInList)
		{
			continue;
		}
		// The list: "person N" then one line per valid angle, positions as in Sports2D (relative to its 10 px margin).
		const FVector2f ListOrigin = FVector2f(ListPosition);
		const double ListScale = P.LocalPerPixel * S;
		auto ListToLocal = [&](const FVector2D& Sports2DPixel)
		{
			return ListOrigin + FVector2f((Sports2DPixel.X - 10.0) * ListScale, Sports2DPixel.Y * ListScale);
		};
		const FVector2D LabelPosition(FMath::TruncToDouble(10.0 + FontSize * 150.0 / 0.3 * PersonIndex), FMath::TruncToDouble(FontSize * 50.0));
		const FLinearColor PersonCol = PersonColor(PersonIndex);
		if (bAngleValuesInList)
		{
			P.Text(TextLayer, FString::Printf(TEXT("person %d"), PersonIndex), ListToLocal(LabelPosition), FontSize + 0.2, PersonCol, &White);
		}

		int32 ListLine = 1;
		for (int32 AngleIndex = 0; AngleIndex < Person.Angles.Num() && AngleIndex < Definitions.Num(); ++AngleIndex)
		{
			const FPoseAngle& Angle = Person.Angles[AngleIndex];
			const PoseSports2D::FAngleDefinition& Definition = Definitions[AngleIndex];
			if (!Angle.bValid)
			{
				continue;
			}
			const FString Name(Definition.Name);
			const FString Value = FString::Printf(TEXT("%.1f"), Angle.Degrees);
			const FLinearColor Color = Definition.bJoint ? Green : White;
			const double Flip = AngleFlip(Person.VisibleSide, Name);

			FVector2D Points[4];
			for (int32 I = 0; I < Definition.NumKeypoints; ++I)
			{
				Points[I] = K[Definition.Keypoints[I]].ImagePosition;
			}

			if (bShowAngles)
			{
				FVector2D TextAnchor, Vec1, Vec2;
				double TextDistance;
				bool bWriteText = true;
				if (Definition.NumKeypoints == 2)
				{
					// draw_segment_angle: the segment (from its middle) and a horizontal reference line, 20 px each.
					const FVector2D App = Trunc((Points[0] + Points[1]) * 0.5);
					const FVector2D Direction = Trunc(Points[0]) - Trunc(Points[1]);
					Vec1 = FVector2D::ZeroVector;
					if (!Direction.IsZero())
					{
						Vec1 = Direction / Direction.Size();
						P.Line(AngleLayer, P.ToLocal(App), P.ToLocal(Trunc(App + Vec1 * SegmentLength * S)), White, LineThickness);
						P.Line(AngleLayer, P.ToLocal(App), P.ToLocal(FVector2D(App.X + Flip * SegmentLength * S, App.Y)), White, LineThickness);
					}
					TextAnchor = App;
					Vec2 = FVector2D(1.0, 0.0);
					TextDistance = 20.0;
				}
				else
				{
					// draw_joint_angle: the segment (solid), the parent segment (dotted) and an arc between them.
					const FVector2D App = Trunc(Points[1]);
					FVector2D Segment = Trunc(Points[0] - Points[1]);
					FVector2D Parent = Trunc(Points[Definition.NumKeypoints - 2] - Points[Definition.NumKeypoints - 1]);
					TextAnchor = App;
					TextDistance = 40.0;
					if (Segment.IsZero() || Parent.IsZero())
					{
						bWriteText = false;
					}
					else
					{
						if (Definition.Offset == 90.0)   // ankle: the foot is compared to the perpendicular of the shank
						{
							Segment = FVector2D(-Flip * Segment.Y, Flip * Segment.X);
							Swap(Segment, Parent);
						}
						Vec1 = Segment / Segment.Size();
						Vec2 = Parent / Parent.Size();
						P.Line(AngleLayer, P.ToLocal(App), P.ToLocal(Trunc(App + Vec1 * JointLength * S)), Green, LineThickness);
						for (double Along = 0.0; Along < JointLength * S; Along += DotGap * S)
						{
							const FVector2D Start = App + Vec2 * Along;
							P.Line(AngleLayer, P.ToLocal(Trunc(Start)), P.ToLocal(Trunc(Start + Vec2 * DotLength * S)), Green, LineThickness);
						}
						double StartAngle = FMath::RadiansToDegrees(FMath::Atan2(Vec1.Y, Vec1.X));
						double EndAngle = FMath::RadiansToDegrees(FMath::Atan2(Vec2.Y, Vec2.X));
						if (FMath::Abs(EndAngle - StartAngle) > 180.0)
						{
							if (EndAngle > StartAngle)
							{
								StartAngle += 360.0;
							}
							else
							{
								EndAngle += 360.0;
							}
						}
						const double From = FMath::Min(StartAngle, EndAngle);
						const double To = FMath::Max(StartAngle, EndAngle);
						TArray<FVector2f> Arc;
						const int32 Steps = FMath::Max(2, FMath::CeilToInt32((To - From) / 6.0));
						for (int32 Step = 0; Step <= Steps; ++Step)
						{
							const double Radians = FMath::DegreesToRadians(From + (To - From) * Step / Steps);
							Arc.Add(P.ToLocal(App + FVector2D(FMath::Cos(Radians), FMath::Sin(Radians)) * ArcRadius * S));
						}
						P.Polyline(AngleLayer, Arc, Green, LineThickness);
					}
				}

				// write_angle_on_body: the value, pushed away from the joint along the bisector of the two directions.
				const FVector2D Sum = Vec1 + Vec2;
				if (bAngleValuesOnBody && bWriteText && !Sum.IsZero())
				{
					const FVector2D Position = Trunc(TextAnchor + Sum / Sum.Size() * TextDistance * S);
					P.Text(TextLayer, Value, P.ToLocal(Position), FontSize, Color, &Black);
				}
			}

			// write_angle_as_list: "name:  value  [bar]", the bar is 50 px per 180 degrees, white at 40 %.
			if (bAngleValuesInList)
			{
				const FVector2D NamePosition(LabelPosition.X, LabelPosition.Y + FMath::TruncToDouble(ListLine * 40.0 * FontSize));
				const FVector2D ValuePosition(NamePosition.X + FMath::TruncToDouble(250.0 * FontSize), NamePosition.Y);
				P.Text(TextLayer, Name + TEXT(":"), ListToLocal(NamePosition), FontSize, Color, &Black);
				P.Text(TextLayer, Value, ListToLocal(ValuePosition), FontSize, Color, &Black);
				const double Percent = FMath::TruncToDouble(Angle.Degrees * 50.0 / 180.0);
				const double BarX = NamePosition.X + FMath::TruncToDouble(300.0 * FontSize);
				const FVector2D BarMin(BarX, ValuePosition.Y - FMath::TruncToDouble(35.0 * FontSize));
				const FVector2D BarMax(BarX + FMath::TruncToDouble(Percent * FontSize / 0.3), ValuePosition.Y);
				P.Rect(AngleLayer, ListToLocal(BarMin), ListToLocal(BarMax), FLinearColor(1.f, 1.f, 1.f, 0.4f));
				++ListLine;
			}
		}
	}
	return TextLayer;
}
