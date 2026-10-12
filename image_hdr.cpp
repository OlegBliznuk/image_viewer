#include "image_formats.h"

#include <immintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Radiance RGBE / XYZE: new and old RLE, '-Y h +X w' layout; rows decode in parallel to 4-byte RGBE pixels the GPU expands
namespace iv::fmt
{
	namespace
	{
		struct Header
		{
			int w = 0, h = 0;
			bool xyze = false;
			size_t dataOffset = 0;
		};

		std::string Parse(const uint8_t* d, size_t size, Header& hd)
		{
			if (size < 16)
				return "[hdr-read] file too short";
			size_t pos = 0;
			auto readLine = [&](std::string& out)
			{
				out.clear();
				while (pos < size && d[pos] != '\n')
					out += (char)d[pos++];
				if (pos < size)
					++pos;
				return pos <= size;
			};
			std::string line;
			readLine(line);
			if (line.rfind("#?RADIANCE", 0) != 0 && line.rfind("#?RGBE", 0) != 0)
				return "[hdr-read] not a Radiance HDR file";
			while (true)
			{
				if (pos >= size)
					return "[hdr-read] unexpected eof in header";
				readLine(line);
				if (line.empty())
					break;
				if (line.rfind("FORMAT=", 0) == 0)
				{
					if (line.ends_with("32-bit_rle_rgbe"))
						hd.xyze = false;
					else if (line.ends_with("32-bit_rle_xyze"))
						hd.xyze = true;
					else
						return "[hdr-read] unsupported FORMAT: " + line;
				}
			}
			readLine(line);
			if (sscanf(line.c_str(), "-Y %d +X %d", &hd.h, &hd.w) != 2)
				return "[hdr-read] unsupported resolution layout (need '-Y h +X w'): " + line;
			if (hd.w <= 0 || hd.h <= 0)
				return "[hdr-read] bad resolution";
			hd.dataOffset = pos;
			return {};
		}

		bool NewRle(const uint8_t* p, int w)
		{
			return p[0] == 2 && p[1] == 2 && (p[2] & 0x80) == 0 && ((uint32_t(p[2]) << 8 | p[3]) == (uint32_t)w);
		}

		// the scanline starting at `at`, skipped without decoding; false when the data ends first
		bool SkipScanline(const uint8_t* d, size_t size, int w, size_t& at)
		{
			if (at + 4 > size)
				return false;
			if (NewRle(d + at, w))
			{
				at += 4;
				for (int ch = 0; ch < 4; ++ch)
					for (int x = 0; x < w;)
					{
						if (at >= size)
							return false;
						const uint8_t b = d[at++];
						if (b > 128)
							x += b - 128, at += 1;
						else
							x += b, at += b;
					}
				return at <= size;
			}
			for (int x = 0; x < w;)
			{
				if (at + 4 > size)
					return false;
				const uint8_t* q = d + at;
				at += 4;
				x += q[0] == 1 && q[1] == 1 && q[2] == 1 ? q[3] : 1;
			}
			return true;
		}

		// the four planes of a new-RLE scanline (r, g, b, e: w bytes each)
		std::string ReadPlanes(const uint8_t* p, const uint8_t* end, int w, uint8_t* scan)
		{
			for (int ch = 0; ch < 4; ++ch)
			{
				uint8_t* plane = scan + (size_t)ch * w;
				for (int x = 0; x < w;)
				{
					if (p >= end)
						return "[hdr-read] truncated RLE";
					const uint8_t b = *p++;
					if (b > 128)
					{
						const int run = b - 128;
						if (p >= end || x + run > w)
							return "[hdr-read] bad RLE run";
						memset(plane + x, *p++, run);
						x += run;
					}
					else
					{
						if (p + b > end || x + b > w)
							return "[hdr-read] bad RLE literal";
						memcpy(plane + x, p, b);
						p += b, x += b;
					}
				}
			}
			return {};
		}

