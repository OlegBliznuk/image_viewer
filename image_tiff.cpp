#include "image_formats.h"

#include <libdeflate.h>
#include <tiffio.h>

#include <immintrin.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>

#pragma comment(lib, "tiff.lib")
#pragma comment(lib, "deflate.lib")

// TIFF and .tx through libtiff (tiff.dll delay-loaded), read from memory; strips or tiles are handed out to the workers of a Parallel, deflate chunks inflated straight from the file
namespace iv::fmt
{
	namespace
	{
		struct Mem
		{
			const uint8_t* data = nullptr;
			size_t size = 0;
			size_t pos = 0;
		};

		tmsize_t ReadProc(thandle_t h, void* buf, tmsize_t n)
		{
			Mem& m = *(Mem*)h;
			const size_t k = std::min((size_t)n, m.size - std::min(m.pos, m.size));
			memcpy(buf, m.data + m.pos, k);
			m.pos += k;
			return (tmsize_t)k;
		}
		tmsize_t WriteProc(thandle_t, void*, tmsize_t) { return 0; }
		toff_t SeekProc(thandle_t h, toff_t off, int whence)
		{
			Mem& m = *(Mem*)h;
			m.pos = whence == SEEK_SET ? (size_t)off : whence == SEEK_CUR ? m.pos + (size_t)off : m.size + (size_t)off;
			return m.pos;
		}
		int CloseProc(thandle_t) { return 0; }
		toff_t SizeProc(thandle_t h) { return ((Mem*)h)->size; }
		int MapProc(thandle_t h, void** base, toff_t* size) { *base = (void*)((Mem*)h)->data, *size = ((Mem*)h)->size; return 1; }
		void UnmapProc(thandle_t, void*, toff_t) {}
		int OnError(TIFF*, void* user, const char*, const char* fmt, va_list ap)
		{
			std::string& e = *(std::string*)user;
			if (e.empty())
			{
				char buf[256];
				vsnprintf(buf, sizeof(buf), fmt, ap);
				e = std::string("tiff: ") + buf;
			}
			return 1;
		}
		int OnWarning(TIFF*, void*, const char*, const char*, va_list) { return 1; }

		// one libtiff handle over the buffer (the buffer is mapped, not copied); libtiff errors land in error
		struct Handle
		{
			Mem mem;
			std::string error;
			TIFF* tif = nullptr;

			Handle(const uint8_t* data, size_t size) : mem{ data, size }
			{
				TIFFOpenOptions* o = TIFFOpenOptionsAlloc();
				TIFFOpenOptionsSetErrorHandlerExtR(o, OnError, &error);
				TIFFOpenOptionsSetWarningHandlerExtR(o, OnWarning, nullptr);
				tif = TIFFClientOpenExt("memory", "r", (thandle_t)&mem, ReadProc, WriteProc, SeekProc, CloseProc, SizeProc, MapProc, UnmapProc, o);
				TIFFOpenOptionsFree(o);
			}
			~Handle() { if (tif) TIFFClose(tif); }
			Handle(const Handle&) = delete;
			std::string Error() const { return error.empty() ? std::string("tiff: cannot read") : error; }
		};

		struct Page
		{
			int dir = 0;
			uint32_t w = 0, h = 0, rowsPerStrip = 0, tileW = 0, tileH = 0;
			uint16_t spp = 1, bps = 1, format = SAMPLEFORMAT_UINT, photometric = PHOTOMETRIC_RGB, planar = PLANARCONFIG_CONTIG, orientation = ORIENTATION_TOPLEFT;
			bool tiled = false;

