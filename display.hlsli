cbuffer Params : register(b0)
{
	float2 ndcMin;
	float2 ndcMax;
	float2 imgOrigin;
	float2 imgSize;
	float2 texSize;
	float exposure;
	uint flags;
	float4 mask;
	uint maxLevel;
	float4 m0;   // linear RGB to Rec.709 primaries, rows
	float4 m1;
	float4 m2;
};
