#include "picture.h"

#include <Imath/half.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace iv
{
	namespace
	{
		uint8_t Code(float v) { return (uint8_t)std::clamp((int)(v + 0.5f), 0, 255); }
	}

	SourceRows::SourceRows(const Layout& layout, const uint8_t* pixels, const DrawParams& params) : l(layout), data(pixels), p(params), yuv(layout.packing == Packing::Yuv ? pixels : nullptr), yuvOk(layout.packing == Packing::Yuv && yuv.Fits(layout.size, layout.width, layout.height))
	{
		for (int i = 0; i < 256; ++i)
			lut8[i] = p.srgbIn ? SrgbToLinear(i / 255.0f) : i / 255.0f;
		if (l.format == DXGI_FORMAT_R16G16B16A16_UNORM)
		{
			lut16.resize(65536);
			for (int i = 0; i < 65536; ++i)
				lut16[i] = p.srgbIn ? SrgbToLinear(i / 65535.0f) : i / 65535.0f;
		}
	}

	void SourceRows::Row(int y, float* out) const
	{
		const uint8_t* row = data + (size_t)y * l.rowPitch;
		const uint16_t* r16 = (const uint16_t*)row;
		const int w = l.width;
		if (l.packing == Packing::Yuv)
			for (int x = 0; x < w; ++x)
			{
				uint8_t c[3] = {};
				if (yuvOk)
					yuv.Pixel(x, y, c);
				for (int k = 0; k < 3; ++k)
					out[x * 4 + k] = lut8[c[k]];
				out[x * 4 + 3] = 1.0f;
			}
		if (l.packing == Packing::Rgb)
			for (int x = 0; x < w; ++x)
			{
				const uint8_t* q = row + (size_t)x * l.bytesPerPixel;
				for (int c = 0; c < 3; ++c)
					out[x * 4 + c] = l.format == DXGI_FORMAT_R8G8B8A8_UNORM ? lut8[q[c]] : l.format == DXGI_FORMAT_R16G16B16A16_UNORM ? lut16[((const uint16_t*)q)[c]] : l.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? imath_half_to_float(((const uint16_t*)q)[c]) : ((const float*)q)[c];
				out[x * 4 + 3] = 1.0f;
			}
		switch (l.packing == Packing::Rgb || l.packing == Packing::Yuv ? DXGI_FORMAT_FORCE_UINT : l.packing == Packing::Rgbe ? DXGI_FORMAT_UNKNOWN : l.format)
		{
		case DXGI_FORMAT_FORCE_UINT:
			break;
		case DXGI_FORMAT_UNKNOWN:
			for (int x = 0; x < w; ++x)
				DecodeRgbe(row + x * 4, out + x * 4);
			break;
		case DXGI_FORMAT_R8G8B8A8_UNORM:
			for (int i = 0; i < w * 4; ++i)
				out[i] = (i & 3) == 3 ? row[i] / 255.0f : lut8[row[i]];
			break;
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			for (int i = 0; i < w * 4; ++i)
				out[i] = (i & 3) == 3 ? r16[i] / 65535.0f : lut16[r16[i]];
			break;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			for (int i = 0; i < w * 4; ++i)
				out[i] = imath_half_to_float(r16[i]);
			break;
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			memcpy(out, row, (size_t)w * 16);
			break;
		case DXGI_FORMAT_R16_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			for (int x = 0; x < w; ++x)
			{
				const float g = l.format == DXGI_FORMAT_R16_FLOAT ? imath_half_to_float(r16[x]) : ((const float*)row)[x];
				out[x * 4] = out[x * 4 + 1] = out[x * 4 + 2] = g, out[x * 4 + 3] = 1.0f;
			}
			break;
		default:
			std::fill(out, out + (size_t)w * 4, 0.0f);
			break;
		}
		for (int x = 0; x < w; ++x)
		{
			float* c = out + x * 4;
			if (p.gray)
			{
				const float g = c[0] * p.mask[0] + c[1] * p.mask[1] + c[2] * p.mask[2] + c[3] * p.mask[3];
				c[0] = c[1] = c[2] = g, c[3] = 1.0f;
			}
			else if (p.colorMatrix)
			{
				const float r = c[0], g = c[1], b = c[2];
				for (int k = 0; k < 3; ++k)
					c[k] = p.matrix[k * 3] * r + p.matrix[k * 3 + 1] * g + p.matrix[k * 3 + 2] * b;
			}
		}
	}

	void ComputeHistogram(const SourceRows& s, const AdjustState& adj, int maxSamples, DisplayHistogram& out, const Parallel& par)
	{
		const int w = s.l.width, h = s.l.height;
		const int step = std::max(1, (int)std::ceil(std::sqrt((double)w * h / std::max(maxSamples, 1))));
		const float gain = std::exp2(s.p.exposure);
		std::vector<DisplayHistogram> parts(par.MaxWorkers());
		std::vector<std::vector<float>> rows(par.MaxWorkers());
		auto bin = [](float v) { return std::clamp((int)(v * 255.0f + 0.5f), 0, 255); };
		par.For((h + step - 1) / step, 4, [&](int64_t r0, int64_t r1, int worker)
		{
			DisplayHistogram& part = parts[worker];
			std::vector<float>& row = rows[worker];
			row.resize((size_t)w * 4);
			for (int r = (int)r0; r < (int)r1; ++r)
			{
				s.Row(r * step, row.data());
				for (int x = 0; x < w; x += step)
				{
					float c[3] = { row[(size_t)x * 4], row[(size_t)x * 4 + 1], row[(size_t)x * 4 + 2] };
					ApplyGrade(adj, c);
					DisplayEncode(c, gain, s.p.view);
					for (int k = 0; k < 3; ++k)
						part.bins[0][k][bin(c[k])]++;
					ApplyLevels(adj, c);
					for (int k = 0; k < 3; ++k)
						part.bins[1][k][bin(c[k])]++;
					part.samples++;
				}
			}
		});
		out = {};
		for (const DisplayHistogram& p : parts)
		{
			for (int st = 0; st < 2; ++st)
				for (int k = 0; k < 3; ++k)
					for (int i = 0; i < 256; ++i)
						out.bins[st][k][i] += p.bins[st][k][i];
			out.samples += p.samples;
		}
	}

	void ToYuv420(const SourceRows& s, int scale, int width, int height, const Yuv420Target& dst, const AdjustState& adjust, const Parallel& par)
	{
		const int w = s.l.width, h = s.l.height, shift = scale == 4 ? 2 : scale == 2 ? 1 : 0;
		const int vw = (w + scale - 1) / scale, vh = (h + scale - 1) / scale;
		const float gain = std::exp2(s.p.exposure);
		constexpr float kr = 0.2126f, kg = 0.7152f, kb = 0.0722f;
		struct Scratch { std::vector<float> row, acc, rgb; };
		std::vector<Scratch> scratch(par.MaxWorkers());
		par.For(height / 2, 4, [&](int64_t p0, int64_t p1, int worker)
		{
			Scratch& t = scratch[worker];
			t.row.resize((size_t)w * 4);
			t.acc.resize((size_t)vw * 3);
			t.rgb.resize((size_t)width * 6);
			for (int pair = (int)p0; pair < (int)p1; ++pair)
			{
				// both rows of the pair as display values; output rows past the picture repeat its last row
				for (int r = 0; r < 2; ++r)
				{
					const int oy = std::min(pair * 2 + r, vh - 1), y0 = oy << shift, y1 = std::min(y0 + scale, h);
					float* out = t.rgb.data() + (size_t)r * width * 3;
					std::fill(t.acc.begin(), t.acc.end(), 0.0f);
					for (int y = y0; y < y1; ++y)
					{
						s.Row(y, t.row.data());
						for (int x = 0; x < w; ++x)
							for (int k = 0; k < 3; ++k)
								t.acc[(size_t)(x >> shift) * 3 + k] += t.row[(size_t)x * 4 + k];
					}
					for (int ox = 0; ox < vw; ++ox)
					{
						const float n = (float)((std::min((ox + 1) << shift, w) - (ox << shift)) * (y1 - y0));
						float* c = out + (size_t)ox * 3;
						for (int k = 0; k < 3; ++k)
							c[k] = t.acc[(size_t)ox * 3 + k] / n;
						ApplyGrade(adjust, c);
						DisplayEncode(c, gain, s.p.view);
						ApplyDisplayAdjust(adjust, c);
					}
					for (int ox = vw; ox < width; ++ox)
						std::copy_n(out + (size_t)(vw - 1) * 3, 3, out + (size_t)ox * 3);
				}
				const float* a = t.rgb.data();
				const float* b = a + (size_t)width * 3;
				uint8_t* ya = dst.y + (size_t)pair * 2 * dst.yStride;
				uint8_t* yb = ya + dst.yStride;
				uint8_t* cb = dst.cb + (size_t)pair * dst.cStride;
				uint8_t* cr = dst.cr + (size_t)pair * dst.cStride;
				for (int x = 0; x < width; x += 2)
				{
					const float* q[4] = { a + (size_t)x * 3, a + (size_t)x * 3 + 3, b + (size_t)x * 3, b + (size_t)x * 3 + 3 };
					uint8_t* yo[4] = { ya + x, ya + x + 1, yb + x, yb + x + 1 };
					float sr = 0.0f, sg = 0.0f, sb = 0.0f;
					for (int i = 0; i < 4; ++i)
					{
						*yo[i] = Code(16.0f + 219.0f * (kr * q[i][0] + kg * q[i][1] + kb * q[i][2]));
						sr += q[i][0], sg += q[i][1], sb += q[i][2];
					}
					sr *= 0.25f, sg *= 0.25f, sb *= 0.25f;
					const float yc = kr * sr + kg * sg + kb * sb;
					cb[(x >> 1) * dst.cStep] = Code(128.0f + 224.0f * (sb - yc) / 1.8556f);
					cr[(x >> 1) * dst.cStep] = Code(128.0f + 224.0f * (sr - yc) / 1.5748f);
				}
			}
		});
	}
}