			// gray / RGB(A) in 8 / 16-bit integers or 16 / 32-bit floats decode directly; anything else through libtiff's RGBA8 reader
			bool Direct() const
			{
				const bool ints = format == SAMPLEFORMAT_UINT && (bps == 8 || bps == 16), floats = format == SAMPLEFORMAT_IEEEFP && (bps == 16 || bps == 32);
				return (photometric == PHOTOMETRIC_MINISBLACK || (photometric == PHOTOMETRIC_RGB && spp >= 3)) && (ints || floats);
			}
			int Bits() const { return Direct() ? bps : 8; }
			// contiguous RGB of up to 16-bit samples stays 3 channels: rows copy straight into the frame and the GPU adds alpha (measured slower for 32-bit floats)
			int Channels() const { return Direct() && photometric == PHOTOMETRIC_RGB && planar == PLANARCONFIG_CONTIG && spp == 3 && bps <= 16 ? 3 : 4; }
			// orientations 5-8 turn the page by 90 degrees (applied to direct pages; libtiff's RGBA reader only flips)
			bool Turned() const { return Direct() && orientation >= 5 && orientation <= 8; }
			int W() const { return (int)(Turned() ? h : w); }
			int H() const { return (int)(Turned() ? w : h); }
			bool Float() const { return Direct() && format == SAMPLEFORMAT_IEEEFP; }
			size_t PixelBytes() const { return (size_t)Bits() / 8 * Channels(); }
			std::string Note() const
			{
				const char* type = Float() ? (bps == 16 ? "16fp" : "32fp") : Direct() ? (bps == 16 ? "16u" : "8u") : "8u (converted)";
				return std::to_string(W()) + " x " + std::to_string(H()) + ", " + type + (photometric == PHOTOMETRIC_MINISBLACK ? " gray" : "");
			}
		};

