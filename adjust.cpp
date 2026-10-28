#include "adjust.h"

#include <algorithm>
#include <cmath>

namespace iv
{
	namespace
	{
		constexpr int kCurveSamples = 256;

		float Clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

		// GIMP's ROUND for the non-negative values here
		int Round(double x) { return (int)(x + 0.5); }

		// babl's HSL (extensions/HSL.c) on 0-1 values
		void RgbToHsl(const float* c, float& h, float& s, float& l)
		{
			const float mx = std::max({ c[0], c[1], c[2] }), mn = std::min({ c[0], c[1], c[2] });
			l = (mx + mn) * 0.5f;
			if (mx - mn < 1e-10f)
			{
				h = s = 0.0f;
				return;
			}
			const float d = mx - mn, sum = mx + mn;
			s = l > 0.5f ? d / (2.0f - sum) : d / sum;
			if (mx - c[0] < 1e-10f)
				h = (c[1] - c[2]) / d + (c[1] < c[2] ? 6.0f : 0.0f);
			else if (mx - c[1] < 1e-10f)
				h = (c[2] - c[0]) / d + 2.0f;
			else
				h = (c[0] - c[1]) / d + 4.0f;
			h /= 6.0f;
		}

		float HueToRgb(float p, float q, float t)
		{
			if (t < 0.0f)
				t += 1.0f;
			if (t > 1.0f)
				t -= 1.0f;
			if (t < 1.0f / 6.0f)
				return p + (q - p) * 6.0f * t;
			if (t < 0.5f)
				return q;
			if (t < 2.0f / 3.0f)
				return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
			return p;
		}

		void HslToRgb(float h, float s, float l, float* c)
		{
			if (s < 1e-7f)
			{
				c[0] = c[1] = c[2] = l;
				return;
			}
			const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s, p = 2.0f * l - q;
			h = std::fmod(h, 1.0f);
			if (h < 0.0f)
				h += 1.0f;
			c[0] = HueToRgb(p, q, h + 1.0f / 3.0f);
			c[1] = HueToRgb(p, q, h);
			c[2] = HueToRgb(p, q, h - 1.0f / 3.0f);
		}

		// Grade.C per channel: forward x = A x + B, clamps, gamma (negatives untouched, the tangent line above 1); reverse in the opposite order
		float GradeChannel(const AdjustState& s, int z, float x)
		{
			const float A = s.gradeA[z], B = s.gradeB[z], G = s.gradeGamma[z];
			if (!s.gradeReverse)
			{
				if (A != 1.0f || B != 0.0f)
					x = x * A + B;
				if (s.blackClamp && s.whiteClamp)
					x = std::clamp(x, 0.0f, 1.0f);
				else if (s.blackClamp)
					x = std::max(x, 0.0f);
				else if (s.whiteClamp)
					x = std::min(x, 1.0f);
				if (G <= 0.0f)
					x = x < 0.0f ? 0.0f : x > 1.0f ? INFINITY : x;
				else if (G != 1.0f && x > 0.0f)
					x = x < 1.0f ? std::pow(x, 1.0f / G) : 1.0f + (x - 1.0f) * (1.0f / G);
				return x;
			}
			if (G <= 0.0f)
				x = x > 0.0f ? 1.0f : 0.0f;
			else if (G != 1.0f && x > 0.0f)
				x = x < 1.0f ? std::pow(x, G) : 1.0f + (x - 1.0f) * G;
			if (A != 1.0f || B != 0.0f)
			{
				const float ia = A != 0.0f ? 1.0f / A : 1.0f;
				x = x * ia + -B * ia;
			}
			// both clamps on: Grade.C's reverse applies only the black one
			if (s.blackClamp)
				x = std::max(x, 0.0f);
			else if (s.whiteClamp)
				x = std::min(x, 1.0f);
			return x;
		}

		float LevelsMap(float v, float low, float high, float invGamma, float outLow, float outHigh)
		{
			v = high != low ? (v - low) / (high - low) : v - low;
			if (invGamma != 1.0f && v > 0.0f)
				v = std::pow(v, invGamma);
			return v * (outHigh - outLow) + outLow;
		}

		float CurveMap(const float* s, float v)
		{
			if (v > 0.0f && v < 1.0f)
			{
				const float x = v * (kCurveSamples - 1);
				const int i = (int)x;
				return s[i] + (s[i + 1] - s[i]) * (x - (float)i);
			}
			return v >= 1.0f ? s[kCurveSamples - 1] : s[0];
		}

