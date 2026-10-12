#include "image_formats.h"

#include <algorithm>
#include <cstring>

// TGA: types 1/2/3 and RLE 9/10/11; 8, 15, 16, 24, 32 bpp pixels and colour maps; any origin corner
namespace iv::fmt
{
	namespace
	{
		struct Info
		{
			int w = 0, h = 0, type = 0, bpp = 0, alphaBits = 0;
			bool rightToLeft = false, topDown = false;
			size_t dataOffset = 0;
			int mapFirst = 0, mapLength = 0, mapBpp = 0;
			size_t mapOffset = 0;
		};

		std::string Parse(const uint8_t* d, size_t size, Info& t)
		{
			if (size < 18)
				return "truncated TGA header";
			const int idLength = d[0], mapType = d[1];
			t.type = d[2];
			t.mapFirst = Le16(d + 3), t.mapLength = Le16(d + 5), t.mapBpp = d[7];
			t.w = Le16(d + 12), t.h = Le16(d + 14), t.bpp = d[16];
			t.alphaBits = d[17] & 15, t.rightToLeft = (d[17] & 0x10) != 0, t.topDown = (d[17] & 0x20) != 0;
			const int base = t.type & 7;
			if ((t.type & ~8) < 1 || (t.type & ~8) > 3 || t.w <= 0 || t.h <= 0)
				return "not a supported TGA file";
			if (base == 1 && (mapType != 1 || t.bpp != 8 || (t.mapBpp != 15 && t.mapBpp != 16 && t.mapBpp != 24 && t.mapBpp != 32)))
				return "unsupported TGA colour map";
			if (base == 2 && t.bpp != 15 && t.bpp != 16 && t.bpp != 24 && t.bpp != 32)
				return "unsupported TGA bit depth";
			if (base == 3 && t.bpp != 8 && t.bpp != 16)
				return "unsupported TGA grey depth";
			t.mapOffset = 18 + idLength;
			t.dataOffset = t.mapOffset + (mapType == 1 ? (size_t)t.mapLength * ((t.mapBpp + 7) / 8) : 0);
			if (t.dataOffset > size)
				return "truncated TGA colour map";
			return {};
		}

		// one source pixel to RGBA
		void Convert(const uint8_t* s, int bits, bool gray, int alphaBits, uint8_t* o)
		{
			if (gray)
			{
				o[0] = o[1] = o[2] = s[0], o[3] = bits == 16 ? s[1] : 255;
				return;
			}
			switch (bits)
			{
			case 15: case 16:
			{
				const int v = s[0] | (s[1] << 8);
				o[0] = (uint8_t)(((v >> 10) & 31) * 255 / 31), o[1] = (uint8_t)(((v >> 5) & 31) * 255 / 31), o[2] = (uint8_t)((v & 31) * 255 / 31);
				o[3] = bits == 16 && alphaBits > 0 ? ((v & 0x8000) ? 255 : 0) : 255;
				break;
			}
			case 24:
				o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = 255;
				break;
			default:
				o[0] = s[2], o[1] = s[1], o[2] = s[0], o[3] = s[3];
				break;
			}
		}
	}

	std::string ProbeTga(const uint8_t* data, size_t size, Probe& out)
	{
		Info t;
		const std::string err = Parse(data, size, t);
		out.w = t.w, out.h = t.h;
		return err;
	}

	std::string DecodeTga(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Info t;
		if (std::string err = Parse(data, size, t); !err.empty())
			return err;
		const int base = t.type & 7;
		const bool rle = (t.type & 8) != 0;
		const int bytes = (t.bpp + 7) / 8;
		std::vector<uint8_t> map;
		if (base == 1)
		{
			map.assign(256 * 4, 0);
			const int mb = (t.mapBpp + 7) / 8;
			for (int i = 0; i < t.mapLength; ++i)
				if (t.mapFirst + i < 256)
					Convert(data + t.mapOffset + (size_t)i * mb, t.mapBpp, false, t.mapBpp == 32 ? 8 : t.alphaBits, &map[(t.mapFirst + i) * 4]);
		}
		auto put = [&](size_t index, const uint8_t* s)
		{
			const int sx = (int)(index % t.w), sy = (int)(index / t.w);
			const int x = t.rightToLeft ? t.w - 1 - sx : sx, y = t.topDown ? sy : t.h - 1 - sy;
			if (x >= maxW || y >= maxH)
				return;
			uint8_t* o = dst + (size_t)y * pitch + (size_t)x * 4;
			if (base == 1)
				memcpy(o, &map[s[0] * 4], 4);
			else
				Convert(s, t.bpp, base == 3, t.alphaBits, o);
		};
		const size_t total = (size_t)t.w * t.h;
		const uint8_t* p = data + t.dataOffset;
		const uint8_t* end = data + size;
		if (!rle)
		{
			if (p + total * bytes > end)
				return "truncated TGA pixel data";
			par.For(t.h, 64, [&](int64_t r0, int64_t r1, int)
			{
				for (size_t i = (size_t)r0 * t.w; i < (size_t)r1 * t.w; ++i)
					put(i, p + i * bytes);
			});
			return {};
		}
		for (size_t i = 0; i < total;)
		{
			if (p >= end)
				return "truncated TGA RLE data";
			const int header = *p++, count = (header & 127) + 1;
			if (header & 128)
			{
				if (p + bytes > end)
					return "truncated TGA RLE packet";
				for (int k = 0; k < count && i < total; ++k)
					put(i++, p);
				p += bytes;
			}
			else
			{
				if (p + (size_t)count * bytes > end)
					return "truncated TGA raw packet";
				for (int k = 0; k < count && i < total; ++k, p += bytes)
					put(i++, p);
			}
		}
		return {};
	}
}
