#pragma once

#include <cstdint>
#include <vector>

// colour adjustments: Nuke's Grade on the scene-linear values (before exposure and the view transform), then GIMP's Levels, Curves, Color Balance and
// Hue-Saturation on the displayed 0-1 values; the maths follows Foundry's NDK Grade.C and GIMP's gimpoperationlevels / gimpcurve / gimpoperationcolorbalance /
// gimpoperationhuesaturation (written from the formulas, no code taken); the shader (display_ps.hlsl) and the CPU functions below compute the same
namespace iv
{
	// Nuke's knobs per channel (R, G, B); left at the defaults it is no node at all, once touched the clamps act as in Nuke (black clamp also on identity channels)
	struct GradeParams
	{
		float blackpoint[3] = { 0.0f, 0.0f, 0.0f };
		float whitepoint[3] = { 1.0f, 1.0f, 1.0f };
		float lift[3] = { 0.0f, 0.0f, 0.0f };   // knob "black"
		float gain[3] = { 1.0f, 1.0f, 1.0f };   // knob "white"
		float multiply[3] = { 1.0f, 1.0f, 1.0f };
		float offset[3] = { 0.0f, 0.0f, 0.0f };   // knob "add"
		float gamma[3] = { 1.0f, 1.0f, 1.0f };
		bool reverse = false;
		bool blackClamp = true;
		bool whiteClamp = false;
		float mix = 1.0f;
		bool enabled = true;
		bool operator==(const GradeParams&) const = default;
	};

	// one channel of GIMP's levels, values 0-1 (the UI shows 0-255)
	struct LevelsChannel
	{
		float inLow = 0.0f, inHigh = 1.0f, gamma = 1.0f, outLow = 0.0f, outHigh = 1.0f;
		bool operator==(const LevelsChannel&) const = default;
	};
	struct LevelsParams
	{
		LevelsChannel ch[4];   // RGB, R, G, B: each channel first, then RGB
		bool enabled = true;
		bool operator==(const LevelsParams&) const = default;
	};

	struct CurvePoint
	{
		float x = 0.0f, y = 0.0f;
		bool corner = false;
		bool operator==(const CurvePoint&) const = default;
	};
	struct CurvesParams
	{
		std::vector<CurvePoint> points[4];   // RGB, R, G, B, x ascending; each channel first, then RGB
		bool enabled = true;
		CurvesParams() { for (auto& p : points) p = { { 0.0f, 0.0f }, { 1.0f, 1.0f } }; }
		bool operator==(const CurvesParams&) const = default;
	};

	// GIMP's colour balance per range (shadows, midtones, highlights), -1..1 (the UI shows -100..100)
	struct BalanceParams
	{
		float cyanRed[3] = {};
		float magentaGreen[3] = {};
		float yellowBlue[3] = {};
		bool preserveLuminosity = true;
		bool enabled = true;
		bool operator==(const BalanceParams&) const = default;
	};

	// GIMP's hue-saturation per range (master, reds, yellows, greens, cyans, blues, magentas), -1..1 (hue shown as -180..180 degrees), overlap 0..1
	struct HueSatParams
	{
		float hue[7] = {};
		float saturation[7] = {};
		float lightness[7] = {};
		float overlap = 0.0f;
		bool enabled = true;
		bool operator==(const HueSatParams&) const = default;
	};

	struct ColorAdjust
	{
		GradeParams grade;
		LevelsParams levels;
		CurvesParams curves;
		BalanceParams balance;
		HueSatParams hueSat;
		bool operator==(const ColorAdjust&) const = default;
	};

	// what the per-pixel functions read: the tools that change something, the grade's slope and offset, the curve tables
	struct AdjustState
	{
		bool grade = false, levels = false, curves = false, balance = false, hueSat = false;
		float gradeA[3] = {}, gradeB[3] = {}, gradeGamma[3] = {};
		bool gradeReverse = false, blackClamp = false, whiteClamp = false;
		float gradeMix = 1.0f;
		float lvInLow[4] = {}, lvInHigh[4] = {}, lvInvGamma[4] = {}, lvOutLow[4] = {}, lvOutHigh[4] = {};
		bool curveActive[4] = {};
		float curve[4][256] = {};
		float balanceAmount[3][3] = {};   // range (shadows, midtones, highlights) x channel (cyan-red, magenta-green, yellow-blue)
		bool preserveLuminosity = true;
		float hue[7] = {}, saturation[7] = {}, lightness[7] = {}, overlap = 0.0f;
		bool Any() const { return grade || levels || curves || balance || hueSat; }
		bool Display() const { return levels || curves || balance || hueSat; }
	};
	AdjustState CompileAdjust(const ColorAdjust& a);
	// GIMP's 256 curve samples: a cubic Bezier per segment, tangents towards the neighbours (corner points cut them), flat beyond the end points, control points exact
	void CurveSamples(const std::vector<CurvePoint>& points, float out[256]);
	// scene-linear rgb through the grade
	void ApplyGrade(const AdjustState& s, float* rgb);
	// displayed rgb through levels, curves, colour balance and hue-saturation, clamped to 0..1 after each as an 8-bit image would be
	void ApplyDisplayAdjust(const AdjustState& s, float* rgb);
	// only the levels (what Curves gets), clamped
	void ApplyLevels(const AdjustState& s, float* rgb);

	// cbuffer Adjust of display.hlsli, the same layout
	struct AdjustConstants
	{
		uint32_t flags;
		float gradeMix;
		float overlap;
		float pad;
		float gradeA[4];
		float gradeB[4];
		float gradeGamma[4];
		float levelsIn[4][4];   // per channel (RGB, R, G, B): low, high, 1 / gamma
		float levelsOut[4][4];   // low, high
		float balance[3][4];   // per range: cyan-red, magenta-green, yellow-blue
		float hueSat[7][4];   // per range: hue, saturation, lightness
		float curves[256][4];   // per sample: RGB, R, G, B
	};
	enum : uint32_t
	{
		kAdjGrade = 1, kAdjLevels = 2, kAdjCurves = 4, kAdjBalance = 8, kAdjHueSat = 16,
		kAdjReverse = 32, kAdjBlackClamp = 64, kAdjWhiteClamp = 128, kAdjPreserveLum = 256,
		kAdjCurveRgb = 512, kAdjCurveR = 1024, kAdjCurveG = 2048, kAdjCurveB = 4096,
	};
	void PackAdjust(const AdjustState& s, AdjustConstants& out);
}
