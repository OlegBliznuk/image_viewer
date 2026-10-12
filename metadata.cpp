#include "metadata.h"
#include "image_formats.h"

#include <OpenEXR/openexr.h>
#include <libheif/heif.h>
#include <libdeflate.h>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <span>

namespace iv
{
	namespace
	{
		using fmt::Be32;
		using fmt::Le16;
		using fmt::Le32;

		constexpr size_t kMaxText = 4000;

		struct Sink
		{
			std::vector<MetaEntry>& out;
			void Section(std::string name) { out.push_back({ std::move(name), {}, true }); }
			void Add(std::string key, std::string value)
			{
				if (value.empty())
					return;
				if (value.size() > kMaxText)
					value = value.substr(0, kMaxText) + " ...";
				out.push_back({ std::move(key), std::move(value), false });
			}
		};

		template <class... A> std::string Fmt(const char* f, A... a)
		{
			char b[512];
			snprintf(b, sizeof(b), f, a...);
			return b;
		}

		std::string Number(double v) { return Fmt("%.6g", v); }

		bool ValidUtf8(const uint8_t* p, size_t n)
		{
			for (size_t i = 0; i < n;)
			{
				const uint8_t c = p[i];
				const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
				if (!len || i + len > n)
					return false;
				for (size_t k = 1; k < len; ++k)
					if ((p[i + k] & 0xC0) != 0x80)
						return false;
				i += len;
			}
			return true;
		}

		// stored text up to its first NUL: UTF-8 kept, anything else read as Latin-1, control characters as spaces, trailing blanks trimmed
		std::string Text(const void* data, size_t n)
		{
			const uint8_t* p = (const uint8_t*)data;
			n = std::find(p, p + n, 0) - p;
			std::string s;
			const bool utf8 = ValidUtf8(p, n);
			for (size_t i = 0; i < n; ++i)
			{
				const uint8_t c = p[i];
				if (c < 0x20 && c != '\n')
					s += ' ';
				else if (c < 0x80 || utf8)
					s += (char)c;
				else
					s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
			}
			while (!s.empty() && (s.back() == ' ' || s.back() == '\n'))
				s.pop_back();
			return s.substr(std::min(s.find_first_not_of(' '), s.size()));
		}

		std::string Bytes(uint64_t n)
		{
			std::string digits = std::to_string(n);
			for (int i = (int)digits.size() - 3; i > 0; i -= 3)
				digits.insert((size_t)i, ",");
			return n < 1024 ? digits + " bytes" : n < (1u << 20) ? Fmt("%s bytes (%.1f KB)", digits.c_str(), n / 1024.0) : Fmt("%s bytes (%.1f MB)", digits.c_str(), n / 1048576.0);
		}

		std::string LocalTime(const FILETIME& ft)
		{
			FILETIME local;
			SYSTEMTIME st;
			if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st))
				return {};
			return Fmt("%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
		}

		struct Named
		{
			int value;
			const char* name;
		};

		std::string Name(std::span<const Named> table, int value)
		{
			for (const Named& n : table)
				if (n.value == value)
					return n.name;
			return std::to_string(value);
		}

		// the description of an ICC profile ('desc' tag, v2 text or v4 multi-localised)
		std::string IccName(const uint8_t* p, size_t n)
		{
			if (n < 132)
				return {};
			const uint32_t count = Be32(p + 128);
			for (uint32_t i = 0; i < count && 132 + 12 * (size_t)(i + 1) <= n; ++i)
			{
				const uint8_t* e = p + 132 + 12 * (size_t)i;
				if (memcmp(e, "desc", 4) != 0)
					continue;
				const size_t off = Be32(e + 4), size = Be32(e + 8);
				if (off + size > n || size < 12)
					return {};
				const uint8_t* t = p + off;
				if (memcmp(t, "desc", 4) == 0)
					return Text(t + 12, std::min<size_t>(Be32(t + 8), size - 12));
				if (memcmp(t, "mluc", 4) == 0 && size >= 28 && Be32(t + 8) > 0)
				{
					const size_t len = Be32(t + 20), at = Be32(t + 24);
					if (at + len > size)
						return {};
					std::string s;
					for (size_t k = 0; k + 1 < len; k += 2)
					{
						const uint32_t c = (uint32_t)t[at + k] << 8 | t[at + k + 1];
						if (c < 0x80)
							s += (char)c;
						else if (c < 0x800)
							s += (char)(0xC0 | c >> 6), s += (char)(0x80 | (c & 0x3F));
						else
							s += (char)(0xE0 | c >> 12), s += (char)(0x80 | ((c >> 6) & 0x3F)), s += (char)(0x80 | (c & 0x3F));
					}
					return Text(s.data(), s.size());
				}
			}
			return {};
		}

		// zlib data (PNG zTXt / iTXt / iCCP), at most 16 MB out
		std::vector<uint8_t> Inflate(const uint8_t* p, size_t n)
		{
			std::vector<uint8_t> out;
			libdeflate_decompressor* d = libdeflate_alloc_decompressor();
			if (!d)
				return out;
			for (size_t cap = std::max<size_t>(n * 4, 4096); cap <= (16u << 20); cap *= 4)
			{
				out.resize(cap);
				size_t got = 0;
				const libdeflate_result r = libdeflate_zlib_decompress(d, p, n, out.data(), cap, &got);
				if (r == LIBDEFLATE_SUCCESS)
				{
					out.resize(got);
					break;
				}
				out.clear();
				if (r != LIBDEFLATE_INSUFFICIENT_SPACE)
					break;
			}
			libdeflate_free_decompressor(d);
			return out;
		}

		// ---- TIFF structures: TIFF files, EXIF blocks, CR3 CMT boxes ----

		struct Tiff
		{
			const uint8_t* d = nullptr;
			size_t n = 0;
			bool le = true;

			bool Init(const uint8_t* data, size_t size)
			{
				d = data, n = size;
				if (size < 8 || !((d[0] == 'I' && d[1] == 'I') || (d[0] == 'M' && d[1] == 'M')))
					return false;
				le = d[0] == 'I';
				return U16(2) == 42;
			}
			uint32_t U16(size_t o) const { return o + 2 <= n ? (le ? Le16(d + o) : uint32_t(d[o] << 8 | d[o + 1])) : 0; }
			uint32_t U32(size_t o) const { return o + 4 <= n ? (le ? Le32(d + o) : Be32(d + o)) : 0; }
		};

		struct Entry
		{
			uint32_t tag, type, count;
			size_t at;   // offset of the value bytes
		};

