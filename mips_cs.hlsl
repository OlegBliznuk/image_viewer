#include "filter.hlsli"

cbuffer Params : register(b0)
{
	uint2 srcSize;
	uint2 dstSize;
	uint srgb;
};

Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);

#define G 8
#define T 24

groupshared float4 tile[T * T];
groupshared float4 rowSum[T * G];
groupshared float4 rowMin[T * G];
groupshared float4 rowMax[T * G];

float4 Fetch(int2 p)
{
	float4 v = src.Load(int3(clamp(p, int2(0, 0), int2(srcSize) - 1), 0));
	if (srgb != 0)
		v.rgb = SrgbToLinear(saturate(v.rgb));
	return v;
}

void Store(uint2 p, float4 c)
{
	if (srgb != 0)
		c.rgb = LinearToSrgb(saturate(c.rgb));
	dst[p] = c;
}

// one mip level from the level above at the exact size ratio: MitchellFilter made separable through groupshared memory, each texel loaded and decoded once per group
[numthreads(G, G, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
	const float2 s = float2(srcSize) / float2(dstSize);
	const float2 r = 2.0 * s;
	// the footprint of G outputs, (G - 1) s + 2 r + 1 texels, outgrows the tile only on tiny odd levels
	if (max(s.x, s.y) > 2.09)
	{
		if (all(id.xy < dstSize))
			Store(id.xy, MitchellFilter(src, 0, (float2(id.xy) + 0.5) * s, s, srcSize, srgb != 0));
		return;
	}
	const int2 origin = int2(ceil((float2(gid.xy * G) + 0.5) * s - r - 0.5));
	for (uint i = tid.y * G + tid.x; i < T * T; i += G * G)
		tile[i] = Fetch(origin + int2(i % T, i / T));
	GroupMemoryBarrierWithGroupSync();

	// horizontal: this thread's output column over every tile row
	const float cx = (gid.x * G + tid.x + 0.5) * s.x;
	const int x0 = max((int)ceil(cx - r.x - 0.5), 0), x1 = min((int)floor(cx + r.x - 0.5), (int)srcSize.x - 1);
	for (uint row = tid.y; row < T; row += G)
	{
		float4 sum = 0.0, vmin = 3.0e38, vmax = -3.0e38;
		float wsum = 0.0;
		for (int x = x0; x <= x1; ++x)
		{
			const float d = (x + 0.5 - cx) / s.x;
			const float w = Mitchell(d);
			const float4 v = tile[row * T + (x - origin.x)];
			sum += w * v;
			wsum += w;
			if (abs(d) < 1.0)
			{
				vmin = min(vmin, v);
				vmax = max(vmax, v);
			}
		}
		rowSum[row * G + tid.x] = sum / wsum;
		rowMin[row * G + tid.x] = vmin;
		rowMax[row * G + tid.x] = vmax;
	}
	GroupMemoryBarrierWithGroupSync();

	// vertical
	if (any(id.xy >= dstSize))
		return;
	const float cy = (id.y + 0.5) * s.y;
	const int y0 = max((int)ceil(cy - r.y - 0.5), 0), y1 = min((int)floor(cy + r.y - 0.5), (int)srcSize.y - 1);
	float4 sum = 0.0, vmin = 3.0e38, vmax = -3.0e38;
	float wsum = 0.0;
	for (int y = y0; y <= y1; ++y)
	{
		const float d = (y + 0.5 - cy) / s.y;
		const float w = Mitchell(d);
		const uint k = (y - origin.y) * G + tid.x;
		sum += w * rowSum[k];
		wsum += w;
		if (abs(d) < 1.0)
		{
			vmin = min(vmin, rowMin[k]);
			vmax = max(vmax, rowMax[k]);
		}
	}
	Store(id.xy, clamp(sum / wsum, vmin, vmax));
}
