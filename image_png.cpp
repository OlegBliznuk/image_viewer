#include "image_formats.h"

#include <libdeflate.h>

#include <immintrin.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "deflate.lib")

// PNG: IDAT inflated by libdeflate in one call, rows unfiltered per pixel with SSE2; 8-bit RGB / RGBA rows are unfiltered straight into the frame, other layouts converted afterwards in parallel (same output as stb_image)
namespace iv::fmt
{
	namespace
	{
		struct Png
		{
			int w = 0, h = 0, depth = 0, colorType = 0, interlace = 0;
			bool trns = false;
			uint16_t key[3] = {};   // tRNS colour key of gray / RGB images (in sample units)
			uint8_t palette[256 * 4] = {};
			std::vector<std::pair<const uint8_t*, size_t>> idat;
			size_t idatBytes = 0;

			int Channels() const { return colorType == 0 ? 1 : colorType == 2 ? 3 : colorType == 3 ? 1 : colorType == 4 ? 2 : 4; }
			int BitsPerPixel() const { return Channels() * depth; }
			size_t RowBytes(int width) const { return ((size_t)width * BitsPerPixel() + 7) / 8; }
			int FilterBpp() const { return std::max(1, BitsPerPixel() / 8); }
			// RGB without a colour key stays 3 channels in the frame
			bool Packed() const { return colorType == 2 && !trns; }
			bool Sixteen() const { return depth == 16; }
		};

		std::string Parse(const uint8_t* d, size_t size, Png& png, bool needData)
		{
			if (size < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0)
				return "png: bad signature";
			bool header = false, plte = false;
			for (size_t at = 8; at + 12 <= size;)
			{
				const uint32_t len = Be32(d + at);
				const uint8_t* type = d + at + 4;
				const uint8_t* body = d + at + 8;
				if (len > size - at - 12)
					return "png: truncated chunk";
				if (!memcmp(type, "IHDR", 4) && len >= 13)
				{
					png.w = (int)Be32(body), png.h = (int)Be32(body + 4), png.depth = body[8], png.colorType = body[9], png.interlace = body[12];
					const int ct = png.colorType, bd = png.depth;
					const bool ok = (ct == 0 && (bd == 1 || bd == 2 || bd == 4 || bd == 8 || bd == 16)) || (ct == 3 && (bd == 1 || bd == 2 || bd == 4 || bd == 8)) || ((ct == 2 || ct == 4 || ct == 6) && (bd == 8 || bd == 16));
					if (!ok || body[10] != 0 || body[11] != 0 || png.interlace > 1 || png.w <= 0 || png.h <= 0 || png.w > (1 << 24) || png.h > (1 << 24))
						return "png: unsupported header";
					header = true;
				}
				else if (!memcmp(type, "PLTE", 4))
				{
					for (uint32_t i = 0; i < std::min<uint32_t>(len / 3, 256); ++i)
						png.palette[i * 4] = body[i * 3], png.palette[i * 4 + 1] = body[i * 3 + 1], png.palette[i * 4 + 2] = body[i * 3 + 2], png.palette[i * 4 + 3] = 255;
					plte = true;
				}
				else if (!memcmp(type, "tRNS", 4))
				{
					png.trns = true;
					if (png.colorType == 3)
						for (uint32_t i = 0; i < std::min<uint32_t>(len, 256); ++i)
							png.palette[i * 4 + 3] = body[i];
					else if (png.colorType == 0 && len >= 2)
						png.key[0] = (uint16_t)(body[0] << 8 | body[1]);
					else if (png.colorType == 2 && len >= 6)
						for (int c = 0; c < 3; ++c)
							png.key[c] = (uint16_t)(body[c * 2] << 8 | body[c * 2 + 1]);
				}
				else if (!memcmp(type, "IDAT", 4))
				{
					if (!header)
						return "png: IDAT before IHDR";
					if (!needData)
						return {};
					png.idat.push_back({ body, len });
					png.idatBytes += len;
				}
				else if (!memcmp(type, "IEND", 4))
					break;
				at += 12 + (size_t)len;
			}
			if (!header)
				return "png: no IHDR";
			if (png.colorType == 3 && !plte)
				return "png: palette missing";
			if (!needData)
				return "png: header not complete";   // the probe needs everything up to the first IDAT
			return png.idat.empty() ? "png: no image data" : std::string();
		}