		// the three masks sum to 1 over 0..1: ramps of width 0.25 at 0.333 and 0.667
		float BalanceMap(float v, float l, float shadows, float midtones, float highlights)
		{
			constexpr float a = 0.25f, b = 0.333f, scale = 0.7f;
			shadows *= Clamp01((l - b) / -a + 0.5f) * scale;
			midtones *= Clamp01((l - b) / a + 0.5f) * Clamp01((l + b - 1.0f) / -a + 0.5f) * scale;
			highlights *= Clamp01((l + b - 1.0f) / a + 0.5f) * scale;
			v += shadows;
			v += midtones;
			v += highlights;
			return Clamp01(v);
		}

		float WrapHue(float v) { return v < 0.0f ? v + 1.0f : v > 1.0f ? v - 1.0f : v; }
		float MapSaturation(const AdjustState& s, int r, float v) { return Clamp01(v * (s.saturation[0] + s.saturation[r] + 1.0f)); }
		float MapLightness(float v, float amount) { return amount < 0.0f ? v * (amount + 1.0f) : v + amount * (1.0f - v); }

		void HueSaturation(const AdjustState& s, float* c)
		{
			float H, S, L;
			RgbToHsl(c, H, S, L);
			const float overlap = s.overlap / 2.0f, h = H * 6.0f;
			int hue = 0, secondary = 0;
			bool useSecondary = false;
			float pInt = 1.0f, sInt = 0.0f;
			for (int k = 0; k < 7; ++k)
			{
				const float t = (float)k + 0.5f;
				if (h < t + overlap)
				{
					hue = k;
					if (overlap > 0.0f && h > t - overlap)
					{
						useSecondary = true;
						secondary = k + 1;
						sInt = (h - t + overlap) / (2.0f * overlap);
						pInt = 1.0f - sInt;
					}
					break;
				}
			}
			if (hue >= 6)
				hue = 0, useSecondary = false;
			if (secondary >= 6)
				secondary = 0;
			// range 0 is the master
			hue++, secondary++;
			if (useSecondary)
			{
				H = WrapHue(H + (s.hue[0] + s.hue[hue] * pInt + s.hue[secondary] * sInt) / 2.0f);
				S = MapSaturation(s, hue, S) * pInt + MapSaturation(s, secondary, S) * sInt;
				L = MapLightness(L, s.lightness[0] + s.lightness[hue]) * pInt + MapLightness(L, s.lightness[0] + s.lightness[secondary]) * sInt;
			}
			else if (S <= 0.0f)
				L = MapLightness(L, s.lightness[0]);
			else
			{
				H = WrapHue(H + (s.hue[0] + s.hue[hue]) / 2.0f);
				L = MapLightness(L, s.lightness[0] + s.lightness[hue]);
				S = MapSaturation(s, hue, S);
			}
			HslToRgb(H, S, L, c);
		}

		void PlotSegment(const std::vector<CurvePoint>& p, int p1, int p2, int p3, int p4, float* out)
		{
			const double x0 = p[p2].x, y0 = p[p2].y, x3 = p[p3].x, y3 = p[p3].y, dx = x3 - x0, dy = y3 - y0;
			if (dx <= 1e-6)
			{
				out[Round(x0 * (kCurveSamples - 1))] = (float)y3;
				return;
			}
			double y1, y2;
			if (p1 == p2 && p3 == p4)
				y1 = y0 + dy / 3.0, y2 = y0 + dy * 2.0 / 3.0;
			else if (p1 == p2)
			{
				const double slope = (p[p4].y - y0) / (p[p4].x - x0);
				y2 = y3 - slope * dx / 3.0;
				y1 = y0 + (y2 - y0) / 2.0;
			}
			else if (p3 == p4)
			{
				const double slope = (y3 - p[p1].y) / (x3 - p[p1].x);
				y1 = y0 + slope * dx / 3.0;
				y2 = y3 + (y1 - y3) / 2.0;
			}
			else
			{
				y1 = y0 + (y3 - p[p1].y) / (x3 - p[p1].x) * dx / 3.0;
				y2 = y3 - (p[p4].y - y0) / (p[p4].x - x0) * dx / 3.0;
			}
			const int steps = Round(dx * (kCurveSamples - 1)), base = Round(x0 * (kCurveSamples - 1));
			for (int i = 0; i <= steps; ++i)
			{
				const double t = i / dx / (kCurveSamples - 1), u = 1.0 - t;
				const double y = y0 * u * u * u + 3.0 * y1 * u * u * t + 3.0 * y2 * u * t * t + y3 * t * t * t;
				if (i + base < kCurveSamples)
					out[i + base] = (float)std::clamp(y, 0.0, 1.0);
			}
		}

