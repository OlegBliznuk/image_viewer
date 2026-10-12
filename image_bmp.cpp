#include "image_formats.h"

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <cstring>

// BMP: core / info / V2-V5 headers; 1, 4, 8 bpp palette (incl. RLE4, RLE8), 16 / 24 / 32 bpp (BI_RGB, BITFIELDS)
namespace iv::fmt
{
	namespace
	{
		struct Channel
		{
			uint32_t mask = 0;
			int shift = 0;
			uint32_t max = 0;

			void Set(uint32_t m)
			{
				mask = m;
				shift = m ? std::countr_zero(m) : 0;
				max = m ? (m >> shift) : 0;
			}
			uint8_t Get(uint32_t px, uint8_t fallback) const
			{
				if (!mask)
					return fallback;
				const uint32_t v = (px & mask) >> shift;
				return max == 255 ? (uint8_t)v : (uint8_t)((v * 255 + max / 2) / max);
			}
		};

		struct Info
		{
			int w = 0, h = 0, bpp = 0;
			bool topDown = false;
			uint32_t compression = 0, dataOffset = 0;
			Channel ch[4];
			std::vector<uint8_t> palette;
		};

		std::string Parse(const uint8_t* d, size_t size, Info& bi)
		{
			if (size < 26 || d[0] != 'B' || d[1] != 'M')
				return "not a BMP file";
			bi.dataOffset = Le32(d + 10);
			const uint32_t hs = Le32(d + 14);
			const uint8_t* h = d + 14;
			if (hs == 12)
			{
				const int hh = (int16_t)Le16(h + 6);
				bi.w = Le16(h + 4), bi.h = std::abs(hh), bi.topDown = hh < 0, bi.bpp = Le16(h + 10);
			}
			else if (hs >= 40 && size >= 14 + 40)
			{
				const int32_t hh = (int32_t)Le32(h + 8);
				bi.w = (int32_t)Le32(h + 4), bi.h = hh < 0 ? -hh : hh, bi.topDown = hh < 0, bi.bpp = Le16(h + 14), bi.compression = Le32(h + 16);
			}
			else
				return "unsupported BMP header";
			if (bi.w <= 0 || bi.h <= 0 || bi.w > 65535 || bi.h > 65535)
				return "bad BMP size";
			if (bi.compression != 0 && bi.compression != 1 && bi.compression != 2 && bi.compression != 3 && bi.compression != 6)
				return "unsupported BMP compression";
			size_t tail = 14 + hs;
			if (bi.compression == 3 || bi.compression == 6)
			{
				const size_t nm = bi.compression == 6 || hs >= 56 ? 4 : 3;
				if (size < 14 + 40 + nm * 4)
					return "truncated BMP masks";
				for (size_t i = 0; i < nm; ++i)
					bi.ch[i].Set(Le32(h + 40 + i * 4));
				if (hs == 40)
					tail += nm * 4;
			}
			else if (bi.bpp == 16)
				bi.ch[0].Set(0x7C00), bi.ch[1].Set(0x03E0), bi.ch[2].Set(0x001F);
			else if (bi.bpp == 32)
				bi.ch[0].Set(0x00FF0000), bi.ch[1].Set(0x0000FF00), bi.ch[2].Set(0x000000FF);
			if (bi.bpp <= 8)
			{
				const size_t entry = hs == 12 ? 3 : 4;
				size_t count = hs >= 40 ? Le32(h + 32) : 0;
				if (count == 0 || count > 256)
					count = size_t(1) << bi.bpp;
				if (tail + count * entry > size)
					return "truncated BMP palette";
				bi.palette.assign(256 * 4, 0);
				for (size_t i = 0; i < count; ++i)
				{
					const uint8_t* e = d + tail + i * entry;
					bi.palette[i * 4 + 0] = e[2], bi.palette[i * 4 + 1] = e[1], bi.palette[i * 4 + 2] = e[0], bi.palette[i * 4 + 3] = 255;
				}
			}
			else if (bi.bpp != 16 && bi.bpp != 24 && bi.bpp != 32)
				return "unsupported BMP bit depth";
			if (bi.dataOffset >= size)
				return "truncated BMP data";
			return {};
		}