		size_t TypeSize(uint32_t type)
		{
			static constexpr uint8_t sizes[14] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4 };
			return type < 14 ? sizes[type] : 0;
		}

		std::vector<Entry> ReadIfd(const Tiff& t, size_t ifd, size_t& next)
		{
			std::vector<Entry> out;
			next = 0;
			if (ifd < 8 || ifd + 2 > t.n)
				return out;
			const uint32_t count = t.U16(ifd);
			for (uint32_t i = 0; i < count && ifd + 2 + 12 * (size_t)(i + 1) <= t.n; ++i)
			{
				const size_t e = ifd + 2 + 12 * (size_t)i;
				Entry x = { t.U16(e), t.U16(e + 2), t.U32(e + 4), 0 };
				const uint64_t bytes = (uint64_t)TypeSize(x.type) * x.count;
				x.at = bytes <= 4 ? e + 8 : t.U32(e + 8);
				if (TypeSize(x.type) && bytes <= t.n && x.at + bytes <= t.n)
					out.push_back(x);
			}
			next = t.U32(ifd + 2 + 12 * (size_t)count);
			return out;
		}

		int64_t Int(const Tiff& t, const Entry& e, uint32_t i)
		{
			const size_t o = e.at + (size_t)i * TypeSize(e.type);
			switch (e.type)
			{
			case 1: case 7: return t.d[o];
			case 6: return (int8_t)t.d[o];
			case 3: return t.U16(o);
			case 8: return (int16_t)t.U16(o);
			case 4: case 13: return t.U32(o);
			case 9: return (int32_t)t.U32(o);
			default: return 0;
			}
		}

		double Real(const Tiff& t, const Entry& e, uint32_t i)
		{
			const size_t o = e.at + (size_t)i * TypeSize(e.type);
			if (e.type == 5 || e.type == 10)
			{
				const double num = e.type == 5 ? (double)t.U32(o) : (double)(int32_t)t.U32(o), den = e.type == 5 ? (double)t.U32(o + 4) : (double)(int32_t)t.U32(o + 4);
				return den != 0.0 ? num / den : 0.0;
			}
			if (e.type == 11)
			{
				const uint32_t u = t.U32(o);
				float f;
				memcpy(&f, &u, 4);
				return f;
			}
			if (e.type == 12)
			{
				uint64_t u = (uint64_t)t.U32(o) | (uint64_t)t.U32(o + 4) << 32;
				if (!t.le)
					u = (uint64_t)t.U32(o) << 32 | t.U32(o + 4);
				double v;
				memcpy(&v, &u, 8);
				return v;
			}
			return (double)Int(t, e, i);
		}

		enum Kind : uint8_t { kPlain, kSkip, kDate, kSize, kExifIfd, kGpsIfd, kIcc, kExposure, kFNumber, kMm, kEv, kMetres, kVersion, kComment, kGpsCoord, kGpsTime, kOrientation, kResUnit, kCompression, kPhotometric, kPlanar, kPredictor, kSampleFormat, kProgram, kMetering, kFlash, kColorSpace, kWhiteBalance, kExposureMode, kScene, kLight };

		struct TagInfo
		{
			uint16_t tag;
			const char* name;
			Kind kind;
		};

		constexpr TagInfo kTags[] = {
			{ 0x00FE, "Subfile type", kPlain }, { 0x0100, "Image width", kPlain }, { 0x0101, "Image height", kPlain }, { 0x0102, "Bits per sample", kPlain },
			{ 0x0103, "Compression", kCompression }, { 0x0106, "Photometric", kPhotometric }, { 0x010D, "Document name", kPlain }, { 0x010E, "Description", kPlain },
			{ 0x010F, "Camera make", kPlain }, { 0x0110, "Camera model", kPlain }, { 0x0111, "", kSkip }, { 0x0112, "Orientation", kOrientation },
			{ 0x0115, "Samples per pixel", kPlain }, { 0x0116, "Rows per strip", kPlain }, { 0x0117, "", kSkip }, { 0x011A, "X resolution", kPlain },
			{ 0x011B, "Y resolution", kPlain }, { 0x011C, "Planar configuration", kPlanar }, { 0x0128, "Resolution unit", kResUnit }, { 0x0131, "Software", kPlain },
			{ 0x0132, "Date and time", kDate }, { 0x013B, "Artist", kPlain }, { 0x013C, "Host computer", kPlain }, { 0x013D, "Predictor", kPredictor },
			{ 0x013E, "White point", kPlain }, { 0x013F, "Primary chromaticities", kPlain }, { 0x0140, "", kSkip }, { 0x0142, "Tile width", kPlain },
			{ 0x0143, "Tile height", kPlain }, { 0x0144, "", kSkip }, { 0x0145, "", kSkip }, { 0x014A, "", kSkip }, { 0x0152, "Extra samples", kPlain },
			{ 0x0153, "Sample format", kSampleFormat }, { 0x015B, "", kSkip }, { 0x0201, "", kSkip }, { 0x0202, "", kSkip }, { 0x0211, "", kSkip }, { 0x0212, "", kSkip },
			{ 0x0213, "", kSkip }, { 0x0214, "", kSkip }, { 0x02BC, "XMP", kSize }, { 0x8298, "Copyright", kPlain }, { 0x83BB, "IPTC", kSize },
			{ 0x8649, "Photoshop resources", kSize }, { 0x8769, "", kExifIfd }, { 0x8773, "ICC profile", kIcc }, { 0x8825, "", kGpsIfd }, { 0x935C, "", kSkip },
			{ 0xC4A5, "", kSkip }, { 0xC612, "DNG version", kPlain }, { 0x8214, "Full image width", kPlain }, { 0x8215, "Full image height", kPlain },
			{ 0x8216, "Texture format", kPlain }, { 0x8217, "Wrap modes", kPlain }, { 0x8218, "Field of view cotangent", kPlain }, { 0x8219, "World to screen matrix", kPlain },
			{ 0x821A, "World to camera matrix", kPlain },
			{ 0x829A, "Exposure time", kExposure }, { 0x829D, "F-number", kFNumber }, { 0x8822, "Exposure program", kProgram }, { 0x8827, "ISO", kPlain },
			{ 0x8830, "Sensitivity type", kPlain }, { 0x8832, "Recommended exposure index", kPlain }, { 0x9000, "Exif version", kVersion }, { 0x9003, "Date taken", kDate },
			{ 0x9004, "Date digitized", kDate }, { 0x9010, "Time offset", kPlain }, { 0x9011, "Time offset (taken)", kPlain }, { 0x9012, "Time offset (digitized)", kPlain },
			{ 0x9101, "", kSkip }, { 0x9102, "Compressed bits per pixel", kPlain }, { 0x9201, "Shutter speed (APEX)", kPlain }, { 0x9202, "Aperture (APEX)", kPlain },
			{ 0x9203, "Brightness (APEX)", kPlain }, { 0x9204, "Exposure bias", kEv }, { 0x9205, "Max aperture (APEX)", kPlain }, { 0x9206, "Subject distance", kMetres },
			{ 0x9207, "Metering mode", kMetering }, { 0x9208, "Light source", kLight }, { 0x9209, "Flash", kFlash }, { 0x920A, "Focal length", kMm },
			{ 0x9214, "Subject area", kPlain }, { 0x927C, "Maker note", kSize }, { 0x9286, "User comment", kComment }, { 0x9290, "Subsecond time", kPlain },
			{ 0x9291, "Subsecond time (taken)", kPlain }, { 0x9292, "Subsecond time (digitized)", kPlain }, { 0xA000, "FlashPix version", kVersion },
			{ 0xA001, "Colour space", kColorSpace }, { 0xA002, "Pixel width", kPlain }, { 0xA003, "Pixel height", kPlain }, { 0xA004, "Related sound file", kPlain },
			{ 0xA005, "", kSkip }, { 0xA20E, "Focal plane X resolution", kPlain }, { 0xA20F, "Focal plane Y resolution", kPlain }, { 0xA210, "Focal plane resolution unit", kResUnit },
			{ 0xA217, "Sensing method", kPlain }, { 0xA300, "", kSkip }, { 0xA301, "", kSkip }, { 0xA401, "Custom rendered", kPlain }, { 0xA402, "Exposure mode", kExposureMode },
			{ 0xA403, "White balance", kWhiteBalance }, { 0xA404, "Digital zoom", kPlain }, { 0xA405, "Focal length (35 mm)", kMm }, { 0xA406, "Scene type", kScene },
			{ 0xA407, "Gain control", kPlain }, { 0xA408, "Contrast", kPlain }, { 0xA409, "Saturation", kPlain }, { 0xA40A, "Sharpness", kPlain }, { 0xA40C, "Subject distance range", kPlain },
			{ 0xA420, "Image unique ID", kPlain }, { 0xA430, "Camera owner", kPlain }, { 0xA431, "Body serial number", kPlain }, { 0xA432, "Lens specification", kPlain },
			{ 0xA433, "Lens make", kPlain }, { 0xA434, "Lens model", kPlain }, { 0xA435, "Lens serial number", kPlain }, { 0xA460, "Composite image", kPlain },
		};

		constexpr TagInfo kGpsTags[] = {
			{ 0x0000, "GPS version", kPlain }, { 0x0001, "", kSkip }, { 0x0002, "Latitude", kGpsCoord }, { 0x0003, "", kSkip }, { 0x0004, "Longitude", kGpsCoord },
			{ 0x0005, "", kSkip }, { 0x0006, "Altitude", kMetres }, { 0x0007, "GPS time (UTC)", kGpsTime }, { 0x0008, "Satellites", kPlain }, { 0x0009, "GPS status", kPlain },
			{ 0x000A, "Measure mode", kPlain }, { 0x000B, "Precision (DOP)", kPlain }, { 0x000C, "", kSkip }, { 0x000D, "Speed", kPlain }, { 0x000E, "", kSkip },
			{ 0x000F, "Track", kPlain }, { 0x0010, "", kSkip }, { 0x0011, "Image direction", kPlain }, { 0x0012, "Map datum", kPlain }, { 0x001B, "Processing method", kComment },
			{ 0x001D, "GPS date", kDate }, { 0x001F, "Horizontal error", kMetres },
		};

		constexpr Named kOrientations[] = { { 1, "normal" }, { 2, "mirrored" }, { 3, "rotated 180" }, { 4, "mirrored vertically" }, { 5, "mirrored, rotated 90 CCW" }, { 6, "rotated 90 CW" }, { 7, "mirrored, rotated 90 CW" }, { 8, "rotated 90 CCW" } };
		constexpr Named kResUnits[] = { { 1, "none" }, { 2, "inch" }, { 3, "centimetre" } };
		constexpr Named kCompressions[] = { { 1, "none" }, { 2, "CCITT RLE" }, { 3, "CCITT G3" }, { 4, "CCITT G4" }, { 5, "LZW" }, { 6, "old JPEG" }, { 7, "JPEG" }, { 8, "deflate" }, { 32773, "PackBits" }, { 32946, "deflate (old)" }, { 34925, "LZMA" }, { 50000, "Zstandard" }, { 50001, "WebP" }, { 34892, "lossy JPEG (DNG)" } };
		constexpr Named kPhotometrics[] = { { 0, "white is zero" }, { 1, "black is zero" }, { 2, "RGB" }, { 3, "palette" }, { 4, "transparency mask" }, { 5, "CMYK" }, { 6, "YCbCr" }, { 8, "CIE L*a*b*" }, { 32803, "colour filter array" }, { 34892, "linear raw" } };
		constexpr Named kPlanars[] = { { 1, "contiguous" }, { 2, "separate planes" } };
		constexpr Named kPredictors[] = { { 1, "none" }, { 2, "horizontal" }, { 3, "floating point" } };
		constexpr Named kSampleFormats[] = { { 1, "unsigned integer" }, { 2, "signed integer" }, { 3, "floating point" } };
		constexpr Named kPrograms[] = { { 0, "not defined" }, { 1, "manual" }, { 2, "program" }, { 3, "aperture priority" }, { 4, "shutter priority" }, { 5, "creative" }, { 6, "action" }, { 7, "portrait" }, { 8, "landscape" } };
		constexpr Named kMeterings[] = { { 0, "unknown" }, { 1, "average" }, { 2, "centre-weighted average" }, { 3, "spot" }, { 4, "multi-spot" }, { 5, "pattern" }, { 6, "partial" } };
		constexpr Named kColorSpaces[] = { { 1, "sRGB" }, { 2, "Adobe RGB" }, { 65535, "uncalibrated" } };
		constexpr Named kWhiteBalances[] = { { 0, "auto" }, { 1, "manual" } };
		constexpr Named kExposureModes[] = { { 0, "auto" }, { 1, "manual" }, { 2, "auto bracket" } };
		constexpr Named kScenes[] = { { 0, "standard" }, { 1, "landscape" }, { 2, "portrait" }, { 3, "night" } };
		constexpr Named kLights[] = { { 0, "unknown" }, { 1, "daylight" }, { 2, "fluorescent" }, { 3, "tungsten" }, { 4, "flash" }, { 9, "fine weather" }, { 10, "cloudy" }, { 11, "shade" }, { 17, "standard light A" }, { 18, "standard light B" }, { 19, "standard light C" }, { 20, "D55" }, { 21, "D65" }, { 22, "D75" }, { 23, "D50" }, { 255, "other" } };

		const TagInfo* Find(std::span<const TagInfo> table, uint32_t tag)
		{
			for (const TagInfo& t : table)
				if (t.tag == tag)
					return &t;
			return nullptr;
		}

		const Entry* FindEntry(const std::vector<Entry>& es, uint32_t tag)
		{
			for (const Entry& e : es)
				if (e.tag == tag)
					return &e;
			return nullptr;
		}

		std::string Plain(const Tiff& t, const Entry& e)
		{
			if (e.type == 2)
				return Text(t.d + e.at, e.count);
			if ((e.type == 7 || e.type == 1) && e.count > 16)
				return Bytes(e.count);
			std::string s;
			const uint32_t shown = std::min<uint32_t>(e.count, 16);
			for (uint32_t i = 0; i < shown; ++i)
				s += (i ? ", " : "") + (e.type == 5 || e.type == 10 || e.type == 11 || e.type == 12 ? Number(Real(t, e, i)) : std::to_string(Int(t, e, i)));
			return e.count > shown ? s + Fmt(" ... (%u values)", e.count) : s;
		}

		std::string Value(const Tiff& t, const Entry& e, Kind kind, const std::vector<Entry>& ifd)
		{
			switch (kind)
			{
			case kSize: return Bytes(e.count * TypeSize(e.type));
			case kDate:
			{
				// EXIF writes 2026:10:12 12:34:56
				std::string d = Plain(t, e);
				if (d.size() >= 10 && d[4] == ':' && d[7] == ':')
					d[4] = d[7] = '-';
				return d;
			}
			case kIcc: { const std::string name = IccName(t.d + e.at, e.count * TypeSize(e.type)); return name.empty() ? Bytes(e.count) : name; }
			case kExposure:
			{
				const double v = Real(t, e, 0);
				return v > 0.0 && v < 0.5 ? Fmt("1/%.0f s", 1.0 / v) : Number(v) + " s";
			}
			case kFNumber: return "f/" + Number(Real(t, e, 0));
			case kMm: return Number(Real(t, e, 0)) + " mm";
			case kEv: return Fmt("%+.2g EV", Real(t, e, 0));
			case kMetres:
			{
				double v = Real(t, e, 0);
				const Entry* ref = e.tag == 0x0006 ? FindEntry(ifd, 0x0005) : nullptr;
				if (ref && Int(t, *ref, 0) == 1)
					v = -v;
				return Number(v) + " m";
			}
			case kVersion: return e.count == 4 ? Fmt("%c%c.%c%c", t.d[e.at], t.d[e.at + 1], t.d[e.at + 2], t.d[e.at + 3]) : Plain(t, e);
			case kComment: return e.count > 8 ? Text(t.d + e.at + 8, e.count - 8) : std::string();
			case kGpsCoord:
			{
				if (e.count < 3)
					return Plain(t, e);
				const double deg = Real(t, e, 0), min = Real(t, e, 1), sec = Real(t, e, 2), v = deg + min / 60.0 + sec / 3600.0;
				const Entry* ref = FindEntry(ifd, e.tag - 1);
				const char r = ref && ref->type == 2 && ref->count ? (char)t.d[ref->at] : ' ';
				return Fmt("%.0f\xC2\xB0 %.0f' %.2f\" %c  (%.6f)", deg, min, sec, r, r == 'S' || r == 'W' ? -v : v);
			}
			case kGpsTime: return e.count >= 3 ? Fmt("%02.0f:%02.0f:%02.0f", Real(t, e, 0), Real(t, e, 1), Real(t, e, 2)) : Plain(t, e);
			case kOrientation: return Name(kOrientations, (int)Int(t, e, 0));
			case kResUnit: return Name(kResUnits, (int)Int(t, e, 0));
			case kCompression: return Name(kCompressions, (int)Int(t, e, 0));
			case kPhotometric: return Name(kPhotometrics, (int)Int(t, e, 0));
			case kPlanar: return Name(kPlanars, (int)Int(t, e, 0));
			case kPredictor: return Name(kPredictors, (int)Int(t, e, 0));
			case kSampleFormat: return Name(kSampleFormats, (int)Int(t, e, 0));
			case kProgram: return Name(kPrograms, (int)Int(t, e, 0));
			case kMetering: return Name(kMeterings, (int)Int(t, e, 0));
			case kFlash: { const int64_t f = Int(t, e, 0); return (f & 1) ? "fired" : "did not fire"; }
			case kColorSpace: return Name(kColorSpaces, (int)Int(t, e, 0));
			case kWhiteBalance: return Name(kWhiteBalances, (int)Int(t, e, 0));
			case kExposureMode: return Name(kExposureModes, (int)Int(t, e, 0));
			case kScene: return Name(kScenes, (int)Int(t, e, 0));
			case kLight: return Name(kLights, (int)Int(t, e, 0));
			default: return Plain(t, e);
			}
		}

		// one IFD's tags into the current section; its Exif IFD continues the section, its GPS IFD opens one
		void Tags(Sink& s, const Tiff& t, size_t ifd, std::span<const TagInfo> table, int depth)
		{
			size_t next = 0;
			const std::vector<Entry> es = ReadIfd(t, ifd, next);
			size_t exif = 0, gps = 0;
			for (const Entry& e : es)
			{
				const TagInfo* info = Find(table, e.tag);
				const Kind kind = info ? info->kind : kPlain;
				if (kind == kSkip)
					continue;
				if (kind == kExifIfd || kind == kGpsIfd)
				{
					(kind == kExifIfd ? exif : gps) = (size_t)Int(t, e, 0);
					continue;
				}
				s.Add(info ? info->name : Fmt("Tag 0x%04X", e.tag), Value(t, e, kind, es));
			}
			if (exif && depth < 2)
				Tags(s, t, exif, kTags, depth + 1);
			if (gps && depth < 2)
			{
				s.Section("GPS");
				Tags(s, t, gps, kGpsTags, depth + 1);
			}
		}

		void Exif(Sink& s, const uint8_t* data, size_t size, std::span<const TagInfo> table = kTags)
		{
			if (size >= 6 && memcmp(data, "Exif\0\0", 6) == 0)
				data += 6, size -= 6;
			Tiff t;
			if (!t.Init(data, size))
				return;
			Tags(s, t, t.U32(4), table, 0);
		}

		// ---- formats ----

		void Tiffs(Sink& s, const uint8_t* d, size_t n)
		{
			if (n >= 4 && ((d[0] == 'I' && d[1] == 'I' && Le16(d + 2) == 43) || (d[0] == 'M' && d[1] == 'M' && (d[2] << 8 | d[3]) == 43)))
			{
				s.Section("TIFF");
				s.Add("Variant", "BigTIFF (tags not listed)");
				return;
			}
			Tiff t;
			if (!t.Init(d, n))
				return;
			s.Section("TIFF");
			int pages = 0;
			for (size_t ifd = t.U32(4); ifd && pages < 4096; ++pages)
			{
				size_t next = 0;
				ReadIfd(t, ifd, next);
				ifd = next;
			}
			s.Add("Byte order", t.le ? "little-endian" : "big-endian");
			if (pages > 1)
				s.Add("Pages", std::to_string(pages));
			Tags(s, t, t.U32(4), kTags, 0);
		}

		void Jpeg(Sink& s, const uint8_t* d, size_t n)
		{
			s.Section("JPEG");
			std::vector<uint8_t> icc;
			const uint8_t* exif = nullptr;
			size_t exifSize = 0, xmp = 0;
			std::string comment;
			for (size_t o = 2; o + 4 <= n;)
			{
				if (d[o] != 0xFF)
					break;
				const uint8_t m = d[o + 1];
				if (m == 0xFF)
				{
					++o;
					continue;
				}
				if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7))
				{
					o += 2;
					continue;
				}
				const size_t len = (size_t)d[o + 2] << 8 | d[o + 3];
				if (m == 0xDA || len < 2 || o + 2 + len > n)
					break;
				const uint8_t* p = d + o + 4;
				const size_t pn = len - 2;
				if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && pn >= 6)
				{
					const int comps = p[5];
					s.Add("Coding", m == 0xC2 || m == 0xC6 || m == 0xCA || m == 0xCE ? "progressive" : m == 0xC3 || m == 0xC7 || m == 0xCB || m == 0xCF ? "lossless" : "baseline");
					if (m >= 0xC9)
						s.Add("Entropy coding", "arithmetic");
					s.Add("Precision", Fmt("%d bits", p[0]));
					s.Add("Components", std::to_string(comps));
					if (comps == 3 && pn >= 6 + 9)
					{
						const int h = p[7] >> 4, v = p[7] & 15, h2 = p[10] >> 4, v2 = p[10] & 15;
						s.Add("Chroma subsampling", h == h2 && v == v2 ? "4:4:4" : h == 2 * h2 && v == v2 ? "4:2:2" : h == 2 * h2 && v == 2 * v2 ? "4:2:0" : h == h2 && v == 2 * v2 ? "4:4:0" : h == 4 * h2 && v == v2 ? "4:1:1" : Fmt("%dx%d / %dx%d", h, v, h2, v2));
					}
				}
				else if (m == 0xE0 && pn >= 12 && memcmp(p, "JFIF\0", 5) == 0)
				{
					s.Add("JFIF version", Fmt("%d.%02d", p[5], p[6]));
					const int ux = p[8] << 8 | p[9], uy = p[10] << 8 | p[11];
					if (p[7] == 1 || p[7] == 2)
						s.Add("Density", Fmt("%d x %d per %s", ux, uy, p[7] == 1 ? "inch" : "cm"));
				}
				else if (m == 0xE1 && pn > 6 && memcmp(p, "Exif\0\0", 6) == 0)
					exif = p + 6, exifSize = pn - 6;
				else if (m == 0xE1 && pn > 29 && memcmp(p, "http://ns.adobe.com/xap/1.0/\0", 29) == 0)
					xmp = pn - 29;
				else if (m == 0xE2 && pn > 14 && memcmp(p, "ICC_PROFILE\0", 12) == 0)
					icc.insert(icc.end(), p + 14, p + pn);
				else if (m == 0xED && pn > 14)
					s.Add("Photoshop / IPTC", Bytes(pn));
				else if (m == 0xEE && pn >= 12 && memcmp(p, "Adobe", 5) == 0)
					s.Add("Adobe transform", p[11] == 0 ? "none (RGB or CMYK)" : p[11] == 1 ? "YCbCr" : "YCCK");
				else if (m == 0xFE)
					comment += (comment.empty() ? "" : "\n") + Text(p, pn);
				o += 2 + len;
			}
			s.Add("Comment", comment);
			if (!icc.empty())
				s.Add("ICC profile", IccName(icc.data(), icc.size()));
			if (xmp)
				s.Add("XMP", Bytes(xmp));
			if (exif)
			{
				s.Section("EXIF");
				Exif(s, exif, exifSize);
			}
		}

		void Png(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8) != 0)
				return;
			s.Section("PNG");
			const uint8_t* exif = nullptr;
			size_t exifSize = 0;
			int idats = 0;
			for (size_t o = 8; o + 12 <= n;)
			{
				const size_t len = Be32(d + o);
				const uint8_t* type = d + o + 4;
				const uint8_t* p = d + o + 8;
				if (len > n - o - 12)
					break;
				const auto is = [&](const char* t) { return memcmp(type, t, 4) == 0; };
				if (is("IHDR") && len >= 13)
				{
					static constexpr Named colours[] = { { 0, "greyscale" }, { 2, "RGB" }, { 3, "palette" }, { 4, "greyscale + alpha" }, { 6, "RGBA" } };
					s.Add("Colour type", Name(colours, p[9]));
					s.Add("Bit depth", std::to_string(p[8]));
					s.Add("Interlace", p[12] ? "Adam7" : "none");
				}
				else if (is("tEXt") || is("zTXt") || is("iTXt"))
				{
					const uint8_t* end = p + len;
					const uint8_t* key = p;
					const uint8_t* z = std::find(p, end, 0);
					if (z < end)
					{
						std::string text;
						if (is("tEXt"))
							text = Text(z + 1, end - z - 1);
						else if (is("zTXt") && z + 2 <= end)
						{
							const std::vector<uint8_t> raw = Inflate(z + 2, end - z - 2);
							text = Text(raw.data(), raw.size());
						}
						else if (is("iTXt") && z + 3 <= end)
						{
							const bool compressed = z[1] != 0;
							const uint8_t* lang = z + 3;
							const uint8_t* lz = std::find(lang, end, 0);
							const uint8_t* tz = lz < end ? std::find(lz + 1, end, 0) : end;
							if (tz < end)
							{
								const std::vector<uint8_t> raw = compressed ? Inflate(tz + 1, end - tz - 1) : std::vector<uint8_t>(tz + 1, end);
								text = Text(raw.data(), raw.size());
							}
						}
						const std::string k = Text(key, z - key);
						s.Add(k == "XML:com.adobe.xmp" ? "XMP" : k, k == "XML:com.adobe.xmp" ? Bytes(text.size()) : text);
					}
				}
				else if (is("gAMA") && len >= 4)
					s.Add("Gamma", Number(Be32(p) / 100000.0));
				else if (is("sRGB") && len >= 1)
				{
					static constexpr Named intents[] = { { 0, "perceptual" }, { 1, "relative colorimetric" }, { 2, "saturation" }, { 3, "absolute colorimetric" } };
					s.Add("sRGB", Name(intents, p[0]));
				}
				else if (is("cHRM") && len >= 32)
					s.Add("Chromaticities", Fmt("white %.4f %.4f, red %.4f %.4f, green %.4f %.4f, blue %.4f %.4f", Be32(p) / 1e5, Be32(p + 4) / 1e5, Be32(p + 8) / 1e5, Be32(p + 12) / 1e5, Be32(p + 16) / 1e5, Be32(p + 20) / 1e5, Be32(p + 24) / 1e5, Be32(p + 28) / 1e5));
				else if (is("cICP") && len >= 4)
					s.Add("Coding-independent code points", Fmt("primaries %d, transfer %d, matrix %d, %s range", p[0], p[1], p[2], p[3] ? "full" : "narrow"));
				else if (is("iCCP"))
				{
					const uint8_t* z = std::find(p, p + len, 0);
					std::string name;
					if (z + 2 <= p + len)
					{
						const std::vector<uint8_t> icc = Inflate(z + 2, p + len - z - 2);
						name = IccName(icc.data(), icc.size());
					}
					s.Add("ICC profile", name.empty() ? Text(p, z - p) : name);
				}
				else if (is("pHYs") && len >= 9)
					s.Add(p[8] == 1 ? "Resolution" : "Pixel aspect", p[8] == 1 ? Fmt("%.0f x %.0f dpi", Be32(p) * 0.0254, Be32(p + 4) * 0.0254) : Fmt("%u : %u", Be32(p), Be32(p + 4)));
				else if (is("tIME") && len >= 7)
					s.Add("Modified (tIME)", Fmt("%04d-%02d-%02d %02d:%02d:%02d", p[0] << 8 | p[1], p[2], p[3], p[4], p[5], p[6]));
				else if (is("acTL") && len >= 8)
					s.Add("Animation", Fmt("%u frames, %s", Be32(p), Be32(p + 4) ? Fmt("%u plays", Be32(p + 4)).c_str() : "loops forever"));
				else if (is("eXIf"))
					exif = p, exifSize = len;
				else if (is("IDAT"))
					++idats;
				else if (is("IEND"))
					break;
				o += 12 + len;
			}
			s.Add("Data chunks", std::to_string(idats));
			if (exif)
			{
				s.Section("EXIF");
				Exif(s, exif, exifSize);
			}
		}

		void Webp(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0)
				return;
			s.Section("WebP");
			const uint8_t* exif = nullptr;
			size_t exifSize = 0;
			int frames = 0;
			for (size_t o = 12; o + 8 <= n;)
			{
				const uint8_t* id = d + o;
				const size_t size = Le32(d + o + 4);
				const uint8_t* p = d + o + 8;
				if (size > n - o - 8)
					break;
				if (memcmp(id, "VP8 ", 4) == 0)
					s.Add("Coding", "lossy (VP8)");
				else if (memcmp(id, "VP8L", 4) == 0)
					s.Add("Coding", "lossless (VP8L)");
				else if (memcmp(id, "VP8X", 4) == 0 && size >= 10)
				{
					s.Add("Canvas", Fmt("%u x %u", 1 + (p[4] | p[5] << 8 | p[6] << 16), 1 + (p[7] | p[8] << 8 | p[9] << 16)));
					s.Add("Alpha", (p[0] & 0x10) ? "yes" : "no");
				}
				else if (memcmp(id, "ANIM", 4) == 0 && size >= 6)
					s.Add("Loops", Le16(p + 4) ? std::to_string(Le16(p + 4)) : std::string("forever"));
				else if (memcmp(id, "ANMF", 4) == 0)
					++frames;
				else if (memcmp(id, "ICCP", 4) == 0)
					s.Add("ICC profile", IccName(p, size));
				else if (memcmp(id, "EXIF", 4) == 0)
					exif = p, exifSize = size;
				else if (memcmp(id, "XMP ", 4) == 0)
					s.Add("XMP", Bytes(size));
				o += 8 + size + (size & 1);
			}
			if (frames)
				s.Add("Frames", std::to_string(frames));
			if (exif)
			{
				s.Section("EXIF");
				Exif(s, exif, exifSize);
			}
		}

		void Heic(Sink& s, const uint8_t* d, size_t n)
		{
			heif_context* ctx = heif_context_alloc();
			if (!ctx)
				return;
			heif_image_handle* h = nullptr;
			if (heif_context_read_from_memory_without_copy(ctx, d, n, nullptr).code == heif_error_Ok && heif_context_get_primary_image_handle(ctx, &h).code == heif_error_Ok)
			{
				s.Section("HEIF");
				s.Add("Images in file", std::to_string(heif_context_get_number_of_top_level_images(ctx)));
				s.Add("Luma bits", std::to_string(heif_image_handle_get_luma_bits_per_pixel(h)));
				s.Add("Chroma bits", std::to_string(heif_image_handle_get_chroma_bits_per_pixel(h)));
				s.Add("Alpha", heif_image_handle_has_alpha_channel(h) ? "yes" : "no");
				if (const int thumbs = heif_image_handle_get_number_of_thumbnails(h))
					s.Add("Thumbnails", std::to_string(thumbs));
				if (heif_image_handle_has_depth_image(h))
					s.Add("Depth image", "yes");
				heif_color_profile_nclx* nclx = nullptr;
				if (heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok && nclx)
				{
					s.Add("Colour (nclx)", Fmt("primaries %d, transfer %d, matrix %d, %s range", (int)nclx->color_primaries, (int)nclx->transfer_characteristics, (int)nclx->matrix_coefficients, nclx->full_range_flag ? "full" : "limited"));
					heif_nclx_color_profile_free(nclx);
				}
				if (const size_t iccSize = heif_image_handle_get_raw_color_profile_size(h))
				{
					std::vector<uint8_t> icc(iccSize);
					if (heif_image_handle_get_raw_color_profile(h, icc.data()).code == heif_error_Ok)
						s.Add("ICC profile", IccName(icc.data(), icc.size()));
				}
				std::vector<uint8_t> exif;
				const int count = heif_image_handle_get_number_of_metadata_blocks(h, nullptr);
				std::vector<heif_item_id> ids((size_t)std::max(count, 0));
				heif_image_handle_get_list_of_metadata_block_IDs(h, nullptr, ids.data(), count);
				for (heif_item_id id : ids)
				{
					const std::string type = heif_image_handle_get_metadata_type(h, id);
					const size_t size = heif_image_handle_get_metadata_size(h, id);
					if (type == "Exif" && size > 4)
					{
						exif.resize(size);
						if (heif_image_handle_get_metadata(h, id, exif.data()).code != heif_error_Ok)
							exif.clear();
					}
					else if (type == "mime")
						s.Add(std::string(heif_image_handle_get_metadata_content_type(h, id)) == "application/rdf+xml" ? "XMP" : heif_image_handle_get_metadata_content_type(h, id), Bytes(size));
				}
				// the Exif item starts with the offset of its TIFF header
				if (exif.size() > 4 && 4 + (size_t)Be32(exif.data()) < exif.size())
				{
					s.Section("EXIF");
					Exif(s, exif.data() + 4 + Be32(exif.data()), exif.size() - 4 - Be32(exif.data()));
				}
				heif_image_handle_release(h);
			}
			heif_context_free(ctx);
		}

		void Cr3(Sink& s, const std::wstring& path)
		{
			std::vector<uint8_t> cmt[4];
			if (!fmt::ReadCr3Metadata(path, cmt).empty())
				return;
			if (!cmt[0].empty() || !cmt[1].empty())
			{
				s.Section("EXIF");
				Exif(s, cmt[0].data(), cmt[0].size());
				Exif(s, cmt[1].data(), cmt[1].size());
			}
			if (!cmt[3].empty())
			{
				s.Section("GPS");
				Exif(s, cmt[3].data(), cmt[3].size(), kGpsTags);
			}
		}

		void Gif(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 13 || memcmp(d, "GIF", 3) != 0)
				return;
			const auto skip = [&](size_t o) { while (o < n && d[o]) o += 1 + (size_t)d[o]; return o + 1; };
			s.Section("GIF");
			s.Add("Version", std::string((const char*)d + 3, 3));
			size_t o = 13;
			if (d[10] & 0x80)
			{
				s.Add("Global palette", Fmt("%d colours", 2 << (d[10] & 7)));
				o += 3 * (size_t)(2 << (d[10] & 7));
			}
			int frames = 0, loops = -1;
			std::string comment;
			while (o < n && d[o] != 0x3B)
			{
				if (d[o] == 0x2C && o + 10 <= n)
				{
					++frames;
					const uint8_t packed = d[o + 9];
					o += 10 + ((packed & 0x80) ? 3 * (size_t)(2 << (packed & 7)) : 0) + 1;
					o = skip(o);
				}
				else if (d[o] == 0x21 && o + 2 <= n)
				{
					const uint8_t label = d[o + 1];
					o += 2;
					if (label == 0xFF && o + 16 <= n && d[o] == 11 && memcmp(d + o + 1, "NETSCAPE2.0", 11) == 0 && d[o + 12] == 3 && d[o + 13] == 1)
						loops = Le16(d + o + 14);
					if (label == 0xFE)
						for (size_t b = o; b < n && d[b]; b += 1 + (size_t)d[b])
							comment += Text(d + b + 1, std::min<size_t>(d[b], n - b - 1));
					o = skip(o);
				}
				else
					break;
			}
			s.Add("Frames", std::to_string(frames));
			if (loops >= 0)
				s.Add("Loops", loops ? std::to_string(loops) : std::string("forever"));
			s.Add("Comment", comment);
		}

		void Hdr(Sink& s, const uint8_t* d, size_t n)
		{
			s.Section("Radiance HDR");
			size_t o = 0;
			for (int lines = 0; o < n && lines < 64; ++lines)
			{
				const uint8_t* e = std::find(d + o, d + n, '\n');
				const std::string line = Text(d + o, e - (d + o));
				o = e - d + 1;
				if (line.empty())
					break;
				const size_t eq = line.find('=');
				if (line.rfind("#?", 0) == 0)
					s.Add("Signature", line.substr(2));
				else if (line[0] == '#')
					s.Add("Comment", Text(line.data() + 1, line.size() - 1));
				else if (eq != std::string::npos)
					s.Add(line.substr(0, eq), line.substr(eq + 1));
			}
			if (o < n)
			{
				const uint8_t* e = std::find(d + o, d + n, '\n');
				s.Add("Resolution line", Text(d + o, e - (d + o)));
			}
		}

		void Bmp(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 30 || d[0] != 'B' || d[1] != 'M')
				return;
			s.Section("BMP");
			const uint32_t hs = Le32(d + 14);
			s.Add("Header", hs == 12 ? "BITMAPCOREHEADER" : hs == 40 ? "BITMAPINFOHEADER" : hs == 52 ? "BITMAPV2INFOHEADER" : hs == 56 ? "BITMAPV3INFOHEADER" : hs == 108 ? "BITMAPV4HEADER" : hs == 124 ? "BITMAPV5HEADER" : Fmt("%u bytes", hs));
			if (hs >= 40 && n >= 54)
			{
				static constexpr Named comps[] = { { 0, "none" }, { 1, "RLE8" }, { 2, "RLE4" }, { 3, "bit fields" }, { 4, "JPEG" }, { 5, "PNG" }, { 6, "alpha bit fields" } };
				s.Add("Bits per pixel", std::to_string(Le16(d + 28)));
				s.Add("Compression", Name(comps, (int)Le32(d + 30)));
				if (Le32(d + 38))
					s.Add("Resolution", Fmt("%.0f x %.0f dpi", Le32(d + 38) * 0.0254, Le32(d + 42) * 0.0254));
				if (Le32(d + 46))
					s.Add("Palette colours", std::to_string(Le32(d + 46)));
				s.Add("Row order", (int32_t)Le32(d + 22) < 0 ? "top-down" : "bottom-up");
			}
		}

		void Tga(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 18)
				return;
			static constexpr Named types[] = { { 1, "colour-mapped" }, { 2, "true colour" }, { 3, "greyscale" }, { 9, "colour-mapped, RLE" }, { 10, "true colour, RLE" }, { 11, "greyscale, RLE" } };
			s.Section("TGA");
			s.Add("Image type", Name(types, d[2]));
			s.Add("Bits per pixel", std::to_string(d[16]));
			s.Add("Alpha bits", std::to_string(d[17] & 15));
			s.Add("Origin", (d[17] & 0x20) ? "top" : "bottom");
			if (d[0] && 18u + d[0] <= n)
				s.Add("Image ID", Text(d + 18, d[0]));
			if (n >= 26 + 495 && memcmp(d + n - 18, "TRUEVISION-XFILE.", 17) == 0)
			{
				const size_t ext = Le32(d + n - 26);
				if (ext && ext + 495 <= n && Le16(d + ext) >= 495)
				{
					s.Add("Author", Text(d + ext + 2, 41));
					s.Add("Comments", Text(d + ext + 43, 324));
					s.Add("Job", Text(d + ext + 379, 41));
					s.Add("Software", Text(d + ext + 426, 41));
				}
			}
		}

		void Ico(Sink& s, const uint8_t* d, size_t n)
		{
			if (n < 6)
				return;
			const int count = Le16(d + 4);
			s.Section(Le16(d + 2) == 2 ? "Cursor" : "Icon");
			for (int i = 0; i < count && 6 + 16 * (size_t)(i + 1) <= n; ++i)
			{
				const uint8_t* e = d + 6 + 16 * (size_t)i;
				const size_t at = Le32(e + 12);
				const bool png = at + 8 <= n && memcmp(d + at, "\x89PNG", 4) == 0;
				s.Add(Fmt("Entry %d", i + 1), Fmt("%d x %d, %s, %s", e[0] ? e[0] : 256, e[1] ? e[1] : 256, png ? "PNG" : Fmt("%d bpp", Le16(e + 6)).c_str(), Bytes(Le32(e + 8)).c_str()));
			}
		}

		void Svg(Sink& s, const uint8_t* d, size_t n)
		{
			const std::string text((const char*)d, std::min<size_t>(n, 256 * 1024));
			const size_t at = text.find("<svg");
			if (at == std::string::npos)
				return;
			const size_t end = text.find('>', at);
			const std::string tag = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
			const auto attr = [&](const char* name) -> std::string
			{
				for (size_t p = tag.find(name); p != std::string::npos; p = tag.find(name, p + 1))
				{
					const size_t q = p + strlen(name);
					if ((p > 0 && !isspace((unsigned char)tag[p - 1])) || q + 1 >= tag.size() || tag[q] != '=' || (tag[q + 1] != '"' && tag[q + 1] != '\''))
						continue;
					const size_t close = tag.find(tag[q + 1], q + 2);
					return close == std::string::npos ? std::string() : tag.substr(q + 2, close - q - 2);
				}
				return {};
			};
			s.Section("SVG");
			s.Add("Width", attr("width"));
			s.Add("Height", attr("height"));
			s.Add("View box", attr("viewBox"));
			s.Add("Version", attr("version"));
			const size_t t0 = text.find("<title>"), t1 = t0 == std::string::npos ? t0 : text.find("</title>", t0);
			if (t1 != std::string::npos)
				s.Add("Title", Text(text.data() + t0 + 7, t1 - t0 - 7));
		}

		std::string ExrValue(const exr_attribute_t& a)
		{
			static constexpr const char* compressions[] = { "none", "RLE", "ZIPS", "ZIP", "PIZ", "PXR24", "B44", "B44A", "DWAA", "DWAB", "HTJ2K256", "HTJ2K32" };
			static constexpr const char* pixels[] = { "uint", "half", "float" };
			switch (a.type)
			{
			case EXR_ATTR_BOX2I: return Fmt("(%d, %d) - (%d, %d)  [%d x %d]", a.box2i->min.x, a.box2i->min.y, a.box2i->max.x, a.box2i->max.y, a.box2i->max.x - a.box2i->min.x + 1, a.box2i->max.y - a.box2i->min.y + 1);
			case EXR_ATTR_BOX2F: return Fmt("(%g, %g) - (%g, %g)", a.box2f->min.x, a.box2f->min.y, a.box2f->max.x, a.box2f->max.y);
			case EXR_ATTR_CHLIST:
			{
				std::string s;
				for (int i = 0; i < a.chlist->num_channels; ++i)
				{
					const auto& c = a.chlist->entries[i];
					s += (i ? ", " : "") + std::string(c.name.str, (size_t)c.name.length) + " " + (c.pixel_type < 3 ? pixels[c.pixel_type] : "?");
					if (c.x_sampling != 1 || c.y_sampling != 1)
						s += Fmt(" (%dx%d)", c.x_sampling, c.y_sampling);
				}
				return s;
			}
			case EXR_ATTR_CHROMATICITIES: { const auto& c = *a.chromaticities; return Fmt("red %.4f %.4f, green %.4f %.4f, blue %.4f %.4f, white %.4f %.4f", c.red_x, c.red_y, c.green_x, c.green_y, c.blue_x, c.blue_y, c.white_x, c.white_y); }
			case EXR_ATTR_COMPRESSION: return a.uc < 12 ? compressions[a.uc] : std::to_string(a.uc);
			case EXR_ATTR_DOUBLE: return Number(a.d);
			case EXR_ATTR_ENVMAP: return a.uc == 0 ? "lat-long" : a.uc == 1 ? "cube" : std::to_string(a.uc);
			case EXR_ATTR_FLOAT: return Number(a.f);
			case EXR_ATTR_FLOAT_VECTOR:
			{
				std::string s;
				for (int i = 0; i < std::min(a.floatvector->length, 16); ++i)
					s += (i ? ", " : "") + Number(a.floatvector->arr[i]);
				return a.floatvector->length > 16 ? s + Fmt(" ... (%d values)", a.floatvector->length) : s;
			}
			case EXR_ATTR_INT: return std::to_string(a.i);
			case EXR_ATTR_KEYCODE: { const auto& k = *a.keycode; return Fmt("film %d, type %d, prefix %d, count %d, perf offset %d, %d perfs/frame, %d perfs/count", k.film_mfc_code, k.film_type, k.prefix, k.count, k.perf_offset, k.perfs_per_frame, k.perfs_per_count); }
			case EXR_ATTR_LINEORDER: return a.uc == 0 ? "increasing Y" : a.uc == 1 ? "decreasing Y" : a.uc == 2 ? "random Y" : std::to_string(a.uc);
			case EXR_ATTR_M33F: case EXR_ATTR_M33D: case EXR_ATTR_M44F: case EXR_ATTR_M44D:
			{
				const int k = a.type == EXR_ATTR_M33F || a.type == EXR_ATTR_M33D ? 3 : 4;
				std::string s;
				for (int r = 0; r < k; ++r)
				{
					s += r ? "  [" : "[";
					for (int c = 0; c < k; ++c)
					{
						const double v = a.type == EXR_ATTR_M33F ? a.m33f->m[r * 3 + c] : a.type == EXR_ATTR_M33D ? a.m33d->m[r * 3 + c] : a.type == EXR_ATTR_M44F ? a.m44f->m[r * 4 + c] : a.m44d->m[r * 4 + c];
						s += (c ? " " : "") + Number(v);
					}
					s += "]";
				}
				return s;
			}
			case EXR_ATTR_PREVIEW: return Fmt("%u x %u thumbnail", a.preview->width, a.preview->height);
			case EXR_ATTR_RATIONAL: return a.rational->denom == 1 ? std::to_string(a.rational->num) : Fmt("%d/%u (%g)", a.rational->num, a.rational->denom, a.rational->denom ? (double)a.rational->num / a.rational->denom : 0.0);
			case EXR_ATTR_STRING: return Text(a.string->str, (size_t)a.string->length);
			case EXR_ATTR_STRING_VECTOR:
			{
				std::string s;
				for (int i = 0; i < a.stringvector->n_strings; ++i)
					s += (i ? "; " : "") + Text(a.stringvector->strings[i].str, (size_t)a.stringvector->strings[i].length);
				return s;
			}
			case EXR_ATTR_TILEDESC:
			{
				const int mode = a.tiledesc->level_and_round & 15, round = a.tiledesc->level_and_round >> 4;
				return Fmt("%u x %u tiles, %s%s", a.tiledesc->x_size, a.tiledesc->y_size, mode == 0 ? "one level" : mode == 1 ? "mip-mapped" : "rip-mapped", mode ? (round ? ", rounded up" : ", rounded down") : "");
			}
			case EXR_ATTR_TIMECODE:
			{
				const uint32_t t = a.timecode->time_and_flags;
				const auto bcd = [](uint32_t v) { return (int)((v >> 4) * 10 + (v & 15)); };
				return Fmt("%02d:%02d:%02d:%02d", bcd((t >> 24) & 0x3F), bcd((t >> 16) & 0x7F), bcd((t >> 8) & 0x7F), bcd(t & 0x3F));
			}
			case EXR_ATTR_V2I: return Fmt("(%d, %d)", a.v2i->x, a.v2i->y);
			case EXR_ATTR_V2F: return Fmt("(%g, %g)", a.v2f->x, a.v2f->y);
			case EXR_ATTR_V2D: return Fmt("(%g, %g)", a.v2d->x, a.v2d->y);
			case EXR_ATTR_V3I: return Fmt("(%d, %d, %d)", a.v3i->x, a.v3i->y, a.v3i->z);
			case EXR_ATTR_V3F: return Fmt("(%g, %g, %g)", a.v3f->x, a.v3f->y, a.v3f->z);
			case EXR_ATTR_V3D: return Fmt("(%g, %g, %g)", a.v3d->x, a.v3d->y, a.v3d->z);
			case EXR_ATTR_DEEP_IMAGE_STATE: { static constexpr const char* states[] = { "messy", "sorted", "non-overlapping", "tidy" }; return a.uc < 4 ? states[a.uc] : std::to_string(a.uc); }
			case EXR_ATTR_OPAQUE: return Fmt("%s, %s", a.type_name, Bytes((uint64_t)std::max(a.opaque->size, 0)).c_str());
			default: return a.type_name ? a.type_name : "?";
			}
		}

		void Exr(Sink& s, const std::wstring& path)
		{
			exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
			init.error_handler_fn = [](exr_const_context_t, exr_result_t, const char*) {};
			exr_context_t ctx = nullptr;
			if (exr_start_read(&ctx, ToUtf8(path).c_str(), &init) == EXR_ERR_SUCCESS)
			{
				int parts = 0;
				exr_get_count(ctx, &parts);
				for (int p = 0; p < parts; ++p)
				{
					const char* name = nullptr;
					exr_get_name(ctx, p, &name);
					s.Section(parts > 1 ? Fmt("EXR part %d%s%s", p, name ? ": " : "", name ? name : "") : std::string("EXR header"));
					int32_t count = 0;
					exr_get_attribute_count(ctx, p, &count);
					for (int32_t i = 0; i < count; ++i)
					{
						const exr_attribute_t* a = nullptr;
						if (exr_get_attribute_by_index(ctx, p, EXR_ATTR_LIST_FILE_ORDER, i, &a) == EXR_ERR_SUCCESS && a)
							s.Add(a->name, ExrValue(*a));
					}
				}
			}
			exr_finish(&ctx);
		}

		void FileFacts(Sink& s, const std::wstring& path, const ImageInfo& info)
		{
			const std::filesystem::path p(path);
			s.Section("File");
			s.Add("Name", ToUtf8(p.filename().wstring()));
			s.Add("Folder", ToUtf8(p.parent_path().wstring()));
			WIN32_FILE_ATTRIBUTE_DATA fa;
			if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
			{
				s.Add("Size", Bytes((uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow));
				s.Add("Modified", LocalTime(fa.ftLastWriteTime));
				s.Add("Created", LocalTime(fa.ftCreationTime));
			}
			s.Section("Image");
			std::wstring ext = p.extension().wstring();
			for (auto& c : ext)
				c = (wchar_t)towlower(c);
			for (const FormatDesc& f : Formats())
				if (ext.size() > 1 && ToUtf8(ext.substr(1)) == f.ext)
					s.Add("Format", f.name);
			const auto [w, h] = info.mips.empty() ? std::make_pair(info.width, info.height) : info.mips[0];
			s.Add("Dimensions", Fmt("%d x %d", w, h));
			if (info.mips.size() > 1)
				s.Add("Mip levels", std::to_string(info.mips.size()));
			if (info.frames > 1)
				s.Add("Frames", std::to_string(info.frames));
			if (info.colorMatrix)
				s.Add("Colour", "wide gamut, shown converted to sRGB primaries");
		}
	}

	std::vector<MetaEntry> ReadMetadata(const std::wstring& path, const ImageInfo& info)
	{
		std::vector<MetaEntry> out;
		Sink s{ out };
		try
		{
			FileFacts(s, path, info);
			if (info.kind == FileKind::Exr)
				Exr(s, path);
			else if (info.kind == FileKind::Cr3)
				Cr3(s, path);
			else
			{
				const fmt::MappedFile m(path);
				if (!m.data)
					return out;
				switch (info.kind)
				{
				case FileKind::Jpeg: Jpeg(s, m.data, m.size); break;
				case FileKind::Png: Png(s, m.data, m.size); break;
				case FileKind::Tiff: Tiffs(s, m.data, m.size); break;
				case FileKind::Heic: Heic(s, m.data, m.size); break;
				case FileKind::Webp: Webp(s, m.data, m.size); break;
				case FileKind::Gif: Gif(s, m.data, m.size); break;
				case FileKind::Hdr: Hdr(s, m.data, m.size); break;
				case FileKind::Bmp: Bmp(s, m.data, m.size); break;
				case FileKind::Tga: Tga(s, m.data, m.size); break;
				case FileKind::Ico: Ico(s, m.data, m.size); break;
				case FileKind::Svg: Svg(s, m.data, m.size); break;
				default: break;
				}
			}
		}
		catch (...)
		{
		}
		return out;
	}
}