		bool IsIdentityCurve(const std::vector<CurvePoint>& p)
		{
			return p.size() == 2 && p[0] == CurvePoint{ 0.0f, 0.0f } && p[1] == CurvePoint{ 1.0f, 1.0f };
		}
	}

	void CurveSamples(const std::vector<CurvePoint>& p, float out[256])
	{
		for (int i = 0; i < kCurveSamples; ++i)
			out[i] = (float)i / (kCurveSamples - 1);
		const int n = (int)p.size();
		if (n == 0)
			return;
		for (int i = 0; i < Round(p[0].x * (kCurveSamples - 1)); ++i)
			out[i] = p[0].y;
		for (int i = Round(p[n - 1].x * (kCurveSamples - 1)); i < kCurveSamples; ++i)
			out[i] = p[n - 1].y;
		for (int i = 0; i + 1 < n; ++i)
		{
			int p1 = std::max(i - 1, 0), p4 = std::min(i + 2, n - 1);
			if (p[i].corner)
				p1 = i;
			if (p[i + 1].corner)
				p4 = i + 1;
			PlotSegment(p, p1, i, i + 1, p4, out);
		}
		for (const CurvePoint& q : p)
			out[Round(q.x * (kCurveSamples - 1))] = q.y;
	}

	AdjustState CompileAdjust(const ColorAdjust& a)
	{
		AdjustState s;
		GradeParams untouched;
		untouched.enabled = a.grade.enabled;
		if (a.grade.enabled && !(a.grade == untouched))
		{
			const GradeParams& g = a.grade;
			// Grade.C's _validate: the clamps alone already change the image
			bool change = g.blackClamp || g.whiteClamp;
			for (int z = 0; z < 3; ++z)
			{
				float A = g.whitepoint[z] - g.blackpoint[z];
				A = A != 0.0f ? (g.gain[z] - g.lift[z]) / A : 10000.0f;
				A *= g.multiply[z];
				const float B = g.offset[z] + g.lift[z] - g.blackpoint[z] * A;
				s.gradeA[z] = A, s.gradeB[z] = B, s.gradeGamma[z] = g.gamma[z];
				change |= A != 1.0f || B != 0.0f || g.gamma[z] != 1.0f;
			}
			s.grade = change && g.mix != 0.0f;
			s.gradeReverse = g.reverse, s.blackClamp = g.blackClamp, s.whiteClamp = g.whiteClamp, s.gradeMix = g.mix;
		}
		LevelsParams levelsDefault;
		levelsDefault.enabled = a.levels.enabled;
		if (a.levels.enabled && !(a.levels == levelsDefault))
		{
			s.levels = true;
			for (int c = 0; c < 4; ++c)
			{
				const LevelsChannel& l = a.levels.ch[c];
				s.lvInLow[c] = l.inLow, s.lvInHigh[c] = l.inHigh, s.lvOutLow[c] = l.outLow, s.lvOutHigh[c] = l.outHigh;
				s.lvInvGamma[c] = l.gamma > 0.0f ? 1.0f / l.gamma : 1.0f;
			}
		}
		if (a.curves.enabled)
			for (int c = 0; c < 4; ++c)
				if (!IsIdentityCurve(a.curves.points[c]))
				{
					s.curveActive[c] = s.curves = true;
					CurveSamples(a.curves.points[c], s.curve[c]);
				}
		const BalanceParams& b = a.balance;
		for (int r = 0; r < 3 && b.enabled; ++r)
		{
			s.balanceAmount[r][0] = b.cyanRed[r], s.balanceAmount[r][1] = b.magentaGreen[r], s.balanceAmount[r][2] = b.yellowBlue[r];
			s.balance |= b.cyanRed[r] != 0.0f || b.magentaGreen[r] != 0.0f || b.yellowBlue[r] != 0.0f;
		}
		s.preserveLuminosity = b.preserveLuminosity;
		const HueSatParams& h = a.hueSat;
		for (int r = 0; r < 7 && h.enabled; ++r)
		{
			s.hue[r] = h.hue[r], s.saturation[r] = h.saturation[r], s.lightness[r] = h.lightness[r];
			s.hueSat |= h.hue[r] != 0.0f || h.saturation[r] != 0.0f || h.lightness[r] != 0.0f;
		}
		s.overlap = h.overlap;
		return s;
	}

