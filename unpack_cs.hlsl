// pixels the frame cache holds packed (read straight from it) to mip 0 of the RGBA texture: Radiance RGBE (2^(e - 136) from the exponent bits, as the CPU did),
// RGB of 8 / 16-bit integers, halves or floats with alpha 1, or YCbCr planes behind a YuvHeader (yuv.h) converted as yuv.cpp's YuvReader does
cbuffer Params : register(b0)
{
	uint width;   // mode 5: of the stored planes (threads walk them row by row and write the oriented texel)
	uint height;
	uint pitch;   // bytes per row of the source; mode 5: the frame's last byte offset
	uint mode;   // 0 RGBE, 1 RGB 8-bit, 2 RGB 16-bit, 3 RGB half, 4 RGB float, 5 YCbCr planes
};

ByteAddressBuffer src : register(t0);
RWTexture2D<float4> dst : register(u0);

// 4 bytes from any byte offset (the cache buffers carry slack past the last row)
uint Bytes4(uint offset)
{
	const uint s = (offset & 3u) * 8;
	const uint2 w = src.Load2(offset & ~3u);
	return s ? (w.x >> s) | (w.y << (32 - s)) : w.x;
}

// a YuvHeader may come from a slot whose decode failed: every load stays inside the frame
int Byte(uint offset)
{
	offset = min(offset, pitch);
	return int((src.Load(offset & ~3u) >> ((offset & 3u) * 8)) & 255u);
}

struct Yuv
{
	uint width, height, yOffset, yPitch, cbOffset, crOffset, cPitch, cWidth, cHeight, sx, sy, orientation, planes, mode, fancy;
	int4 k;
};

Yuv Header()
{
	const uint4 a = src.Load4(0), b = src.Load4(16), c = src.Load4(32), d = src.Load4(48);
	Yuv h;
	h.width = a.x, h.height = a.y, h.yOffset = a.z, h.yPitch = a.w;
	h.cbOffset = b.x, h.crOffset = b.y, h.cPitch = b.z, h.cWidth = max(b.w, 1u);
	h.cHeight = max(c.x, 1u), h.sx = min(c.y, 2u), h.sy = min(c.z, 2u), h.orientation = c.w;
	h.planes = d.x, h.mode = d.y, h.fancy = d.z;
	h.k = asint(src.Load4(64));
	return h;
}

// libjpeg-turbo's jdsample.c: h2v1 (3c + n + 1|2) >> 2, h1v2 the same down the column, h2v2 (3 colsum + colsum' + 8|7) >> 4 with colsum = 3 near row + far row
int Chroma(Yuv h, uint plane, int c, int r)
{
	const int cw = int(h.cWidth) - 1, ch = int(h.cHeight) - 1;
	if (h.fancy == 0)
		return Byte(plane + uint(r >> h.sy) * h.cPitch + uint(c >> h.sx));
	if (h.sy == 0)
	{
		const uint row = plane + uint(r) * h.cPitch;
		const int cc = c >> 1, v = 3 * Byte(row + cc);
		return (c & 1) ? (v + Byte(row + min(cc + 1, cw)) + 2) >> 2 : (v + Byte(row + max(cc - 1, 0)) + 1) >> 2;
	}
	const int nr = r >> 1, fr = (r & 1) ? min(nr + 1, ch) : max(nr - 1, 0);
	const uint n = plane + uint(nr) * h.cPitch, f = plane + uint(fr) * h.cPitch;
	if (h.sx == 0)
		return (3 * Byte(n + c) + Byte(f + c) + ((r & 1) ? 2 : 1)) >> 2;
	const int cc = c >> 1, side = (c & 1) ? min(cc + 1, cw) : max(cc - 1, 0);
	return (3 * (3 * Byte(n + cc) + Byte(f + cc)) + 3 * Byte(n + side) + Byte(f + side) + ((c & 1) ? 7 : 8)) >> 4;
}

// stored pixel (c, r) to its texel: the inverse of YuvReader::Pixel's EXIF mapping
uint2 Oriented(Yuv h, int c, int r)
{
	const int W = int(h.width), H = int(h.height);
	switch (h.orientation)
	{
	case 2: return uint2(W - 1 - c, r);
	case 3: return uint2(W - 1 - c, H - 1 - r);
	case 4: return uint2(c, H - 1 - r);
	case 5: return uint2(r, c);
	case 6: return uint2(H - 1 - r, c);
	case 7: return uint2(H - 1 - r, W - 1 - c);
	case 8: return uint2(r, W - 1 - c);
	default: return uint2(c, r);
	}
}

float3 YuvPixel(Yuv h, int c, int r)
{
	const int Y = Byte(h.yOffset + uint(r) * h.yPitch + uint(c));
	int3 rgb = Y;
	if (h.planes == 3 && h.mode == 1)
	{
		// libheif's Op_YCbCr420_to_RGB32: nearest chroma, 8.8 coefficients
		const uint o = uint(r >> h.sy) * h.cPitch + uint(c >> h.sx);
		const int cb = Byte(h.cbOffset + o) - 128, cr = Byte(h.crOffset + o) - 128;
		rgb = Y + int3((h.k.x * cr + 128) >> 8, (h.k.y * cb + h.k.z * cr + 128) >> 8, (h.k.w * cb + 128) >> 8);
	}
	else if (h.planes == 3)
	{
		// jdcolor.c: FIX(1.40200), FIX(0.34414), FIX(0.71414), FIX(1.77200) at 16 fraction bits, ONE_HALF folded in
		const int cb = Chroma(h, h.cbOffset, c, r) - 128, cr = Chroma(h, h.crOffset, c, r) - 128;
		rgb = Y + int3((91881 * cr + 32768) >> 16, (-22554 * cb - 46802 * cr + 32768) >> 16, (116130 * cb + 32768) >> 16);
	}
	return float3(clamp(rgb, 0, 255)) / 255.0;
}

// rows of 64: a warp reads 32 consecutive bytes of each plane (8 x 8 groups fetched whole sectors of CPU memory for 8 bytes)
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	if (any(id.xy >= uint2(width, height)))
		return;
	if (mode == 5)
	{
		const Yuv h = Header();
		if (all(id.xy < uint2(h.width, h.height)))
			dst[Oriented(h, int(id.x), int(id.y))] = float4(YuvPixel(h, int(id.x), int(id.y)), 1.0);
		return;
	}
	const uint row = id.y * pitch;
	float4 c = float4(0.0, 0.0, 0.0, 1.0);
	if (mode == 0)
	{
		const uint v = src.Load(row + id.x * 4);
		const uint e = v >> 24;
		const float s = e > 9 ? asfloat((e - 9) << 23) : 0.0;
		c.rgb = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) * s;
	}
	else if (mode == 1)
	{
		const uint v = Bytes4(row + id.x * 3);
		c.rgb = float3(v & 255, (v >> 8) & 255, (v >> 16) & 255) / 255.0;
	}
	else if (mode == 4)
		c.rgb = asfloat(src.Load3(row + id.x * 12));
	else
	{
		const uint o = row + id.x * 6, lo = Bytes4(o), hi = Bytes4(o + 4);
		const uint3 v = uint3(lo & 0xffff, lo >> 16, hi & 0xffff);
		c.rgb = mode == 2 ? float3(v) / 65535.0 : f16tof32(v);
	}
	dst[id.xy] = c;
}
