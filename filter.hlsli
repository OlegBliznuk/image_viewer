// shared by the display shader and the mip builder
float3 SrgbToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float3 LinearToSrgb(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// Mitchell-Netravali, B = C = 1/3, times 6 (the weights are normalised)
float Mitchell(float x)
{
	x = abs(x);
	return x < 1.0 ? (7.0 * x - 12.0) * x * x + 16.0 / 3.0 : x < 2.0 ? ((-7.0 / 3.0 * x + 12.0) * x - 20.0) * x + 32.0 / 3.0 : 0.0;
}

// Mitchell around c (texels of a mip level) stretched by s texels per output pixel, in linear light, clamped to the positive lobe's texels so HDR highlights cannot ring
float4 MitchellFilter(Texture2D<float4> t, uint mip, float2 c, float2 s, uint2 size, bool srgb)
{
	const float2 r = 2.0 * s;
	const int2 lo = max(int2(ceil(c - r - 0.5)), int2(0, 0));
	const int2 hi = min(int2(floor(c + r - 0.5)), int2(size) - 1);
	float4 sum = 0.0, vmin = 3.0e38, vmax = -3.0e38;
	float wsum = 0.0;
	[loop]
	for (int y = lo.y; y <= hi.y; ++y)
	{
		const float dy = (y + 0.5 - c.y) / s.y;
		const float wy = Mitchell(dy);
		[loop]
		for (int x = lo.x; x <= hi.x; ++x)
		{
			const float dx = (x + 0.5 - c.x) / s.x;
			const float w = wy * Mitchell(dx);
			float4 v = t.Load(int3(x, y, mip));
			if (srgb)
				v.rgb = SrgbToLinear(saturate(v.rgb));
			sum += w * v;
			wsum += w;
			if (abs(dx) < 1.0 && abs(dy) < 1.0)
			{
				vmin = min(vmin, v);
				vmax = max(vmax, v);
			}
		}
	}
	return clamp(sum / wsum, vmin, vmax);
}