		inline __m128i Load4(const uint8_t* p) { int v; memcpy(&v, p, 4); return _mm_cvtsi32_si128(v); }
		inline void Store4(uint8_t* p, __m128i v) { const int x = _mm_cvtsi128_si32(v); memcpy(p, &x, 4); }
		inline __m128i Load3(const uint8_t* p) { int v = 0; memcpy(&v, p, 3); return _mm_cvtsi32_si128(v); }
		inline void Store3(uint8_t* p, __m128i v) { const int x = _mm_cvtsi128_si32(v); memcpy(p, &x, 3); }

		// Paeth on 16-bit lanes: the predictor among a, b, c whose distance to a + b - c is smallest (ties a, then b)
		inline __m128i Paeth(__m128i a, __m128i b, __m128i c)
		{
			const __m128i pa = _mm_abs_epi16(_mm_sub_epi16(b, c)), pb = _mm_abs_epi16(_mm_sub_epi16(a, c)), pc = _mm_abs_epi16(_mm_sub_epi16(_mm_add_epi16(a, b), _mm_add_epi16(c, c)));
			const __m128i useB = _mm_cmpgt_epi16(pa, pb), smallest = _mm_min_epi16(pa, pb);
			const __m128i ab = _mm_blendv_epi8(a, b, useB);
			return _mm_blendv_epi8(ab, c, _mm_cmpgt_epi16(smallest, pc));
		}

