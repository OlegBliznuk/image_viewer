#include "image_formats.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>

// CR3 (Canon RAW 3, ISO base media file): the full-size JPEG preview of the CRAW track that carries it, turned by the EXIF orientation of CMT1
namespace iv::fmt
{
	namespace
	{
		struct File
		{
			FILE* f = nullptr;
			explicit File(const std::wstring& path) : f(_wfopen(path.c_str(), L"rb")) {}
			~File() { if (f) fclose(f); }
			bool Read(uint64_t offset, void* dst, size_t size) { return _fseeki64(f, (int64_t)offset, SEEK_SET) == 0 && fread(dst, 1, size, f) == size; }
			uint64_t Size() { _fseeki64(f, 0, SEEK_END); return (uint64_t)_ftelli64(f); }
		};

		struct Preview
		{
			uint64_t offset = 0;
			uint64_t size = 0;
			int orientation = 1;
		};

		uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
		constexpr uint32_t Tag(const char (&s)[5]) { return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; }
		constexpr uint8_t kCanonUuid[16] = { 0x85, 0xc0, 0xb6, 0x87, 0x82, 0x0f, 0x11, 0xe0, 0x81, 0x11, 0xf4, 0xce, 0x46, 0x2b, 0x6a, 0x48 };

		template <class F> void ForEachBox(const uint8_t* p, size_t begin, size_t end, F&& fn)
		{
			for (size_t off = begin; off + 8 <= end;)
			{
				uint64_t size = Be32(p + off);
				size_t body = off + 8;
				if (size == 1 && off + 16 <= end)
					size = Be64(p + off + 8), body = off + 16;
				else if (size == 0)
					size = end - off;
				if (size < body - off || size > end - off)
					return;
				fn(Be32(p + off + 4), body, off + (size_t)size);
				off += (size_t)size;
			}
		}

		bool Child(const uint8_t* p, size_t& begin, size_t& end, uint32_t type)
		{
			bool hit = false;
			ForEachBox(p, begin, end, [&](uint32_t t, size_t b, size_t e) { if (!hit && t == type) begin = b, end = e, hit = true; });
			return hit;
		}

		int TiffOrientation(const uint8_t* t, size_t n)
		{
			if (n < 8)
				return 1;
			const bool le = t[0] == 'I';
			auto u16 = [&](size_t o) { return le ? t[o] | t[o + 1] << 8 : t[o] << 8 | t[o + 1]; };
			const size_t ifd = le ? t[4] | t[5] << 8 | t[6] << 16 | (size_t)t[7] << 24 : Be32(t + 4);
			if (ifd + 2 > n)
				return 1;
			const int count = u16(ifd);
			for (int i = 0; i < count && ifd + 2 + 12 * (size_t)(i + 1) <= n; ++i)
			{
				const size_t e = ifd + 2 + 12 * (size_t)i;
				if (u16(e) == 0x112)
					return std::clamp(u16(e + 8), 1, 8);
			}
			return 1;
		}

		// the trak whose CRAW sample entry carries a JPEG box holds the preview as its only sample
		bool JpegTrack(const uint8_t* p, size_t b, size_t e, Preview& out)
		{
			if (!Child(p, b, e, Tag("mdia")) || !Child(p, b, e, Tag("minf")) || !Child(p, b, e, Tag("stbl")))
				return false;
			bool jpeg = false;
			uint64_t offset = 0, size = 0;
			ForEachBox(p, b, e, [&](uint32_t type, size_t body, size_t end)
			{
				if (type == Tag("stsd") && end - body >= 16 && Be32(p + body + 12) == Tag("CRAW"))
				{
					// Canon's entry: the 86-byte visual sample entry and 4 more bytes, then child boxes (JPEG for the preview, CMP1 for raw data)
					const size_t entry = body + 8, entryEnd = std::min(end, entry + (size_t)Be32(p + entry));
					ForEachBox(p, entry + 90, entryEnd, [&](uint32_t t, size_t, size_t) { jpeg |= t == Tag("JPEG"); });
				}
				else if (type == Tag("stsz") && end - body >= 12)
					size = Be32(p + body + 4) ? Be32(p + body + 4) : end - body >= 16 ? Be32(p + body + 12) : 0;
				else if (type == Tag("co64") && end - body >= 16)
					offset = Be64(p + body + 8);
				else if (type == Tag("stco") && end - body >= 12)
					offset = Be32(p + body + 8);
			});
			if (!jpeg || !offset || !size)
				return false;
			out.offset = offset;
			out.size = size;
			return true;
		}

