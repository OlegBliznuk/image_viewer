#include "image_formats.h"

#include <algorithm>
#include <cstring>

// Softimage PIC: a 104-byte big-endian header, then up to four channel packets (8 or 16-bit samples, uncompressed, pure or mixed run-length);
// every scanline holds each packet's part of the full width in turn, the top row first; read as OpenImageIO's softimage reader does
namespace iv::fmt
{
	namespace
	{
		struct Packet
		{
			int bytes = 1, type = 0, count = 0;
			int chans[4] = {};   // 0 R, 1 G, 2 B, 3 A, in stored order
		};

		struct Info
		{
			int w = 0, h = 0, channels = 3, bits = 8, count = 0, seen = 0;
			Packet packets[4];
			size_t dataOffset = 0;
		};

		std::string Parse(const uint8_t* d, size_t size, Info& t)
		{
			if (size < 104 || Be32(d) != 0x5380F634)
				return "not a Softimage PIC file";
			t.w = Be16(d + 92), t.h = Be16(d + 94);
			if (t.w == 0 || t.h == 0)
				return "empty Softimage PIC image";
			size_t o = 104;
			for (bool chained = true; chained; o += 4)
			{
				if (t.count == 4)
					return "too many Softimage PIC channel packets";
				if (o + 4 > size)
					return "truncated Softimage PIC header";
				Packet& k = t.packets[t.count++];
				chained = d[o] != 0;
				if (d[o + 1] != 8 && d[o + 1] != 16)
					return "unsupported Softimage PIC bit depth";
				k.bytes = d[o + 1] / 8, k.type = d[o + 2] & 3;
				if (k.type == 3)
					return "unsupported Softimage PIC encoding";
				for (int c = 0; c < 4; ++c)
					if (d[o + 3] & (0x80 >> c))
						k.chans[k.count++] = c;
				if (k.count == 0)
					return "Softimage PIC channel packet without channels";
				t.seen |= d[o + 3];
				if (k.bytes == 2)
					t.bits = 16;
			}
			t.channels = (t.seen & 0x10) ? 4 : 3;
			t.dataOffset = o;
			return {};
		}

		// the packet's channels of pixel s into n pixels from x, clipped to cols; 16-bit frames widen 8-bit samples by 257
		void Put(const Packet& k, const uint8_t* s, int x, int n, uint8_t* row, int cols, int stride, bool wide)
		{
			n = std::min(n, cols - x);
			uint8_t* o = row + (size_t)x * stride;
			for (int c = 0; c < k.count; ++c)
			{
				const int ch = k.chans[c];
				if (wide)
				{
					const uint16_t v = k.bytes == 2 ? Be16(s + 2 * c) : uint16_t(s[c] * 257);
					for (int i = 0; i < n; ++i)
						memcpy(o + (size_t)i * stride + ch * 2, &v, 2);
				}
				else
					for (int i = 0; i < n; ++i)
						o[(size_t)i * stride + ch] = s[c];
			}
		}

		// one packet's part of a scanline from p, written to row (its first cols pixels) when row is set; where it ends, null when cut short or malformed
		const uint8_t* Run(const uint8_t* p, const uint8_t* end, const Packet& k, int w, uint8_t* row, int cols, int stride, bool wide)
		{
			const size_t pix = (size_t)k.count * k.bytes;
			if (k.type == 0)
			{
				if ((size_t)(end - p) < pix * w)
					return nullptr;
				for (int x = 0; row && x < cols; ++x)
					Put(k, p + x * pix, x, 1, row, cols, stride, wide);
				return p + pix * w;
			}
			for (int x = 0; x < w;)
			{
				if (p >= end)
					return nullptr;
				int n = *p++;
				// mixed: below 128 n + 1 raw pixels, 128 a 16-bit count, above n - 127 repeats; pure: n repeats
				if (k.type == 2 && n < 128)
				{
					n = std::min(n + 1, w - x);
					if ((size_t)(end - p) < pix * n)
						return nullptr;
					for (int i = 0; row && i < n && x + i < cols; ++i)
						Put(k, p + i * pix, x + i, 1, row, cols, stride, wide);
					p += pix * n, x += n;
					continue;
				}
				if (k.type == 2 && n == 128)
				{
					if (end - p < 2)
						return nullptr;
					n = Be16(p), p += 2;
				}
				else if (k.type == 2)
					n -= 127;
				if (n == 0 || (size_t)(end - p) < pix)
					return nullptr;
				n = std::min(n, w - x);
				if (row && x < cols)
					Put(k, p, x, n, row, cols, stride, wide);
				p += pix, x += n;
			}
			return p;
		}
	}

	std::string ProbePic(const uint8_t* data, size_t size, Probe& out)
	{
		Info t;
		const std::string err = Parse(data, size, t);
		out.w = t.w, out.h = t.h, out.bits = t.bits, out.channels = t.channels;
		return err;
	}

	std::string DecodePic(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Info t;
		if (std::string err = Parse(data, size, t); !err.empty())
			return err;
		const bool wide = t.bits == 16, fill = (t.seen & 0xE0) != 0xE0;
		const int stride = t.channels * (wide ? 2 : 1);
		const int rows = std::min(t.h, maxH), cols = std::min({ t.w, maxW, (int)(pitch / stride) });
		// rows follow each other with no index: find where each starts without writing pixels, then decode them in parallel
		std::vector<const uint8_t*> starts(rows);
		const uint8_t* p = data + t.dataOffset;
		const uint8_t* end = data + size;
		for (int y = 0; y < rows; ++y)
		{
			starts[y] = p;
			for (int k = 0; k < t.count; ++k)
				if (!(p = Run(p, end, t.packets[k], t.w, nullptr, 0, 0, false)))
					return "truncated Softimage PIC data";
		}
		par.For(rows, 32, [&](int64_t r0, int64_t r1, int)
		{
			for (int64_t y = r0; y < r1; ++y)
			{
				uint8_t* row = dst + (size_t)y * pitch;
				if (fill)
					memset(row, 0, (size_t)cols * stride);
				const uint8_t* q = starts[y];
				for (int k = 0; k < t.count; ++k)
					q = Run(q, end, t.packets[k], t.w, row, cols, stride, wide);
			}
		});
		return {};
	}
}
