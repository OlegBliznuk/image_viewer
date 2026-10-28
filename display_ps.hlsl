#include "display.hlsli"
#include "filter.hlsli"

Texture2D<float4> tex : register(t0);

// AdjustConstants (adjust.h); the functions below are adjust.cpp's, line for line
cbuffer Adjust : register(b1)
{
	uint adjFlags;
	float gradeMix;
	float hsOverlap;
	float adjPad;
	float4 gradeA;
	float4 gradeB;
	float4 gradeGamma;
	float4 levelsIn[4];   // low, high, 1 / gamma: RGB, R, G, B
	float4 levelsOut[4];
	float4 balance[3];   // shadows, midtones, highlights: cyan-red, magenta-green, yellow-blue
	float4 hueSat[7];   // master, reds ... magentas: hue, saturation, lightness
	float4 curves[256];   // RGB, R, G, B
};

float3 AcesFitted(float3 c)
{
	c = float3(0.59719 * c.r + 0.35458 * c.g + 0.04823 * c.b, 0.07600 * c.r + 0.90834 * c.g + 0.01566 * c.b, 0.02840 * c.r + 0.13383 * c.g + 0.83777 * c.b);
	c = (c * (c + 0.0245786) - 0.000090537) / (c * (0.983729 * c + 0.4329510) + 0.238081);
	c = float3(1.60475 * c.r - 0.53108 * c.g - 0.07367 * c.b, -0.10208 * c.r + 1.10813 * c.g - 0.00605 * c.b, -0.00327 * c.r - 0.07276 * c.g + 1.07602 * c.b);
	return saturate(c);
}

// pow only runs on x > 0 (abs keeps the compiler's negative-base warning quiet)
float GradeChannel(float x, float A, float B, float G)
{
	const bool blackClamp = (adjFlags & 64) != 0, whiteClamp = (adjFlags & 128) != 0;
	if ((adjFlags & 32) == 0)
	{
		if (A != 1.0 || B != 0.0)
			x = x * A + B;
		if (blackClamp && whiteClamp)
			x = clamp(x, 0.0, 1.0);
		else if (blackClamp)
			x = max(x, 0.0);
		else if (whiteClamp)
			x = min(x, 1.0);
		// Grade.C's GPU path writes 9999 where the CPU writes infinity
		if (G <= 0.0)
			x = x < 0.0 ? 0.0 : (x > 1.0 ? 9999.0 : x);
		else if (G != 1.0 && x > 0.0)
			x = x < 1.0 ? pow(abs(x), 1.0 / G) : 1.0 + (x - 1.0) * (1.0 / G);
	}
	else
	{
		if (G <= 0.0)
			x = x > 0.0 ? 1.0 : 0.0;
		else if (G != 1.0 && x > 0.0)
			x = x < 1.0 ? pow(abs(x), G) : 1.0 + (x - 1.0) * G;
		if (A != 1.0 || B != 0.0)
		{
			const float ia = A != 0.0 ? 1.0 / A : 1.0;
			x = x * ia + -B * ia;
		}
		if (blackClamp)
			x = max(x, 0.0);
		else if (whiteClamp)
			x = min(x, 1.0);
	}
	return x;
}

float3 Grade(float3 c)
{
	const float3 g = float3(GradeChannel(c.r, gradeA.r, gradeB.r, gradeGamma.r), GradeChannel(c.g, gradeA.g, gradeB.g, gradeGamma.g), GradeChannel(c.b, gradeA.b, gradeB.b, gradeGamma.b));
	return gradeMix == 1.0 ? g : c + (g - c) * gradeMix;
}

float LevelsMap(float v, float4 i, float4 o)
{
	v = i.y != i.x ? (v - i.x) / (i.y - i.x) : v - i.x;
	if (i.z != 1.0 && v > 0.0)
		v = pow(abs(v), i.z);
	return v * (o.y - o.x) + o.x;
}

