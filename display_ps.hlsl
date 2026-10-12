#include "display.hlsli"
#include "filter.hlsli"

Texture2D<float4> tex : register(t0);

float3 AcesFitted(float3 c)
{
	c = float3(0.59719 * c.r + 0.35458 * c.g + 0.04823 * c.b, 0.07600 * c.r + 0.90834 * c.g + 0.01566 * c.b, 0.02840 * c.r + 0.13383 * c.g + 0.83777 * c.b);
	c = (c * (c + 0.0245786) - 0.000090537) / (c * (0.983729 * c + 0.4329510) + 0.238081);
	c = float3(1.60475 * c.r - 0.53108 * c.g - 0.07367 * c.b, -0.10208 * c.r + 1.10813 * c.g - 0.00605 * c.b, -0.00327 * c.r - 0.07276 * c.g + 1.07602 * c.b);
	return saturate(c);
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
	v *= exposure;
	if (flags & 2)
		v = AcesFitted(v);
	return float4((flags & 16) ? saturate(v) : LinearToSrgb(saturate(v)), 1.0);
}