		// one row in place from its filtered bytes; prev is the unfiltered row above (zeros for the first)
		void Unfilter(int filter, uint8_t* row, const uint8_t* prev, size_t n, int bpp)
		{
			switch (filter)
			{
			case 0:
				return;
			case 2:
			{
				size_t i = 0;
				for (; i + 32 <= n; i += 32)
					_mm256_storeu_si256((__m256i*)(row + i), _mm256_add_epi8(_mm256_loadu_si256((const __m256i*)(row + i)), _mm256_loadu_si256((const __m256i*)(prev + i))));
				for (; i < n; ++i)
					row[i] = (uint8_t)(row[i] + prev[i]);
				return;
			}
			}
			// sub / average / paeth: pixel after pixel, 3 or 4 bytes in one register
			if (bpp == 3 || bpp == 4)
			{
				const auto load = bpp == 3 ? Load3 : Load4;
				const auto store = bpp == 3 ? Store3 : Store4;
				const __m128i zero = _mm_setzero_si128();
				__m128i a = zero, c = zero;
				size_t i = 0;
				if (filter == 1)
					for (; i + bpp <= n; i += bpp)
						a = _mm_add_epi8(load(row + i), a), store(row + i, a);
				else if (filter == 3)
					for (; i + bpp <= n; i += bpp)
					{
						const __m128i b = load(prev + i);
						const __m128i avg = _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
						a = _mm_add_epi8(load(row + i), avg), store(row + i, a);
					}
				else
					for (; i + bpp <= n; i += bpp)
					{
						const __m128i b = _mm_unpacklo_epi8(load(prev + i), zero);
						const __m128i p = Paeth(_mm_unpacklo_epi8(a, zero), b, _mm_unpacklo_epi8(c, zero));
						a = _mm_add_epi8(load(row + i), _mm_packus_epi16(p, p)), store(row + i, a);
						c = _mm_packus_epi16(b, b);
					}
				return;
			}
			for (size_t i = 0; i < n; ++i)
			{
				const int a = i >= (size_t)bpp ? row[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
				int p = 0;
				if (filter == 1)
					p = a;
				else if (filter == 3)
					p = (a + b) >> 1;
				else
				{
					const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
					p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
				}
				row[i] = (uint8_t)(row[i] + p);
			}
		}

		// unfiltered samples of one row to the frame's pixel format: RGB8 / RGB16 (3 channels, no colour key) or RGBA8 / RGBA16, 16-bit in host order
		void Convert(const Png& png, const uint8_t* src, int width, uint8_t* dst)
		{
			const int ct = png.colorType, bd = png.depth;
			if (bd == 16)
			{
				uint16_t* o = (uint16_t*)dst;
				const auto s16 = [&](int i) { return (uint16_t)(src[i * 2] << 8 | src[i * 2 + 1]); };
				for (int x = 0; x < width; ++x)
				{
					if (ct == 0 || ct == 4)
					{
						const uint16_t g = s16(x * png.Channels());
						o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = g;
						o[x * 4 + 3] = ct == 4 ? s16(x * 2 + 1) : png.trns && g == png.key[0] ? 0 : 65535;
					}
					else if (ct == 2)
					{
						const uint16_t r = s16(x * 3), g = s16(x * 3 + 1), b = s16(x * 3 + 2);
						if (png.Packed())
							o[x * 3] = r, o[x * 3 + 1] = g, o[x * 3 + 2] = b;
						else
							o[x * 4] = r, o[x * 4 + 1] = g, o[x * 4 + 2] = b, o[x * 4 + 3] = r == png.key[0] && g == png.key[1] && b == png.key[2] ? 0 : 65535;
					}
					else
						for (int c = 0; c < 4; ++c)
							o[x * 4 + c] = s16(x * 4 + c);
				}
				return;
			}
			if (ct == 2 && png.Packed())
				return (void)memcpy(dst, src, (size_t)width * 3);
			if (ct == 6)
				return (void)memcpy(dst, src, (size_t)width * 4);
			for (int x = 0; x < width; ++x)
			{
				uint8_t* o = dst + x * 4;
				if (ct == 2)
				{
					o[0] = src[x * 3], o[1] = src[x * 3 + 1], o[2] = src[x * 3 + 2];
					o[3] = o[0] == png.key[0] && o[1] == png.key[1] && o[2] == png.key[2] ? 0 : 255;
				}
				else if (ct == 4)
					o[0] = o[1] = o[2] = src[x * 2], o[3] = src[x * 2 + 1];
				else
				{
					// gray or palette index, 1 / 2 / 4 / 8 bits
					const int bit = x * bd, v = bd == 8 ? src[x] : (src[bit >> 3] >> (8 - bd - (bit & 7))) & ((1 << bd) - 1);
					if (ct == 3)
						memcpy(o, png.palette + v * 4, 4);
					else
					{
						const int scale = bd == 1 ? 255 : bd == 2 ? 0x55 : bd == 4 ? 0x11 : 1;
						o[0] = o[1] = o[2] = (uint8_t)(v * scale);
						o[3] = png.trns && v == png.key[0] ? 0 : 255;
					}
				}
			}
		}
	}

	std::string ProbePng(const uint8_t* data, size_t size, Probe& out)
	{
		Png png;
		if (std::string err = Parse(data, size, png, false); !err.empty())
			return err;
		out.w = png.w, out.h = png.h;
		out.bits = png.Sixteen() ? 16 : 8;
		out.channels = png.Packed() ? 3 : 4;
		return {};
	}

