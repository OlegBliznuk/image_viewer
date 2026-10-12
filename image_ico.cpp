#include "image_formats.h"

#include <stb_image.h>

#include <algorithm>
#include <cstring>

// ICO / CUR: every directory entry is a PNG or a headerless BMP (BITMAPINFOHEADER with the height doubled, colours, then a 1-bit AND mask)
namespace iv::fmt
{
	namespace
	{
		struct Entry
		{
			const uint8_t* data = nullptr;
			size_t size = 0;
			int w = 0, h = 0, bpp = 0;
			bool png = false;
		};

		// readable entries, largest and deepest first; the directory's own sizes are ignored (one byte, 0 = 256)
		std::string Parse(const uint8_t* d, size_t size, std::vector<Entry>& out)
		{
			out.clear();
			if (size < 6 || Le16(d) != 0 || (Le16(d + 2) != 1 && Le16(d + 2) != 2))
				return "ico: bad header";
			const int count = Le16(d + 4);
			if (count == 0 || 6 + 16 * (size_t)count > size)
				return "ico: bad directory";
			for (int i = 0; i < count; ++i)
			{
				const uint8_t* de = d + 6 + 16 * i;
				const size_t bytes = Le32(de + 8), offset = Le32(de + 12);
				if (offset >= size || bytes > size - offset || bytes < 40)
					continue;
				Entry e;
				e.data = d + offset;
				e.size = bytes;
				e.png = memcmp(e.data, "\x89PNG\r\n\x1a\n", 8) == 0;
				if (e.png)
				{
					const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
					e.w = (int)Be32(e.data + 16), e.h = (int)Be32(e.data + 20), e.bpp = e.data[24] * (e.data[25] < 7 ? channels[e.data[25]] : 0);
				}
				else
				{
					e.w = (int32_t)Le32(e.data + 4), e.h = (int32_t)Le32(e.data + 8) / 2, e.bpp = Le16(e.data + 14);
					const uint32_t header = Le32(e.data);
					if (header < 40 || header >= bytes || Le32(e.data + 16) != 0 || (e.bpp != 1 && e.bpp != 4 && e.bpp != 8 && e.bpp != 24 && e.bpp != 32))
						continue;
				}
				if (e.w > 0 && e.h > 0 && e.w <= 16384 && e.h <= 16384)
					out.push_back(e);
			}
			if (out.empty())
				return "ico: no readable entry";
			std::stable_sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.w * a.h != b.w * b.h ? a.w * a.h > b.w * b.h : a.bpp > b.bpp; });
			return {};
		}

		// the entry alone, tightly packed RGBA
		std::string DecodeEntry(const Entry& e, std::vector<uint8_t>& rgba)
		{
			rgba.assign((size_t)e.w * e.h * 4, 0);
			if (e.png)
			{
				int w = 0, h = 0, n = 0;
				stbi_uc* px = stbi_load_from_memory(e.data, (int)e.size, &w, &h, &n, 4);
				if (!px)
					return std::string("ico: ") + stbi_failure_reason();
				CopyRows(px, w, h, rgba.data(), (size_t)e.w * 4, e.w, e.h);
				stbi_image_free(px);
				return {};
			}
			const uint8_t* end = e.data + e.size;
			const uint32_t used = Le32(e.data + 32);
			const size_t colors = e.bpp > 8 ? 0 : used && used < (1u << e.bpp) ? used : (size_t)1 << e.bpp;
			const uint8_t* palette = e.data + Le32(e.data);
			const uint8_t* pixels = palette + colors * 4;
			const size_t stride = ((size_t)e.w * e.bpp + 31) / 32 * 4, maskStride = ((size_t)e.w + 31) / 32 * 4;
			if (pixels > end || stride * e.h > (size_t)(end - pixels))
				return "ico: truncated entry";
			const uint8_t* mask = pixels + stride * e.h;
			if (maskStride * e.h > (size_t)(end - mask))
				mask = nullptr;   // some 32 bpp entries leave the AND mask out
			bool alpha = false;
			for (int row = 0; row < e.h; ++row)
			{
				const uint8_t* s = pixels + stride * row;
				uint8_t* o = rgba.data() + (size_t)(e.h - 1 - row) * e.w * 4;
				for (int x = 0; x < e.w; ++x, o += 4)
				{
					if (e.bpp == 32)
						o[0] = s[x * 4 + 2], o[1] = s[x * 4 + 1], o[2] = s[x * 4], o[3] = s[x * 4 + 3], alpha |= o[3] != 0;
					else if (e.bpp == 24)
						o[0] = s[x * 3 + 2], o[1] = s[x * 3 + 1], o[2] = s[x * 3], o[3] = 255;
					else
					{
						const int bit = x * e.bpp;
						const size_t index = (s[bit >> 3] >> (8 - e.bpp - (bit & 7))) & ((1 << e.bpp) - 1);
						if (index < colors)
							o[0] = palette[index * 4 + 2], o[1] = palette[index * 4 + 1], o[2] = palette[index * 4];
						o[3] = 255;
					}
				}
			}
			// the AND mask holds the transparency unless a 32 bpp entry carries its own alpha
			if (e.bpp != 32 || !alpha)
				for (int row = 0; row < e.h; ++row)
				{
					uint8_t* o = rgba.data() + (size_t)(e.h - 1 - row) * e.w * 4;
					for (int x = 0; x < e.w; ++x)
						o[x * 4 + 3] = mask && (mask[maskStride * row + x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
				}
			return {};
		}

		void Canvas(const std::vector<Entry>& entries, int& w, int& h)
		{
			w = h = 0;
			for (const Entry& e : entries)
				w = std::max(w, e.w), h = std::max(h, e.h);
		}

		// the entry centred on a cleared w x h canvas
		std::string Place(const Entry& e, int w, int h, uint8_t* dst, size_t pitch, int maxW, int maxH)
		{
			for (int y = 0; y < std::min(h, maxH); ++y)
				memset(dst + (size_t)y * pitch, 0, (size_t)std::min(w, maxW) * 4);
			std::vector<uint8_t> rgba;
			if (std::string err = DecodeEntry(e, rgba); !err.empty())
				return err;
			const int ox = (w - e.w) / 2, oy = (h - e.h) / 2;
			CopyRows(rgba.data(), e.w, e.h, dst + (size_t)oy * pitch + (size_t)ox * 4, pitch, maxW - ox, maxH - oy);
			return {};
		}
	}

	std::string ProbeIco(const uint8_t* data, size_t size, Probe& out)
	{
		std::vector<Entry> entries;
		if (std::string err = Parse(data, size, entries); !err.empty())
			return err;
		Canvas(entries, out.w, out.h);
		for (const Entry& e : entries)
			out.notes.push_back(std::to_string(e.w) + " x " + std::to_string(e.h) + (e.png ? " PNG" : ", " + std::to_string(e.bpp) + " bpp"));
		return {};
	}

	std::string DecodeIco(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel&)
	{
		std::vector<Entry> entries;
		if (std::string err = Parse(data, size, entries); !err.empty())
			return err;
		int w = 0, h = 0;
		Canvas(entries, w, h);
		return Place(entries[0], w, h, dst, pitch, maxW, maxH);
	}

	std::string DecodeIcoFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames)
	{
		std::vector<Entry> entries;
		if (std::string err = Parse(data, size, entries); !err.empty())
			return err;
		Canvas(entries, w, h);
		count = (int)entries.size();
		const size_t frameBytes = (size_t)w * h * 4;
		frames.resize(frameBytes * count);
		for (int i = 0; i < count; ++i)
			if (std::string err = Place(entries[i], w, h, frames.data() + frameBytes * i, (size_t)w * 4, w, h); !err.empty())
				return err;
		return {};
	}
}
