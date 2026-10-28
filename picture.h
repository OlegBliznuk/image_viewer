#pragma once

#include "adjust.h"
#include "display.h"
#include "image_io.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace iv
{
	// the picture as display_ps.hlsl computes it, on the CPU (Save As, video export); inline so the per-pixel loops compile as one
	inline float SrgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
	inline float LinearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
	inline float Saturate(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }

	// display_ps.hlsl's AcesFitted
	inline void AcesFitted(float* c)
	{
		float a[3] = { 0.59719f * c[0] + 0.35458f * c[1] + 0.04823f * c[2], 0.07600f * c[0] + 0.90834f * c[1] + 0.01566f * c[2], 0.02840f * c[0] + 0.13383f * c[1] + 0.83777f * c[2] };
		for (float& v : a)
			v = (v * (v + 0.0245786f) - 0.000090537f) / (v * (0.983729f * v + 0.4329510f) + 0.238081f);
		c[0] = Saturate(1.60475f * a[0] - 0.53108f * a[1] - 0.07367f * a[2]);
		c[1] = Saturate(-0.10208f * a[0] + 1.10813f * a[1] - 0.00605f * a[2]);
		c[2] = Saturate(-0.00327f * a[0] - 0.07276f * a[1] + 1.07602f * a[2]);
	}

	// exposure gain, view transform and display curve on linear RGB: the values on screen, each in [0, 1]
	inline void DisplayEncode(float* c, float gain, ViewTransform view)
	{
		for (int k = 0; k < 3; ++k)
			c[k] *= gain;
		if (view == ViewTransform::Aces)
			AcesFitted(c);
		for (int k = 0; k < 3; ++k)
			c[k] = view == ViewTransform::Linear ? Saturate(c[k]) : LinearToSrgb(Saturate(c[k]));
	}

	// the frame's rows as linear RGBA in Rec.709 primaries with the channel view applied, as the shader samples them at 1:1
	struct SourceRows
	{
		const Layout& l;
		const uint8_t* data;
		const DrawParams& p;
		const YuvReader yuv;
		const bool yuvOk;
		float lut8[256];
		std::vector<float> lut16;

		SourceRows(const Layout& layout, const uint8_t* pixels, const DrawParams& params);
		void Row(int y, float* out) const;
	};

	// output size of a frame shrunk by scale: ceil(size / scale) rounded up to even, as 4:2:0 needs
	inline int VideoSize(int size, int scale) { const int v = (size + scale - 1) / scale; return v + (v & 1); }

	// the frame as displayed (adjustments included), shrunk by scale (1, 2 or 4: box filter in linear light, the grade after it as the viewer grades the filtered texels),
	// as BT.709 limited-range 4:2:0 of width x height from VideoSize; the last picture column and row repeat into the padding, alpha is ignored
	void ToYuv420(const SourceRows& src, int scale, int width, int height, const Yuv420Target& dst, const AdjustState& adjust, const Parallel& par);

	// displayed values (grade, exposure, view transform) of every step-th pixel in both directions, at most about maxSamples: [0] what Levels gets, [1] what Curves gets
	struct DisplayHistogram
	{
		uint32_t bins[2][3][256] = {};
		uint64_t samples = 0;
	};
	void ComputeHistogram(const SourceRows& src, const AdjustState& adjust, int maxSamples, DisplayHistogram& out, const Parallel& par);
}