	std::string DecodePng(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Png png;
		if (std::string err = Parse(data, size, png, true); !err.empty())
			return err;
		// the zlib stream of all IDAT chunks, in one buffer
		Bytes z;
		const uint8_t* zdata = png.idat[0].first;
		if (png.idat.size() > 1)
		{
			z.resize(png.idatBytes);
			size_t at = 0;
			for (const auto& [p, n] : png.idat)
				memcpy(z.data() + at, p, n), at += n;
			zdata = z.data();
		}
		const int pixelBytes = (png.Packed() ? 3 : 4) * (png.Sixteen() ? 2 : 1);
		const int bpp = png.FilterBpp();
		static constexpr int kPass[7][4] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
		const int passes = png.interlace ? 7 : 1;
		size_t rawBytes = 0;
		for (int ps = 0; ps < passes; ++ps)
		{
			const int pw = png.interlace ? (png.w - kPass[ps][0] + kPass[ps][2] - 1) / kPass[ps][2] : png.w, ph = png.interlace ? (png.h - kPass[ps][1] + kPass[ps][3] - 1) / kPass[ps][3] : png.h;
			if (pw > 0 && ph > 0)
				rawBytes += (png.RowBytes(pw) + 1) * ph;
		}
		Bytes raw;
		raw.resize(rawBytes);
		libdeflate_decompressor* inflater = libdeflate_alloc_decompressor();
		if (!inflater)
			return "png: out of memory";
		// the raw deflate stream after the 2-byte zlib header: like stb_image, no Adler-32 check and a short stream decodes as far as it goes
		size_t got = 0;
		const libdeflate_result res = png.idatBytes > 2 && (zdata[1] & 0x20) == 0 ? libdeflate_deflate_decompress(inflater, zdata + 2, png.idatBytes - 2, raw.data(), raw.size(), &got) : LIBDEFLATE_BAD_DATA;
		libdeflate_free_decompressor(inflater);
		if (res == LIBDEFLATE_BAD_DATA && got == 0)
			return "png: corrupt image data";
		if (got < raw.size())
			memset(raw.data() + got, 0, raw.size() - got);
		const int cols = std::min(png.w, maxW), rows = std::min(png.h, maxH);
		// 8-bit RGB / RGBA without interlace: unfiltered into the frame, the row above read back from it
		if (!png.interlace && png.depth == 8 && (png.colorType == 6 || png.Packed()) && cols == png.w)
		{
			const size_t n = png.RowBytes(png.w);
			std::vector<uint8_t> zero(n, 0);
			for (int y = 0; y < rows; ++y)
			{
				const uint8_t* line = raw.data() + (n + 1) * y;
				uint8_t* out = dst + (size_t)y * pitch;
				memcpy(out, line + 1, n);
				Unfilter(line[0], out, y ? out - pitch : zero.data(), n, bpp);
			}
			return {};
		}
		// unfiltered in place pass by pass, then converted into the frame in parallel
		std::vector<uint8_t> zero(png.RowBytes(png.w), 0);
		size_t at = 0;
		for (int ps = 0; ps < passes; ++ps)
		{
			const int x0 = png.interlace ? kPass[ps][0] : 0, y0 = png.interlace ? kPass[ps][1] : 0, dx = png.interlace ? kPass[ps][2] : 1, dy = png.interlace ? kPass[ps][3] : 1;
			const int pw = (png.w - x0 + dx - 1) / dx, ph = (png.h - y0 + dy - 1) / dy;
			if (pw <= 0 || ph <= 0)
				continue;
			const size_t n = png.RowBytes(pw);
			uint8_t* base = raw.data() + at;
			for (int y = 0; y < ph; ++y)
			{
				uint8_t* line = base + (n + 1) * y;
				Unfilter(line[0], line + 1, y ? line + 1 - (n + 1) : zero.data(), n, bpp);
			}
			par.For(ph, 32, [&](int64_t r0, int64_t r1, int)
			{
				std::vector<uint8_t> px;
				if (png.interlace)
					px.resize((size_t)pw * pixelBytes);
				for (int64_t r = r0; r < r1; ++r)
				{
					const int y = y0 + (int)r * dy;
					if (y >= rows)
						break;
					const uint8_t* src = base + (n + 1) * r + 1;
					if (!png.interlace)
					{
						if (cols == png.w)
							Convert(png, src, png.w, dst + (size_t)y * pitch);
						else
						{
							px.resize((size_t)png.w * pixelBytes);
							Convert(png, src, png.w, px.data());
							memcpy(dst + (size_t)y * pitch, px.data(), (size_t)cols * pixelBytes);
						}
						continue;
					}
					Convert(png, src, pw, px.data());
					for (int i = 0; i < pw; ++i)
						if (const int x = x0 + i * dx; x < cols)
							memcpy(dst + (size_t)y * pitch + (size_t)x * pixelBytes, px.data() + (size_t)i * pixelBytes, pixelBytes);
				}
			});
			at += (n + 1) * ph;
		}
		return {};
	}
}