float CurveMap(float v, uint ch)
{
	float r = v >= 1.0 ? curves[255][ch] : curves[0][ch];
	if (v > 0.0 && v < 1.0)
	{
		const float x = v * 255.0;
		const uint i = (uint)x;
		r = curves[i][ch] + (curves[i + 1][ch] - curves[i][ch]) * (x - (float)i);
	}
	return r;
}

// babl's HSL
float3 RgbToHsl(float3 c)
{
	const float mx = max(c.r, max(c.g, c.b)), mn = min(c.r, min(c.g, c.b));
	const float l = (mx + mn) * 0.5;
	if (mx - mn < 1e-10)
		return float3(0.0, 0.0, l);
	const float d = mx - mn, sum = mx + mn;
	const float s = l > 0.5 ? d / (2.0 - sum) : d / sum;
	float h;
	if (mx - c.r < 1e-10)
		h = (c.g - c.b) / d + (c.g < c.b ? 6.0 : 0.0);
	else if (mx - c.g < 1e-10)
		h = (c.b - c.r) / d + 2.0;
	else
		h = (c.r - c.g) / d + 4.0;
	return float3(h / 6.0, s, l);
}

float HueToRgb(float p, float q, float t)
{
	if (t < 0.0)
		t += 1.0;
	if (t > 1.0)
		t -= 1.0;
	if (t < 1.0 / 6.0)
		return p + (q - p) * 6.0 * t;
	if (t < 0.5)
		return q;
	if (t < 2.0 / 3.0)
		return p + (q - p) * (2.0 / 3.0 - t) * 6.0;
	return p;
}

float3 HslToRgb(float3 hsl)
{
	if (hsl.y < 1e-7)
		return hsl.zzz;
	const float l = hsl.z, s = hsl.y;
	const float q = l < 0.5 ? l * (1.0 + s) : l + s - l * s, p = 2.0 * l - q;
	float h = fmod(hsl.x, 1.0);
	if (h < 0.0)
		h += 1.0;
	return float3(HueToRgb(p, q, h + 1.0 / 3.0), HueToRgb(p, q, h), HueToRgb(p, q, h - 1.0 / 3.0));
}

float BalanceMap(float v, float l, float shadows, float midtones, float highlights)
{
	const float a = 0.25, b = 0.333, scale = 0.7;
	shadows *= saturate((l - b) / -a + 0.5) * scale;
	midtones *= saturate((l - b) / a + 0.5) * saturate((l + b - 1.0) / -a + 0.5) * scale;
	highlights *= saturate((l + b - 1.0) / a + 0.5) * scale;
	v += shadows;
	v += midtones;
	v += highlights;
	return saturate(v);
}

float WrapHue(float v) { return v < 0.0 ? v + 1.0 : (v > 1.0 ? v - 1.0 : v); }
float MapSaturation(uint r, float v) { return saturate(v * (hueSat[0].y + hueSat[r].y + 1.0)); }
float MapLightness(float v, float amount) { return amount < 0.0 ? v * (amount + 1.0) : v + amount * (1.0 - v); }

float3 HueSaturation(float3 c)
{
	float3 hsl = RgbToHsl(c);
	const float overlap = hsOverlap / 2.0, h = hsl.x * 6.0;
	uint hue = 0, secondary = 0;
	bool useSecondary = false;
	float pInt = 1.0, sInt = 0.0;
	for (uint k = 0; k < 7; ++k)
	{
		const float t = (float)k + 0.5;
		if (h < t + overlap)
		{
			hue = k;
			if (overlap > 0.0 && h > t - overlap)
			{
				useSecondary = true;
				secondary = k + 1;
				sInt = (h - t + overlap) / (2.0 * overlap);
				pInt = 1.0 - sInt;
			}
			break;
		}
	}
	if (hue >= 6)
	{
		hue = 0;
		useSecondary = false;
	}
	if (secondary >= 6)
		secondary = 0;
	hue++;
	secondary++;
	if (useSecondary)
	{
		const float H = WrapHue(hsl.x + (hueSat[0].x + hueSat[hue].x * pInt + hueSat[secondary].x * sInt) / 2.0);
		const float S = MapSaturation(hue, hsl.y) * pInt + MapSaturation(secondary, hsl.y) * sInt;
		const float L = MapLightness(hsl.z, hueSat[0].z + hueSat[hue].z) * pInt + MapLightness(hsl.z, hueSat[0].z + hueSat[secondary].z) * sInt;
		hsl = float3(H, S, L);
	}
	else if (hsl.y <= 0.0)
		hsl.z = MapLightness(hsl.z, hueSat[0].z);
	else
		hsl = float3(WrapHue(hsl.x + (hueSat[0].x + hueSat[hue].x) / 2.0), MapSaturation(hue, hsl.y), MapLightness(hsl.z, hueSat[0].z + hueSat[hue].z));
	return HslToRgb(hsl);
}