		// RGBE planes to interleaved RGBE pixels, 32 per step; aligned rows take streaming stores (the frame is not read back on the CPU)
		void Interleave(const uint8_t* scan, int w, int n, uint8_t* out)
		{
			const uint8_t *r = scan, *g = scan + w, *b = scan + 2 * (size_t)w, *e = scan + 3 * (size_t)w;
			const bool stream = ((uintptr_t)out & 31) == 0;
			int x = 0;
			for (; x + 32 <= n; x += 32)
			{
				const __m256i R = _mm256_loadu_si256((const __m256i*)(r + x)), G = _mm256_loadu_si256((const __m256i*)(g + x));
				const __m256i B = _mm256_loadu_si256((const __m256i*)(b + x)), E = _mm256_loadu_si256((const __m256i*)(e + x));
				const __m256i rgLo = _mm256_unpacklo_epi8(R, G), rgHi = _mm256_unpackhi_epi8(R, G), beLo = _mm256_unpacklo_epi8(B, E), beHi = _mm256_unpackhi_epi8(B, E);
				// lanes hold pixels 0-3 | 16-19, 4-7 | 20-23, 8-11 | 24-27, 12-15 | 28-31; the 128-bit permutes put them in order
				const __m256i p0 = _mm256_unpacklo_epi16(rgLo, beLo), p1 = _mm256_unpackhi_epi16(rgLo, beLo), p2 = _mm256_unpacklo_epi16(rgHi, beHi), p3 = _mm256_unpackhi_epi16(rgHi, beHi);
				const __m256i o[4] = { _mm256_permute2x128_si256(p0, p1, 0x20), _mm256_permute2x128_si256(p2, p3, 0x20), _mm256_permute2x128_si256(p0, p1, 0x31), _mm256_permute2x128_si256(p2, p3, 0x31) };
				__m256i* dst = (__m256i*)(out + (size_t)x * 4);
				for (int k = 0; k < 4; ++k)
					stream ? _mm256_stream_si256(dst + k, o[k]) : _mm256_storeu_si256(dst + k, o[k]);
			}
			for (; x < n; ++x)
				out[x * 4] = r[x], out[x * 4 + 1] = g[x], out[x * 4 + 2] = b[x], out[x * 4 + 3] = e[x];
		}

		// an old-RLE or flat scanline, pixel by pixel
		std::string OldScanline(const uint8_t* p, const uint8_t* end, int w, int n, uint8_t* out)
		{
			uint8_t last[4] = { 0, 0, 0, 0 };
			for (int x = 0; x < w;)
			{
				if (p + 4 > end)
					return "[hdr-read] truncated old-RLE";
				const uint8_t* q = p;
				p += 4;
				if (q[0] == 1 && q[1] == 1 && q[2] == 1)
				{
					if (x == 0)
						return "[hdr-read] RLE run at scanline start";
					for (int i = 0; i < q[3] && x < w; ++i, ++x)
						if (x < n)
							memcpy(out + (size_t)x * 4, last, 4);
					continue;
				}
				memcpy(last, q, 4);
				if (x < n)
					memcpy(out + (size_t)x * 4, last, 4);
				++x;
			}
			return {};
		}

		// the first new-RLE scanline header in [from, to) that two more headers follow, or SIZE_MAX
		size_t FindScanline(const uint8_t* d, size_t size, int w, size_t from, size_t to)
		{
			for (size_t at = from; at + 4 <= std::min(to, size); ++at)
			{
				if (!NewRle(d + at, w))
					continue;
				size_t q = at;
				bool ok = true;
				for (int k = 0; k < 2 && ok; ++k)
					ok = SkipScanline(d, size, w, q) && (q + 4 > size || NewRle(d + q, w));
				if (ok)
					return at;
			}
			return SIZE_MAX;
		}