	void ApplyGrade(const AdjustState& s, float* rgb)
	{
		if (!s.grade)
			return;
		for (int z = 0; z < 3; ++z)
		{
			const float x = GradeChannel(s, z, rgb[z]);
			rgb[z] = s.gradeMix == 1.0f ? x : rgb[z] + (x - rgb[z]) * s.gradeMix;
		}
	}

	void ApplyLevels(const AdjustState& s, float* c)
	{
		if (s.levels)
			for (int k = 0; k < 3; ++k)
			{
				const float v = LevelsMap(c[k], s.lvInLow[k + 1], s.lvInHigh[k + 1], s.lvInvGamma[k + 1], s.lvOutLow[k + 1], s.lvOutHigh[k + 1]);
				c[k] = Clamp01(LevelsMap(v, s.lvInLow[0], s.lvInHigh[0], s.lvInvGamma[0], s.lvOutLow[0], s.lvOutHigh[0]));
			}
	}

	void ApplyDisplayAdjust(const AdjustState& s, float* c)
	{
		ApplyLevels(s, c);
		if (s.curves)
			for (int k = 0; k < 3; ++k)
			{
				float v = s.curveActive[k + 1] ? CurveMap(s.curve[k + 1], c[k]) : Clamp01(c[k]);
				c[k] = s.curveActive[0] ? CurveMap(s.curve[0], v) : v;
			}
		if (s.balance)
		{
			float h, sat, l;
			RgbToHsl(c, h, sat, l);
			for (int k = 0; k < 3; ++k)
				c[k] = BalanceMap(c[k], l, s.balanceAmount[0][k], s.balanceAmount[1][k], s.balanceAmount[2][k]);
			if (s.preserveLuminosity)
			{
				float l2;
				RgbToHsl(c, h, sat, l2);
				HslToRgb(h, sat, l, c);
			}
		}
		if (s.hueSat)
		{
			HueSaturation(s, c);
			for (int k = 0; k < 3; ++k)
				c[k] = Clamp01(c[k]);
		}
	}

	void PackAdjust(const AdjustState& s, AdjustConstants& o)
	{
		o = {};
		o.flags = (s.grade ? kAdjGrade : 0u) | (s.levels ? kAdjLevels : 0u) | (s.curves ? kAdjCurves : 0u) | (s.balance ? kAdjBalance : 0u) | (s.hueSat ? kAdjHueSat : 0u);
		o.flags |= (s.gradeReverse ? kAdjReverse : 0u) | (s.blackClamp ? kAdjBlackClamp : 0u) | (s.whiteClamp ? kAdjWhiteClamp : 0u) | (s.preserveLuminosity ? kAdjPreserveLum : 0u);
		o.flags |= (s.curveActive[0] ? kAdjCurveRgb : 0u) | (s.curveActive[1] ? kAdjCurveR : 0u) | (s.curveActive[2] ? kAdjCurveG : 0u) | (s.curveActive[3] ? kAdjCurveB : 0u);
		o.gradeMix = s.gradeMix;
		o.overlap = s.overlap;
		for (int z = 0; z < 3; ++z)
			o.gradeA[z] = s.gradeA[z], o.gradeB[z] = s.gradeB[z], o.gradeGamma[z] = s.gradeGamma[z];
		for (int c = 0; c < 4; ++c)
		{
			o.levelsIn[c][0] = s.lvInLow[c], o.levelsIn[c][1] = s.lvInHigh[c], o.levelsIn[c][2] = s.lvInvGamma[c];
			o.levelsOut[c][0] = s.lvOutLow[c], o.levelsOut[c][1] = s.lvOutHigh[c];
		}
		for (int r = 0; r < 3; ++r)
			for (int k = 0; k < 3; ++k)
				o.balance[r][k] = s.balanceAmount[r][k];
		for (int r = 0; r < 7; ++r)
			o.hueSat[r][0] = s.hue[r], o.hueSat[r][1] = s.saturation[r], o.hueSat[r][2] = s.lightness[r];
		for (int i = 0; i < kCurveSamples; ++i)
			for (int c = 0; c < 4; ++c)
				o.curves[i][c] = s.curveActive[c] ? s.curve[c][i] : (float)i / (kCurveSamples - 1);
	}
}
