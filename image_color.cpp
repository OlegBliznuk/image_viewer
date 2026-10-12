#include "image_formats.h"

#include <cmath>
#include <cstring>

// matrices taking linear RGB in a file's primaries to Rec.709 / sRGB primaries; the display applies them after decoding the sRGB curve
namespace iv::fmt
{
	namespace
	{
		using M3 = double[9];

		void Mul(const double* a, const double* b, double* out)
		{
			double r[9];
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j)
					r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
			memcpy(out, r, sizeof(r));
		}

		bool Invert(const double* m, double* out)
		{
			const double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
			const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
			if (std::fabs(det) < 1e-12)
				return false;
			const double r[9] = { c0, m[2] * m[7] - m[1] * m[8], m[1] * m[5] - m[2] * m[4], c1, m[0] * m[8] - m[2] * m[6], m[2] * m[3] - m[0] * m[5], c2, m[1] * m[6] - m[0] * m[7], m[0] * m[4] - m[1] * m[3] };
			for (int i = 0; i < 9; ++i)
				out[i] = r[i] / det;
			return true;
		}

		// RGB to XYZ from the chromaticities (rx, ry, gx, gy, bx, by, wx, wy), white at Y = 1
		bool RgbToXyz(const double* xy, double* out)
		{
			const double p[9] = { xy[0] / xy[1], xy[2] / xy[3], xy[4] / xy[5], 1, 1, 1, (1 - xy[0] - xy[1]) / xy[1], (1 - xy[2] - xy[3]) / xy[3], (1 - xy[4] - xy[5]) / xy[5] };
			double inv[9];
			if (!Invert(p, inv))
				return false;
			const double w[3] = { xy[6] / xy[7], 1, (1 - xy[6] - xy[7]) / xy[7] };
			for (int c = 0; c < 3; ++c)
			{
				const double s = inv[c * 3] * w[0] + inv[c * 3 + 1] * w[1] + inv[c * 3 + 2] * w[2];
				for (int r = 0; r < 3; ++r)
					out[r * 3 + c] = p[r * 3 + c] * s;
			}
			return true;
		}

		// Bradford adaptation of XYZ from white a to white b (xy chromaticities)
		void Bradford(const double* a, const double* b, double* out)
		{
			const double B[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };
			double Bi[9];
			Invert(B, Bi);
			const double wa[3] = { a[0] / a[1], 1, (1 - a[0] - a[1]) / a[1] }, wb[3] = { b[0] / b[1], 1, (1 - b[0] - b[1]) / b[1] };
			double ca[3], cb[3];
			for (int i = 0; i < 3; ++i)
				ca[i] = B[i * 3] * wa[0] + B[i * 3 + 1] * wa[1] + B[i * 3 + 2] * wa[2], cb[i] = B[i * 3] * wb[0] + B[i * 3 + 1] * wb[1] + B[i * 3 + 2] * wb[2];
			const double d[9] = { cb[0] / ca[0], 0, 0, 0, cb[1] / ca[1], 0, 0, 0, cb[2] / ca[2] };
			double t[9];
			Mul(d, B, t);
			Mul(Bi, t, out);
		}

		// false when the matrix is the identity to within what 8-bit output could show
		bool Store(const double* m, float out[9])
		{
			bool identity = true;
			for (int i = 0; i < 9; ++i)
			{
				out[i] = (float)m[i];
				identity &= std::fabs(m[i] - (i % 4 == 0 ? 1.0 : 0.0)) < 2e-3;
			}
			return !identity;
		}

		constexpr double kRec709[8] = { 0.64, 0.33, 0.30, 0.60, 0.15, 0.06, 0.3127, 0.3290 };
	}

	bool PrimariesToRec709(const float xy[8], float m[9])
	{
		double src[8], toXyz[9], fromXyz[9], adapt[9], t[9];
		for (int i = 0; i < 8; ++i)
			src[i] = xy[i];
		if (src[1] <= 0 || src[3] <= 0 || src[5] <= 0 || src[7] <= 0 || !RgbToXyz(src, toXyz) || !RgbToXyz(kRec709, t) || !Invert(t, fromXyz))
			return false;
		Bradford(src + 6, kRec709 + 6, adapt);
		Mul(adapt, toXyz, t);
		Mul(fromXyz, t, t);
		return Store(t, m);
	}

	bool IccToRec709(const uint8_t* icc, size_t size, float m[9])
	{
		if (size < 132 || memcmp(icc + 16, "RGB ", 4) != 0)
			return false;
		const uint32_t count = Be32(icc + 128);
		double src[9] = {};
		int found = 0;
		for (uint32_t i = 0; i < count && 132 + 12 * (size_t)(i + 1) <= size; ++i)
		{
			const uint8_t* tag = icc + 132 + 12 * i;
			const char* sig = (const char*)tag;
			const int col = memcmp(sig, "rXYZ", 4) == 0 ? 0 : memcmp(sig, "gXYZ", 4) == 0 ? 1 : memcmp(sig, "bXYZ", 4) == 0 ? 2 : -1;
			const uint32_t at = Be32(tag + 4);
			if (col < 0 || at + 20 > size || memcmp(icc + at, "XYZ ", 4) != 0)
				continue;
			for (int r = 0; r < 3; ++r)
				src[r * 3 + col] = (int32_t)Be32(icc + at + 8 + 4 * r) / 65536.0;
			found |= 1 << col;
		}
		if (found != 7)
			return false;
		// the colorants are D50-adapted (PCS); sRGB's D50-adapted colorants as in the ICC sRGB profiles
		const double srgbD50[9] = { 0.4360747, 0.3850649, 0.1430804, 0.2225045, 0.7168786, 0.0606169, 0.0139322, 0.0971045, 0.7141733 };
		double inv[9], t[9];
		if (!Invert(srgbD50, inv))
			return false;
		Mul(inv, src, t);
		return Store(t, m);
	}
}