		// the moov box, whole: the track tables and Canon's uuid box with the CMT metadata
		std::string ReadMoov(File& file, std::vector<uint8_t>& moov)
		{
			if (!file.f)
				return "cannot open file";
			const uint64_t fileSize = file.Size();
			for (uint64_t off = 0; off + 8 <= fileSize && moov.empty();)
			{
				uint8_t h[16];
				if (!file.Read(off, h, fileSize - off >= 16 ? 16 : 8))
					return "cr3: read error";
				uint64_t size = Be32(h);
				if (size == 1)
					size = Be64(h + 8);
				else if (size == 0)
					size = fileSize - off;
				if (off == 0 && (Be32(h + 4) != Tag("ftyp") || Be32(h + 8) != Tag("crx ")))
					return "cr3: not a Canon CR3 file";
				if (size < 8 || size > fileSize - off)
					return "cr3: bad box";
				if (Be32(h + 4) == Tag("moov"))
				{
					if (size > (64u << 20))
						return "cr3: moov box too large";
					moov.resize((size_t)size);
					if (!file.Read(off, moov.data(), moov.size()))
						return "cr3: read error";
				}
				off += size;
			}
			return moov.empty() ? "cr3: no moov box" : std::string();
		}

		std::string Locate(File& file, Preview& out)
		{
			std::vector<uint8_t> moov;
			if (std::string err = ReadMoov(file, moov); !err.empty())
				return err;
			const uint64_t fileSize = file.Size();
			const uint8_t* p = moov.data();
			bool found = false;
			ForEachBox(p, Be32(p) == 1 ? 16 : 8, moov.size(), [&](uint32_t type, size_t body, size_t end)
			{
				if (type == Tag("uuid") && end - body >= 16 && memcmp(p + body, kCanonUuid, 16) == 0)
					ForEachBox(p, body + 16, end, [&](uint32_t t, size_t b, size_t e) { if (t == Tag("CMT1")) out.orientation = TiffOrientation(p + b, e - b); });
				else if (type == Tag("trak") && !found)
					found = JpegTrack(p, body, end, out);
			});
			if (!found)
				return "cr3: no JPEG preview";
			if (out.offset + out.size > fileSize || out.size > (512u << 20))
				return "cr3: preview out of the file";
			return {};
		}

		// EXIF orientation: displayed pixel (x, y) is the stored pixel at origin + x * dx + y * dy
		void Orient(const uint32_t* src, int w, int h, int orientation, uint8_t* dst, size_t pitch, int maxW, int maxH)
		{
			const int64_t W = w, last = (int64_t)(h - 1) * W;
			struct Map { int64_t origin, dx, dy; };
			const Map maps[9] = { {}, { 0, 1, W }, { W - 1, -1, W }, { last + W - 1, -1, -W }, { last, 1, -W }, { 0, W, 1 }, { last, -W, 1 }, { last + W - 1, -W, -1 }, { W - 1, W, -1 } };
			const Map& m = maps[orientation];
			const int dw = std::min(orientation >= 5 ? h : w, maxW), dh = std::min(orientation >= 5 ? w : h, maxH);
			constexpr int kTile = 64;
			for (int ty = 0; ty < dh; ty += kTile)
				for (int tx = 0; tx < dw; tx += kTile)
					for (int y = ty; y < std::min(ty + kTile, dh); ++y)
					{
						uint32_t* row = (uint32_t*)(dst + (size_t)y * pitch);
						const uint32_t* s = src + m.origin + y * m.dy;
						for (int x = tx; x < std::min(tx + kTile, dw); ++x)
							row[x] = s[x * m.dx];
					}
		}
	}