		// where each of the h scanlines starts: new-RLE files are cut into segments walked in parallel from the first plausible header after each cut, joined where the true chain meets them (a segment that started on a false header is walked again from the true position)
		std::string ScanlineStarts(const uint8_t* d, size_t size, int w, int h, size_t first, const Parallel& par, std::vector<size_t>& starts)
		{
			starts.clear();
			starts.reserve(h);
			const auto walk = [&](size_t at, size_t until, std::vector<size_t>& out, bool& truncated)
			{
				truncated = false;
				while (at < until && (int)out.size() < h)
				{
					out.push_back(at);
					if (!SkipScanline(d, size, w, at))
						return truncated = true, at;
				}
				return at;
			};
			const int segments = par.threads > 1 && NewRle(d + first, w) ?(int)std::clamp<int64_t>((int64_t)(size - first) >> 21, 1, std::max(1, par.threads * 2)) : 1;
			const auto cut = [&](int s) { return s == segments ? size : first + (size - first) * s / segments; };
			struct Segment { std::vector<size_t> starts; size_t end = 0; bool truncated = false; };
			std::vector<Segment> seg(segments);
			if (segments > 1)
				par.For(segments - 1, 1, [&](int64_t a, int64_t b, int)
				{
					for (int64_t s = a + 1; s <= b; ++s)
						if (const size_t at = FindScanline(d, size, w, cut((int)s), cut((int)s + 1)); at != SIZE_MAX)
							seg[s].end = walk(at, cut((int)s + 1), seg[s].starts, seg[s].truncated);
				});
			bool truncated = false;
			size_t at = walk(first, cut(1), starts, truncated);
			for (int s = 1; s < segments && !truncated && (int)starts.size() < h; ++s)
			{
				if (!seg[s].starts.empty() && seg[s].starts.front() == at)
				{
					const size_t take = std::min(seg[s].starts.size(), (size_t)h - starts.size());
					starts.insert(starts.end(), seg[s].starts.begin(), seg[s].starts.begin() + take);
					at = seg[s].end, truncated = seg[s].truncated && take == seg[s].starts.size();
				}
				else
					at = walk(at, cut(s + 1), starts, truncated);
			}
			if ((int)starts.size() < h && !truncated)
				walk(at, size, starts, truncated);
			if ((int)starts.size() < h)
				return "[hdr-read] truncated scanline " + std::to_string(starts.size());
			return {};
		}
	}

	std::string ProbeHdr(const uint8_t* data, size_t size, Probe& out)
	{
		Header hd;
		const std::string err = Parse(data, size, hd);
		out.w = hd.w, out.h = hd.h, out.bits = 32, out.floatSamples = true;
		// XYZE stays XYZ in the frame; the display turns it into sRGB primaries
		const float xyzToRgb[9] = { 3.2404542f, -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f, 0.0415560f, 0.0556434f, -0.2040259f, 1.0572252f };
		if (hd.xyze)
			out.colorMatrix = true, std::copy(xyzToRgb, xyzToRgb + 9, out.toRec709);
		return err;
	}

	std::string DecodeHdr(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Header hd;
		if (std::string err = Parse(data, size, hd); !err.empty())
			return err;
		const int w = hd.w, h = std::min(hd.h, maxH), n = std::min(w, maxW);
		// RLE scanlines vary in length: find where each starts, then decode the rows in parallel
		std::vector<size_t> starts;
		if (std::string err = ScanlineStarts(data, size, w, h, hd.dataOffset, par, starts); !err.empty())
			return err;
		std::vector<std::vector<uint8_t>> scans(par.MaxWorkers());
		std::vector<std::string> errors(par.MaxWorkers());
		par.For(h, 16, [&](int64_t y0, int64_t y1, int worker)
		{
			std::vector<uint8_t>& scan = scans[worker];
			if (scan.empty())
				scan.resize((size_t)w * 4);
			const uint8_t* end = data + size;
			for (int64_t y = y0; y < y1 && errors[worker].empty(); ++y)
			{
				const uint8_t* p = data + starts[y];
				uint8_t* out = dst + (size_t)y * pitch;
				if (NewRle(p, w))
				{
					errors[worker] = ReadPlanes(p + 4, end, w, scan.data());
					if (errors[worker].empty())
						Interleave(scan.data(), w, n, out);
				}
				else
					errors[worker] = OldScanline(p, end, w, n, out);
			}
			_mm_sfence();
		});
		for (const std::string& e : errors)
			if (!e.empty())
				return e;
		return {};
	}
}