		Page ReadPage(TIFF* t, int dir)
		{
			Page p;
			p.dir = dir;
			TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &p.w);
			TIFFGetField(t, TIFFTAG_IMAGELENGTH, &p.h);
			TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &p.spp);
			TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &p.bps);
			TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &p.format);
			TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &p.planar);
			TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &p.orientation);
			if (!TIFFGetField(t, TIFFTAG_PHOTOMETRIC, &p.photometric))
				p.photometric = p.spp >= 3 ? PHOTOMETRIC_RGB : PHOTOMETRIC_MINISBLACK;
			p.tiled = TIFFIsTiled(t) != 0;
			if (p.tiled)
				TIFFGetField(t, TIFFTAG_TILEWIDTH, &p.tileW), TIFFGetField(t, TIFFTAG_TILELENGTH, &p.tileH);
			else
				TIFFGetFieldDefaulted(t, TIFFTAG_ROWSPERSTRIP, &p.rowsPerStrip);
			return p;
		}

		// the pages shown as frames: those decoding to the first page's sample type; a mip pyramid (each page half the one before, as maketx writes .tx files) is one image whose pages are its levels
		std::string Pages(Handle& h, std::vector<Page>& pages, int& canvasW, int& canvasH, int& mipLevels)
		{
			if (!h.tif)
				return h.Error();
			pages.clear();
			int dir = 0;
			do
			{
				const Page p = ReadPage(h.tif, dir++);
				if (p.w > 0 && p.h > 0 && p.w <= 65536 && p.h <= 65536 && (pages.empty() || (p.Bits() == pages[0].Bits() && p.Float() == pages[0].Float() && p.Channels() == pages[0].Channels())))
					pages.push_back(p);
			} while (dir < 4096 && TIFFReadDirectory(h.tif));
			if (pages.empty())
				return "tiff: no readable page";
			const auto halved = [](int big, int small) { return small == std::max(1, big / 2) || small == std::max(1, (big + 1) / 2); };
			bool chain = pages.size() > 1;
			for (size_t i = 1; i < pages.size() && chain; ++i)
				chain = halved(pages[i - 1].W(), pages[i].W()) && halved(pages[i - 1].H(), pages[i].H());
			mipLevels = chain ? (int)pages.size() : 1;
			canvasW = canvasH = 0;
			for (const Page& p : pages)
				canvasW = std::max(canvasW, p.W()), canvasH = std::max(canvasH, p.H());
			return {};
		}

		// RGB to RGBA rows; the vector loops stop early enough that their 16-byte loads stay inside the row
		void ExpandRgb(const uint8_t* s, int n, uint8_t* d)
		{
			const __m256i shuffle = _mm256_setr_epi8(0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1, 0, 1, 2, -1, 3, 4, 5, -1, 6, 7, 8, -1, 9, 10, 11, -1);
			const __m256i alpha = _mm256_set1_epi32((int)0xFF000000);
			int x = 0;
			for (; x + 10 <= n; x += 8)
			{
				const __m256i v = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)(s + x * 3))), _mm_loadu_si128((const __m128i*)(s + x * 3 + 12)), 1);
				_mm256_storeu_si256((__m256i*)(d + x * 4), _mm256_or_si256(_mm256_shuffle_epi8(v, shuffle), alpha));
			}
			for (; x < n; ++x)
				d[x * 4] = s[x * 3], d[x * 4 + 1] = s[x * 3 + 1], d[x * 4 + 2] = s[x * 3 + 2], d[x * 4 + 3] = 0xFF;
		}

		void ExpandRgb(const uint16_t* s, int n, uint16_t one, uint16_t* d)
		{
			const __m256i shuffle = _mm256_setr_epi8(0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1, 0, 1, 2, 3, 4, 5, -1, -1, 6, 7, 8, 9, 10, 11, -1, -1);
			const __m256i alpha = _mm256_set1_epi64x((long long)one << 48);
			int x = 0;
			for (; x + 6 <= n; x += 4)
			{
				const __m256i v = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_loadu_si128((const __m128i*)(s + x * 3))), _mm_loadu_si128((const __m128i*)(s + x * 3 + 6)), 1);
				_mm256_storeu_si256((__m256i*)(d + x * 4), _mm256_or_si256(_mm256_shuffle_epi8(v, shuffle), alpha));
			}
			for (; x < n; ++x)
				d[x * 4] = s[x * 3], d[x * 4 + 1] = s[x * 3 + 1], d[x * 4 + 2] = s[x * 3 + 2], d[x * 4 + 3] = one;
		}

		void ExpandRgb(const float* s, int n, float one, float* d)
		{
			const __m128 a = _mm_set1_ps(one);
			int x = 0;
			for (; x + 2 <= n; ++x)
				_mm_storeu_ps(d + x * 4, _mm_blend_ps(_mm_loadu_ps(s + x * 3), a, 8));
			for (; x < n; ++x)
				d[x * 4] = s[x * 3], d[x * 4 + 1] = s[x * 3 + 1], d[x * 4 + 2] = s[x * 3 + 2], d[x * 4 + 3] = one;
		}

		// n source pixels (or samples of one plane) to RGBA
		template <class T> void ExpandRow(const T* s, int n, const Page& p, int plane, T one, T* d)
		{
			const bool gray = p.photometric == PHOTOMETRIC_MINISBLACK;
			const int spp = p.spp;
			if (plane >= 0)
			{
				for (int x = 0; x < n; ++x)
				{
					T* o = d + x * 4;
					if (gray)
					{
						if (plane == 0)
							o[0] = o[1] = o[2] = s[x];
						else if (plane == 1)
							o[3] = s[x];
						if (plane == 0 && spp < 2)
							o[3] = one;
					}
					else
					{
						if (plane < 4)
							o[plane] = s[x];
						if (plane == 0 && spp < 4)
							o[3] = one;
					}
				}
				return;
			}
			if (gray)
			{
				for (int x = 0; x < n; ++x)
					d[x * 4] = d[x * 4 + 1] = d[x * 4 + 2] = s[x * spp], d[x * 4 + 3] = spp > 1 ? s[x * spp + 1] : one;
			}
			else if (spp == 4)
				memcpy(d, s, (size_t)n * 4 * sizeof(T));
			else if (spp == 3)
			{
				if constexpr (sizeof(T) == 1)
					ExpandRgb(s, n, d);
				else
					ExpandRgb(s, n, one, d);
			}
			else
				for (int x = 0; x < n; ++x)
					memcpy(d + x * 4, s + x * spp, 4 * sizeof(T));
		}

		void Expand(const uint8_t* s, int n, const Page& p, int plane, uint8_t* d)
		{
			if (p.Float() && p.bps == 32)
				ExpandRow((const float*)s, n, p, plane, 1.0f, (float*)d);
			else if (p.bps == 16)
				ExpandRow((const uint16_t*)s, n, p, plane, (uint16_t)(p.Float() ? 0x3C00 : 0xFFFF), (uint16_t*)d);
			else
				ExpandRow(s, n, p, plane, (uint8_t)0xFF, d);
		}

		// EXIF-style orientation of a decoded page (tight rows of `bytes`-byte pixels) into the target
		void Orient(const uint8_t* src, int w, int h, int orientation, size_t bytes, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
		{
			const bool swap = orientation >= 5;
			const int dw = std::min(swap ? h : w, maxW), dh = std::min(swap ? w : h, maxH);
			par.For(dh, 32, [&](int64_t y0, int64_t y1, int)
			{
				for (int y = (int)y0; y < (int)y1; ++y)
					for (int x = 0; x < dw; ++x)
					{
						int sx = swap ? y : x, sy = swap ? x : y;
						if (orientation == 2 || orientation == 3 || orientation == 6 || orientation == 7)
							sx = w - 1 - sx;
						if (orientation == 3 || orientation == 4 || orientation == 7 || orientation == 8)
							sy = h - 1 - sy;
						memcpy(dst + (size_t)y * pitch + (size_t)x * bytes, src + ((size_t)sy * w + sx) * bytes, bytes);
					}
			});
		}

		// deflate strips or tiles of a contiguous little-endian page read straight from the file and inflated by libdeflate, predictors undone as libtiff's horAcc / fpAcc do (byte-identical to libtiff, ~1.5-2x faster)
		struct RawChunks
		{
			const uint64_t* offsets = nullptr;
			const uint64_t* counts = nullptr;
			uint16_t predictor = PREDICTOR_NONE;
			size_t chunkBytes = 0;
		};

		bool RawPossible(TIFF* t, const Page& p, size_t size, RawChunks& raw)
		{
			uint16_t compression = 0, fill = FILLORDER_MSB2LSB;
			TIFFGetFieldDefaulted(t, TIFFTAG_COMPRESSION, &compression);
			TIFFGetFieldDefaulted(t, TIFFTAG_FILLORDER, &fill);
			TIFFGetFieldDefaulted(t, TIFFTAG_PREDICTOR, &raw.predictor);
			if ((compression != COMPRESSION_ADOBE_DEFLATE && compression != COMPRESSION_DEFLATE) || fill != FILLORDER_MSB2LSB || TIFFIsByteSwapped(t) || p.planar != PLANARCONFIG_CONTIG)
				return false;
			const int b = p.bps / 8;
			const bool horizontal = raw.predictor == PREDICTOR_HORIZONTAL && p.format == SAMPLEFORMAT_UINT && (b == 1 || b == 2 || b == 4);
			const bool floating = raw.predictor == PREDICTOR_FLOATINGPOINT && p.format == SAMPLEFORMAT_IEEEFP;
			if (raw.predictor != PREDICTOR_NONE && !horizontal && !floating)
				return false;
			const uint32_t chunks = p.tiled ? TIFFNumberOfTiles(t) : TIFFNumberOfStrips(t);
			if (!TIFFGetField(t, p.tiled ? TIFFTAG_TILEOFFSETS : TIFFTAG_STRIPOFFSETS, &raw.offsets) || !TIFFGetField(t, p.tiled ? TIFFTAG_TILEBYTECOUNTS : TIFFTAG_STRIPBYTECOUNTS, &raw.counts) || !raw.offsets || !raw.counts)
				return false;
			for (uint32_t c = 0; c < chunks; ++c)
				if (raw.offsets[c] > size || raw.counts[c] > size - raw.offsets[c])
					return false;
			raw.chunkBytes = (size_t)(p.tiled ? TIFFTileSize(t) : TIFFStripSize(t));
			return raw.chunkBytes > 0;
		}

		// libtiff's horAcc8/16/32: running sums per sample position, kept in registers for the usual 1, 3 and 4 samples per pixel
		template <class T> void HorizontalUndo(uint8_t* row, size_t rowBytes, int spp)
		{
			T* s = (T*)row;
			const size_t n = rowBytes / sizeof(T);
			if (spp == 1)
			{
				T a = s[0];
				for (size_t i = 1; i < n; ++i)
					s[i] = a = (T)(a + s[i]);
			}
			else if (spp == 3)
			{
				T a = s[0], b = s[1], c = s[2];
				for (size_t i = 3; i + 3 <= n; i += 3)
					s[i] = a = (T)(a + s[i]), s[i + 1] = b = (T)(b + s[i + 1]), s[i + 2] = c = (T)(c + s[i + 2]);
			}
			else if (spp == 4)
			{
				T a = s[0], b = s[1], c = s[2], d = s[3];
				for (size_t i = 4; i + 4 <= n; i += 4)
					s[i] = a = (T)(a + s[i]), s[i + 1] = b = (T)(b + s[i + 1]), s[i + 2] = c = (T)(c + s[i + 2]), s[i + 3] = d = (T)(d + s[i + 3]);
			}
			else
				for (size_t i = spp; i < n; ++i)
					s[i] = (T)(s[i] + s[i - spp]);
		}

		// libtiff's fpAcc: byte-wise horizontal accumulation in place, then the byte planes (most significant first) interleaved into little-endian samples in out
		void FloatUndo(uint8_t* row, size_t rowBytes, int spp, int sampleBytes, uint8_t* out)
		{
			HorizontalUndo<uint8_t>(row, rowBytes, spp);
			const size_t wc = rowBytes / sampleBytes;
			size_t v = 0;
			if (sampleBytes == 4)
			{
				const uint8_t *p0 = row + 3 * wc, *p1 = row + 2 * wc, *p2 = row + wc, *p3 = row;
				for (; v + 32 <= wc; v += 32)
				{
					const __m256i a = _mm256_loadu_si256((const __m256i*)(p0 + v)), b = _mm256_loadu_si256((const __m256i*)(p1 + v)), c = _mm256_loadu_si256((const __m256i*)(p2 + v)), d = _mm256_loadu_si256((const __m256i*)(p3 + v));
					const __m256i abLo = _mm256_unpacklo_epi8(a, b), abHi = _mm256_unpackhi_epi8(a, b), cdLo = _mm256_unpacklo_epi8(c, d), cdHi = _mm256_unpackhi_epi8(c, d);
					const __m256i q0 = _mm256_unpacklo_epi16(abLo, cdLo), q1 = _mm256_unpackhi_epi16(abLo, cdLo), q2 = _mm256_unpacklo_epi16(abHi, cdHi), q3 = _mm256_unpackhi_epi16(abHi, cdHi);
					__m256i* o = (__m256i*)(out + v * 4);
					_mm256_storeu_si256(o, _mm256_permute2x128_si256(q0, q1, 0x20));
					_mm256_storeu_si256(o + 1, _mm256_permute2x128_si256(q2, q3, 0x20));
					_mm256_storeu_si256(o + 2, _mm256_permute2x128_si256(q0, q1, 0x31));
					_mm256_storeu_si256(o + 3, _mm256_permute2x128_si256(q2, q3, 0x31));
				}
			}
			else if (sampleBytes == 2)
			{
				const uint8_t *p0 = row + wc, *p1 = row;
				for (; v + 32 <= wc; v += 32)
				{
					const __m256i a = _mm256_loadu_si256((const __m256i*)(p0 + v)), b = _mm256_loadu_si256((const __m256i*)(p1 + v));
					const __m256i lo = _mm256_unpacklo_epi8(a, b), hi = _mm256_unpackhi_epi8(a, b);
					_mm256_storeu_si256((__m256i*)(out + v * 2), _mm256_permute2x128_si256(lo, hi, 0x20));
					_mm256_storeu_si256((__m256i*)(out + v * 2) + 1, _mm256_permute2x128_si256(lo, hi, 0x31));
				}
			}
			for (; v < wc; ++v)
				for (int b = 0; b < sampleBytes; ++b)
					out[v * sampleBytes + b] = row[(size_t)(sampleBytes - b - 1) * wc + v];
		}

		// a directly decodable page into the target (rows of `pitch`), strips or tiles handed out to the workers; libtiff decodes through one handle per worker
		std::string DecodeDirect(const uint8_t* data, size_t size, const Page& p, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
		{
			Handle first(data, size);
			if (!first.tif || !TIFFSetDirectory(first.tif, (tdir_t)p.dir))
				return first.Error();
			const bool separate = p.planar == PLANARCONFIG_SEPARATE;
			const uint32_t chunks = p.tiled ? TIFFNumberOfTiles(first.tif) : TIFFNumberOfStrips(first.tif);
			const uint32_t perPlane = separate ? chunks / std::max<uint32_t>(p.spp, 1) : chunks;
			const size_t sampleBytes = p.bps / 8, chunkSpp = separate ? 1 : p.spp, pixelBytes = p.PixelBytes();
			RawChunks raw;
			const bool direct = RawPossible(first.tif, p, size, raw);
			const size_t chunkBytes = direct ? raw.chunkBytes : (size_t)(p.tiled ? TIFFTileSize(first.tif) : TIFFStripSize(first.tif));
			const int workers = par.MaxWorkers();
			std::vector<std::unique_ptr<Handle>> handles(workers);
			std::vector<std::vector<uint8_t>> bufs(workers), tmps(workers);
			std::vector<libdeflate_decompressor*> inflaters(workers, nullptr);
			std::vector<std::string> errors(workers);
			const int64_t grain = std::max<int64_t>(1, chunks / ((int64_t)workers * 16));
			par.For(chunks, grain, [&](int64_t c0, int64_t c1, int worker)
			{
				std::vector<uint8_t>& buf = bufs[worker];
				if (buf.empty())
					buf.resize(chunkBytes);
				std::string& err = errors[worker];
				for (int64_t ci = c0; ci < c1 && err.empty(); ++ci)
				{
					const uint32_t c = (uint32_t)ci;
					const int plane = separate ? (int)(c / perPlane) : -1;
					const uint32_t k = c % perPlane;
					uint32_t x0 = 0, y0 = 0, cols = p.w, rows = 0, stride = p.w;
					if (p.tiled)
					{
						const uint32_t across = (p.w + p.tileW - 1) / p.tileW;
						x0 = (k % across) * p.tileW, y0 = (k / across) * p.tileH, cols = std::min(p.tileW, p.w - x0), rows = std::min(p.tileH, p.h - y0), stride = p.tileW;
					}
					else
						y0 = k * p.rowsPerStrip, rows = std::min(p.rowsPerStrip, p.h - std::min(y0, p.h));
					const size_t rowBytes = (size_t)stride * chunkSpp * sampleBytes;
					if (direct)
					{
						if (!inflaters[worker])
							inflaters[worker] = libdeflate_alloc_decompressor();
						// a tile always holds full tile rows; the last strip only its own rows
						const size_t want = p.tiled ? chunkBytes : (size_t)rows * rowBytes;
						size_t got = 0;
						if (!inflaters[worker] || libdeflate_zlib_decompress(inflaters[worker], data + raw.offsets[c], (size_t)raw.counts[c], buf.data(), want, &got) != LIBDEFLATE_SUCCESS || got != want)
						{
							err = "tiff: corrupt deflate data in chunk " + std::to_string(c);
							break;
						}
					}
					else
					{
						std::unique_ptr<Handle>& h = handles[worker];
						if (!h)
						{
							h = std::make_unique<Handle>(data, size);
							if (!h->tif || !TIFFSetDirectory(h->tif, (tdir_t)p.dir))
							{
								err = h->Error();
								break;
							}
						}
						const tmsize_t got = p.tiled ? TIFFReadEncodedTile(h->tif, c, buf.data(), (tmsize_t)buf.size()) : TIFFReadEncodedStrip(h->tif, c, buf.data(), (tmsize_t)buf.size());
						if (got < 0)
						{
							err = h->Error();
							break;
						}
					}
					for (uint32_t r = 0; r < rows; ++r)
					{
						const int y = (int)(y0 + r);
						if (y >= maxH || (int)x0 >= maxW)
							continue;
						uint8_t* row = buf.data() + (size_t)r * rowBytes;
						const uint8_t* src = row;
						// predictors are undone per row, only for rows that are kept
						if (direct && raw.predictor == PREDICTOR_FLOATINGPOINT)
						{
							std::vector<uint8_t>& tmp = tmps[worker];
							if (tmp.size() < rowBytes)
								tmp.resize(rowBytes);
							FloatUndo(row, rowBytes, p.spp, (int)sampleBytes, tmp.data());
							src = tmp.data();
						}
						else if (direct && raw.predictor == PREDICTOR_HORIZONTAL)
						{
							if (sampleBytes == 1)
								HorizontalUndo<uint8_t>(row, rowBytes, p.spp);
							else if (sampleBytes == 2)
								HorizontalUndo<uint16_t>(row, rowBytes, p.spp);
							else
								HorizontalUndo<uint32_t>(row, rowBytes, p.spp);
						}
						const int n = std::min((int)cols, maxW - (int)x0);
						if (p.Channels() == 3)
							memcpy(dst + (size_t)y * pitch + x0 * pixelBytes, src, (size_t)n * pixelBytes);
						else
							Expand(src, n, p, plane, dst + (size_t)y * pitch + x0 * pixelBytes);
					}
				}
			});
			for (libdeflate_decompressor* d : inflaters)
				if (d)
					libdeflate_free_decompressor(d);
			for (const std::string& e : errors)
				if (!e.empty())
					return e;
			return {};
		}
	}

	std::string ProbeTiff(const uint8_t* data, size_t size, Probe& out)
	{
		Handle h(data, size);
		std::vector<Page> pages;
		int mips = 1;
		if (std::string err = Pages(h, pages, out.w, out.h, mips); !err.empty())
			return err;
		out.bits = pages[0].Bits();
		out.channels = pages[0].Channels();
		out.floatSamples = pages[0].Float();
		if (mips > 1)
		{
			out.notes.push_back("mip-mapped, " + std::to_string(mips) + " levels");
			for (const Page& p : pages)
				out.mips.emplace_back(p.W(), p.H());
		}
		else if (pages.size() > 1)
			for (const Page& p : pages)
				out.notes.push_back(p.Note());
		return {};
	}

	std::string DecodeTiffPage(const uint8_t* data, size_t size, int page, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Handle h(data, size);
		std::vector<Page> pages;
		int cw = 0, ch = 0, mips = 1;
		if (std::string err = Pages(h, pages, cw, ch, mips); !err.empty())
			return err;
		if (page < 0 || page >= (int)pages.size())
			return "tiff: page " + std::to_string(page + 1) + " missing";
		const Page& p = pages[page];
		const size_t bytes = p.PixelBytes();
		const int pw = p.W(), ph = p.H();
		// a mip level is an image of its own size
		if (mips > 1)
			cw = pw, ch = ph;
		// a smaller page sits centred on a cleared canvas
		const int ox = (cw - pw) / 2, oy = (ch - ph) / 2;
		if (pw != cw || ph != ch)
			for (int y = 0; y < std::min(ch, maxH); ++y)
				memset(dst + (size_t)y * pitch, 0, (size_t)std::min(cw, maxW) * bytes);
		uint8_t* at = dst + (size_t)oy * pitch + (size_t)ox * bytes;
		const int roomW = maxW - ox, roomH = maxH - oy;
		if (!p.Direct())
		{
			if (!TIFFSetDirectory(h.tif, (tdir_t)p.dir))
				return h.Error();
			std::vector<uint32_t> raster((size_t)pw * ph);
			if (!TIFFReadRGBAImageOriented(h.tif, p.w, p.h, raster.data(), ORIENTATION_TOPLEFT, 0))
				return h.Error();
			CopyRows((const uint8_t*)raster.data(), pw, ph, at, pitch, roomW, roomH);
			return {};
		}
		if (p.orientation == ORIENTATION_TOPLEFT || p.orientation < 1 || p.orientation > 8)
			return DecodeDirect(data, size, p, at, pitch, roomW, roomH, par);
		std::vector<uint8_t> upright((size_t)p.w * p.h * bytes);
		if (std::string err = DecodeDirect(data, size, p, upright.data(), (size_t)p.w * bytes, (int)p.w, (int)p.h, par); !err.empty())
			return err;
		Orient(upright.data(), (int)p.w, (int)p.h, p.orientation, bytes, at, pitch, roomW, roomH, par);
		return {};
	}
}