		void Put(uint8_t* dst, size_t pitch, int maxW, int maxH, const Info& bi, int x, int fileRow, const uint8_t* rgba)
		{
			const int y = bi.topDown ? fileRow : bi.h - 1 - fileRow;
			if (x < maxW && y >= 0 && y < maxH)
				memcpy(dst + (size_t)y * pitch + (size_t)x * 4, rgba, 4);
		}

		std::string DecodeRle(const uint8_t* d, size_t size, const Info& bi, uint8_t* dst, size_t pitch, int maxW, int maxH)
		{
			const bool rle4 = bi.compression == 2;
			const uint8_t* p = d + bi.dataOffset;
			const uint8_t* end = d + size;
			int x = 0, row = 0;
			auto pixel = [&](int index) { Put(dst, pitch, maxW, maxH, bi, x++, row, &bi.palette[(index & 255) * 4]); };
			while (p + 2 <= end && row < bi.h)
			{
				const int count = p[0], value = p[1];
				p += 2;
				if (count > 0)
				{
					for (int i = 0; i < count; ++i)
						pixel(rle4 ? (i & 1 ? value & 15 : value >> 4) : value);
					continue;
				}
				if (value == 0)
					x = 0, ++row;
				else if (value == 1)
					break;
				else if (value == 2)
				{
					if (p + 2 > end)
						break;
					x += p[0], row += p[1], p += 2;
				}
				else
				{
					const size_t bytes = rle4 ? (value + 1) / 2 : value;
					if (p + bytes > end)
						return "truncated BMP RLE run";
					for (int i = 0; i < value; ++i)
						pixel(rle4 ? (i & 1 ? p[i / 2] & 15 : p[i / 2] >> 4) : p[i]);
					p += (bytes + 1) & ~size_t(1);
				}
			}
			return {};
		}
	}

	std::string ProbeBmp(const uint8_t* data, size_t size, Probe& out)
	{
		Info bi;
		const std::string err = Parse(data, size, bi);
		out.w = bi.w, out.h = bi.h;
		return err;
	}

	std::string DecodeBmp(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Info bi;
		if (std::string err = Parse(data, size, bi); !err.empty())
			return err;
		if (bi.compression == 1 || bi.compression == 2)
		{
			for (int y = 0; y < maxH; ++y)
				memset(dst + (size_t)y * pitch, 0, (size_t)maxW * 4);
			return DecodeRle(data, size, bi, dst, pitch, maxW, maxH);
		}
		const size_t stride = ((size_t)bi.w * bi.bpp + 31) / 32 * 4;
		if (bi.dataOffset + stride * bi.h > size)
			return "truncated BMP pixel data";
		const bool alpha = bi.ch[3].mask != 0;
		const int w = std::min(bi.w, maxW);
		par.For(bi.h, 64, [&](int64_t r0, int64_t r1, int)
		{
			for (int64_t row = r0; row < r1; ++row)
			{
				const int64_t y = bi.topDown ? row : bi.h - 1 - row;
				if (y >= maxH)
					continue;
				const uint8_t* s = data + bi.dataOffset + stride * row;
				uint8_t* o = dst + (size_t)y * pitch;
				for (int x = 0; x < w; ++x, o += 4)
				{
					switch (bi.bpp)
					{
					case 1: case 2: case 4: case 8:
					{
						const int bit = x * bi.bpp;
						const int index = (s[bit >> 3] >> (8 - bi.bpp - (bit & 7))) & ((1 << bi.bpp) - 1);
						memcpy(o, &bi.palette[index * 4], 4);
						break;
					}
					case 24:
						o[0] = s[x * 3 + 2], o[1] = s[x * 3 + 1], o[2] = s[x * 3 + 0], o[3] = 255;
						break;
					default:
					{
						const uint32_t px = bi.bpp == 16 ? Le16(s + x * 2) : Le32(s + x * 4);
						o[0] = bi.ch[0].Get(px, 0), o[1] = bi.ch[1].Get(px, 0), o[2] = bi.ch[2].Get(px, 0), o[3] = alpha ? bi.ch[3].Get(px, 255) : 255;
						break;
					}
					}
				}
			}
		});
		return {};
	}
}