	std::string ProbeCr3(const std::wstring& path, Probe& out)
	{
		File file(path);
		Preview pv;
		if (std::string err = Locate(file, pv); !err.empty())
			return err;
		std::vector<uint8_t> jpeg((size_t)std::min<uint64_t>(pv.size, 64 * 1024));
		if (!file.Read(pv.offset, jpeg.data(), jpeg.size()))
			return "cr3: read error";
		std::string err = ProbeJpeg(jpeg.data(), jpeg.size(), out);
		if (!err.empty() && jpeg.size() < pv.size)
		{
			jpeg.resize((size_t)pv.size);
			err = file.Read(pv.offset, jpeg.data(), jpeg.size()) ? ProbeJpeg(jpeg.data(), jpeg.size(), out) : "cr3: read error";
		}
		if (pv.orientation >= 5)
			std::swap(out.w, out.h);
		out.yuv.orientation = (uint8_t)pv.orientation;
		return err;
	}

	std::string DecodeCr3Yuv(const std::wstring& path, uint8_t* frame, uint64_t capacity, int dispW, int dispH)
	{
		File file(path);
		Preview pv;
		if (std::string err = Locate(file, pv); !err.empty())
			return err;
		std::vector<uint8_t> jpeg((size_t)pv.size);
		if (!file.Read(pv.offset, jpeg.data(), jpeg.size()))
			return "cr3: read error";
		return DecodeJpegYuv(jpeg.data(), jpeg.size(), frame, capacity, dispW, dispH, pv.orientation);
	}

	std::string DecodeCr3(const std::wstring& path, uint8_t* dst, size_t pitch, int maxW, int maxH)
	{
		File file(path);
		Preview pv;
		if (std::string err = Locate(file, pv); !err.empty())
			return err;
		std::vector<uint8_t> jpeg((size_t)pv.size);
		if (!file.Read(pv.offset, jpeg.data(), jpeg.size()))
			return "cr3: read error";
		if (pv.orientation == 1)
			return DecodeJpeg(jpeg.data(), jpeg.size(), dst, pitch, maxW, maxH);
		Probe stored;
		if (std::string err = ProbeJpeg(jpeg.data(), jpeg.size(), stored); !err.empty())
			return err;
		auto px = std::make_unique_for_overwrite<uint32_t[]>((size_t)stored.w * stored.h);
		if (std::string err = DecodeJpeg(jpeg.data(), jpeg.size(), (uint8_t*)px.get(), (size_t)stored.w * 4, stored.w, stored.h); !err.empty())
			return err;
		Orient(px.get(), stored.w, stored.h, pv.orientation, dst, pitch, maxW, maxH);
		return {};
	}

	std::string ReadCr3Metadata(const std::wstring& path, std::vector<uint8_t> cmt[4])
	{
		File file(path);
		std::vector<uint8_t> moov;
		if (std::string err = ReadMoov(file, moov); !err.empty())
			return err;
		const uint8_t* p = moov.data();
		const uint32_t names[4] = { Tag("CMT1"), Tag("CMT2"), Tag("CMT3"), Tag("CMT4") };
		ForEachBox(p, Be32(p) == 1 ? 16 : 8, moov.size(), [&](uint32_t type, size_t body, size_t end)
		{
			if (type == Tag("uuid") && end - body >= 16 && memcmp(p + body, kCanonUuid, 16) == 0)
				ForEachBox(p, body + 16, end, [&](uint32_t t, size_t b, size_t e) { for (int k = 0; k < 4; ++k) if (t == names[k]) cmt[k].assign(p + b, p + e); });
		});
		return {};
	}
}