float3 DisplayAdjust(float3 c)
{
	if (adjFlags & 2)
	{
		c = float3(LevelsMap(c.r, levelsIn[1], levelsOut[1]), LevelsMap(c.g, levelsIn[2], levelsOut[2]), LevelsMap(c.b, levelsIn[3], levelsOut[3]));
		c = saturate(float3(LevelsMap(c.r, levelsIn[0], levelsOut[0]), LevelsMap(c.g, levelsIn[0], levelsOut[0]), LevelsMap(c.b, levelsIn[0], levelsOut[0])));
	}
	if (adjFlags & 4)
	{
		c = float3((adjFlags & 1024) ? CurveMap(c.r, 1) : saturate(c.r), (adjFlags & 2048) ? CurveMap(c.g, 2) : saturate(c.g), (adjFlags & 4096) ? CurveMap(c.b, 3) : saturate(c.b));
		if (adjFlags & 512)
			c = float3(CurveMap(c.r, 0), CurveMap(c.g, 0), CurveMap(c.b, 0));
	}
	if (adjFlags & 8)
	{
		const float l = RgbToHsl(c).z;
		c = float3(BalanceMap(c.r, l, balance[0].x, balance[1].x, balance[2].x), BalanceMap(c.g, l, balance[0].y, balance[1].y, balance[2].y), BalanceMap(c.b, l, balance[0].z, balance[1].z, balance[2].z));
		if (adjFlags & 256)
		{
			const float3 hsl = RgbToHsl(c);
			c = HslToRgb(float3(hsl.xy, l));
		}
	}
	if (adjFlags & 16)
		c = saturate(HueSaturation(c));
	return c;
}

float4 main(float4 pos : SV_Position) : SV_Target
{
	const float2 uv = (pos.xy - imgOrigin) / imgSize;
	const bool srgb = (flags & 1) != 0;
	float4 c;
	if (imgSize.x < texSize.x)
	{
		// zoomed out: the finest mip level with at most 2x reduction left, Mitchell-filtered by what is left
		const uint level = min((uint)log2(texSize.x / imgSize.x), maxLevel);
		uint w, h, n;
		tex.GetDimensions(level, w, h, n);
		const float2 size = float2(w, h);
		c = MitchellFilter(tex, level, uv * size, clamp(size / imgSize, 1.0, 2.0), uint2(w, h), srgb);
	}
	else
	{
		c = tex.Load(int3(clamp(int2(floor(uv * texSize)), int2(0, 0), int2(texSize) - 1), 0));
		if (srgb)
			c.rgb = SrgbToLinear(saturate(c.rgb));
	}
	if (flags & 32)
		c.rgb = float3(dot(m0.xyz, c.rgb), dot(m1.xyz, c.rgb), dot(m2.xyz, c.rgb));
	float3 v = (flags & 8) ? dot(c, mask).xxx : c.rgb;
	if (adjFlags & 1)
		v = Grade(v);
	v *= exposure;
	if (flags & 2)
		v = AcesFitted(v);
	v = (flags & 16) ? saturate(v) : LinearToSrgb(saturate(v));
	if (adjFlags & 30)
		v = DisplayAdjust(v);
	return float4(v, 1.0);
}
