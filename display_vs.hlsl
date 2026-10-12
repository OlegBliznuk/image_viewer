#include "display.hlsli"

float4 main(uint id : SV_VertexID) : SV_Position
{
	return float4(lerp(ndcMin, ndcMax, float2(id & 1, id >> 1)), 0.0, 1.0);
}
