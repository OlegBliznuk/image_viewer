#include "image_exr.h"
#include "image_formats.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfMultiPartInputFile.h>
#include <OpenEXR/ImfPartType.h>
#include <OpenEXR/openexr.h>
#include <Imath/half.h>

#include <Windows.h>
#include <immintrin.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#pragma comment(lib, "OpenEXR-3_4.lib")
#pragma comment(lib, "OpenEXRCore-3_4.lib")
#pragma comment(lib, "Imath-3_2.lib")

namespace iv
{
	namespace
	{
		int CompIndex(const std::string& base, bool xyzw)
		{
			if (base.size() != 1)
				return -1;
			const char* letters = xyzw ? "xyzw" : "rgba";
			const char* hit = strchr(letters, tolower((unsigned char)base[0]));
			return hit && *hit ? int(hit - letters) : -1;
		}

		// the file behind the core context, read with positional reads on an overlapped handle: workers neither share the file-object lock of a synchronous handle
		// (the core's own reader: 1-line chunks no faster than the C++ API) nor fault on one mapped view (24 threads spent 4.6x the 1-thread copy time); the first error message is kept
		struct Stream
		{
			HANDLE file = INVALID_HANDLE_VALUE;
			uint64_t size = 0;
			std::mutex mutex;
			std::string error;

			explicit Stream(const std::wstring& path)
			{
				file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
				LARGE_INTEGER s = {};
				if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &s))
					size = (uint64_t)s.QuadPart;
			}
			~Stream()
			{
				if (file != INVALID_HANDLE_VALUE)
					CloseHandle(file);
			}
		};

		int64_t ReadStream(exr_const_context_t, void* user, void* buffer, uint64_t size, uint64_t offset, exr_stream_error_func_ptr_t)
		{
			const Stream& s = *(const Stream*)user;
			if (offset >= s.size)
				return 0;
			OVERLAPPED ov = {};
			ov.Offset = (DWORD)offset, ov.OffsetHigh = (DWORD)(offset >> 32);
			ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (!ov.hEvent)
				return -1;
			DWORD got = 0;
			BOOL ok = ReadFile(s.file, buffer, (DWORD)std::min<uint64_t>({ size, s.size - offset, 1u << 30 }), &got, &ov);
			if (!ok && GetLastError() == ERROR_IO_PENDING)
				ok = GetOverlappedResult(s.file, &ov, &got, TRUE);
			CloseHandle(ov.hEvent);
			return ok ? (int64_t)got : -1;
		}

		int64_t StreamSize(exr_const_context_t, void* user) { return (int64_t)((Stream*)user)->size; }

		void OnError(exr_const_context_t ctxt, exr_result_t, const char* msg)
		{
			void* user = nullptr;
			if (exr_get_user_data(ctxt, &user) != EXR_ERR_SUCCESS || !user)
				return;
			Stream& s = *(Stream*)user;
			std::lock_guard lock(s.mutex);
			if (s.error.empty() && msg)
				s.error = msg;
		}

		// ImfConvert's floatToHalf / uintToHalf (finite values past HALF_MAX become infinities), as the C++ API converts into a half slice
		uint16_t FloatToHalf(float f)
		{
			if (f == f && f > 65504.0f && f < INFINITY)
				return 0x7C00;
			if (f == f && f < -65504.0f && f > -INFINITY)
				return 0xFC00;
			return imath_float_to_half(f);
		}

		// where a part's chunks go in the frame: per output component the part channel it comes from (-1: filled)
		struct Target
		{
			int comps = 4;
			bool f32 = false;
			int chan[4] = { -1, -1, -1, -1 };
			uint16_t type[4] = {};   // EXR_PIXEL_* of the source channel
			uint16_t fillH[4] = {};
			float fillF[4] = {};
			bool direct = false;   // every source has the output's type: interleave without conversion
			uint8_t* base = nullptr;   // data-window pixel (x0, y0)
			size_t pitch = 0;
			size_t bpp = 0;
			int x0 = 0, y0 = 0, w = 0, h = 0;   // data window
			int tileW = 0, tileH = 0;   // tiled parts: chunk start_x / start_y are tile indices
		};

		// 16 half RGBA pixels per step
		void RowH4(const uint16_t* const s[4], const uint16_t fill[4], int w, uint16_t* out)
		{
			__m256i f[4];
			for (int k = 0; k < 4; ++k)
				f[k] = _mm256_set1_epi16((short)fill[k]);
			int x = 0;
			for (; x + 16 <= w; x += 16)
			{
				__m256i v[4];
				for (int k = 0; k < 4; ++k)
					v[k] = s[k] ? _mm256_loadu_si256((const __m256i*)(s[k] + x)) : f[k];
				const __m256i rgL = _mm256_unpacklo_epi16(v[0], v[1]), rgH = _mm256_unpackhi_epi16(v[0], v[1]);
				const __m256i baL = _mm256_unpacklo_epi16(v[2], v[3]), baH = _mm256_unpackhi_epi16(v[2], v[3]);
				const __m256i q0 = _mm256_unpacklo_epi32(rgL, baL), q1 = _mm256_unpackhi_epi32(rgL, baL);
				const __m256i q2 = _mm256_unpacklo_epi32(rgH, baH), q3 = _mm256_unpackhi_epi32(rgH, baH);
				__m256i* o = (__m256i*)(out + (size_t)x * 4);
				_mm256_storeu_si256(o, _mm256_permute2x128_si256(q0, q1, 0x20));
				_mm256_storeu_si256(o + 1, _mm256_permute2x128_si256(q2, q3, 0x20));
				_mm256_storeu_si256(o + 2, _mm256_permute2x128_si256(q0, q1, 0x31));
				_mm256_storeu_si256(o + 3, _mm256_permute2x128_si256(q2, q3, 0x31));
			}
			for (; x < w; ++x)
				for (int k = 0; k < 4; ++k)
					out[(size_t)x * 4 + k] = s[k] ? s[k][x] : fill[k];
		}

		// 8 half RGB pixels (6 bytes each) per step: four 12-byte groups stored 16 bytes wide, each store's last 4 bytes overwritten by the next;
		// the loop stops a pixel early so the last store of a step lands on a pixel this row still writes
		void RowH3(const uint16_t* const s[3], const uint16_t fill[3], int w, uint16_t* out)
		{
			const __m128i pack = _mm_setr_epi8(0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, -1, -1, -1, -1), z = _mm_setzero_si128();
			__m128i f[3];
			for (int k = 0; k < 3; ++k)
				f[k] = _mm_set1_epi16((short)fill[k]);
			int x = 0;
			for (; x + 8 < w; x += 8)
			{
				__m128i v[3];
				for (int k = 0; k < 3; ++k)
					v[k] = s[k] ? _mm_loadu_si128((const __m128i*)(s[k] + x)) : f[k];
				const __m128i rg0 = _mm_unpacklo_epi16(v[0], v[1]), rg1 = _mm_unpackhi_epi16(v[0], v[1]);
				const __m128i b0 = _mm_unpacklo_epi16(v[2], z), b1 = _mm_unpackhi_epi16(v[2], z);
				uint8_t* o = (uint8_t*)(out + (size_t)x * 3);
				_mm_storeu_si128((__m128i*)o, _mm_shuffle_epi8(_mm_unpacklo_epi32(rg0, b0), pack));
				_mm_storeu_si128((__m128i*)(o + 12), _mm_shuffle_epi8(_mm_unpackhi_epi32(rg0, b0), pack));
				_mm_storeu_si128((__m128i*)(o + 24), _mm_shuffle_epi8(_mm_unpacklo_epi32(rg1, b1), pack));
				_mm_storeu_si128((__m128i*)(o + 36), _mm_shuffle_epi8(_mm_unpackhi_epi32(rg1, b1), pack));
			}
			for (; x < w; ++x)
				for (int k = 0; k < 3; ++k)
					out[(size_t)x * 3 + k] = s[k] ? s[k][x] : fill[k];
		}

		// 8 float RGBA pixels per step (4 x 8 transpose)
		void RowF4(const float* const s[4], const float fill[4], int w, float* out)
		{
			__m256 f[4];
			for (int k = 0; k < 4; ++k)
				f[k] = _mm256_set1_ps(fill[k]);
			int x = 0;
			for (; x + 8 <= w; x += 8)
			{
				__m256 v[4];
				for (int k = 0; k < 4; ++k)
					v[k] = s[k] ? _mm256_loadu_ps(s[k] + x) : f[k];
				const __m256 t0 = _mm256_unpacklo_ps(v[0], v[1]), t1 = _mm256_unpackhi_ps(v[0], v[1]);
				const __m256 t2 = _mm256_unpacklo_ps(v[2], v[3]), t3 = _mm256_unpackhi_ps(v[2], v[3]);
				const __m256 p0 = _mm256_shuffle_ps(t0, t2, 0x44), p1 = _mm256_shuffle_ps(t0, t2, 0xEE);
				const __m256 p2 = _mm256_shuffle_ps(t1, t3, 0x44), p3 = _mm256_shuffle_ps(t1, t3, 0xEE);
				float* o = out + (size_t)x * 4;
				_mm256_storeu_ps(o, _mm256_permute2f128_ps(p0, p1, 0x20));
				_mm256_storeu_ps(o + 8, _mm256_permute2f128_ps(p2, p3, 0x20));
				_mm256_storeu_ps(o + 16, _mm256_permute2f128_ps(p0, p1, 0x31));
				_mm256_storeu_ps(o + 24, _mm256_permute2f128_ps(p2, p3, 0x31));
			}
			for (; x < w; ++x)
				for (int k = 0; k < 4; ++k)
					out[(size_t)x * 4 + k] = s[k] ? s[k][x] : fill[k];
		}

		// any mix of source types, converted as the C++ API converts into a slice
		void RowConvert(const Target& t, const uint8_t* const s[4], int w, uint8_t* out)
		{
			for (int k = 0; k < t.comps; ++k)
			{
				const uint8_t* p = s[k];
				for (int x = 0; x < w; ++x)
				{
					uint8_t* o = out + (size_t)x * t.bpp;
					if (t.f32)
					{
						float v = t.fillF[k];
						if (p)
							v = t.type[k] == EXR_PIXEL_HALF ? imath_half_to_float(((const uint16_t*)p)[x]) : t.type[k] == EXR_PIXEL_FLOAT ? ((const float*)p)[x] : (float)((const uint32_t*)p)[x];
						((float*)o)[k] = v;
					}
					else
					{
						uint16_t v = t.fillH[k];
						if (p && t.type[k] == EXR_PIXEL_HALF)
							v = ((const uint16_t*)p)[x];
						else if (p && t.type[k] == EXR_PIXEL_FLOAT)
							v = FloatToHalf(((const float*)p)[x]);
						else if (p)
							v = ((const uint32_t*)p)[x] > 65504u ? 0x7C00 : imath_float_to_half((float)((const uint32_t*)p)[x]);
						((uint16_t*)o)[k] = v;
					}
				}
			}
		}

		void Row(const Target& t, const uint8_t* const s[4], int w, uint8_t* out)
		{
			if (!t.direct)
				return RowConvert(t, s, w, out);
			if (t.comps == 4)
				return t.f32 ? RowF4((const float* const*)s, t.fillF, w, (float*)out) : RowH4((const uint16_t* const*)s, t.fillH, w, (uint16_t*)out);
			if (t.comps == 3)
				return RowH3((const uint16_t* const*)s, t.fillH, w, (uint16_t*)out);
			const size_t bytes = t.f32 ? 4 : 2;
			if (s[0])
				memcpy(out, s[0], (size_t)w * bytes);
			else if (t.f32)
				std::fill((float*)out, (float*)out + w, t.fillF[0]);
			else
				std::fill((uint16_t*)out, (uint16_t*)out + w, t.fillH[0]);
		}

		// unpack_and_convert_fn: the chunk's decoded lines (per line, each channel's samples in channel-list order) into the frame
		exr_result_t Unpack(exr_decode_pipeline_t* d)
		{
			const Target& t = *(const Target*)d->decoding_user_data;
			const int w = d->chunk.width, h = d->chunk.height;
			const int px = t.tileW ? t.x0 + d->chunk.start_x * t.tileW : d->chunk.start_x;
			const int py = t.tileW ? t.y0 + d->chunk.start_y * t.tileH : d->chunk.start_y;
			if (px < t.x0 || py < t.y0 || w < 0 || h < 0 || px + w > t.x0 + t.w || py + h > t.y0 + t.h)
				return EXR_ERR_CORRUPT_CHUNK;
			const uint8_t* line = (const uint8_t*)d->unpacked_buffer;
			const uint8_t* end = line + d->chunk.unpacked_size;
			for (int yy = 0; yy < h; ++yy)
			{
				const int y = py + yy;
				const uint8_t* src[4] = {};
				const uint8_t* at = line;
				for (int j = 0; j < d->channel_count; ++j)
				{
					const exr_coding_channel_info_t& c = d->channels[j];
					if (y % std::max<int>(c.y_samples, 1))
						continue;
					for (int k = 0; k < t.comps; ++k)
						if (t.chan[k] == j)
							src[k] = at;
					at += (size_t)c.width * c.bytes_per_element;
				}
				if (at > end)
					return EXR_ERR_CORRUPT_CHUNK;
				Row(t, src, w, t.base + (size_t)(y - t.y0) * t.pitch + (size_t)(px - t.x0) * t.bpp);
				line = at;
			}
			return EXR_ERR_SUCCESS;
		}

	}

	std::string ProbeExr(const std::string& path, ImageInfo& out)
	{
		Imf::MultiPartInputFile file(path.c_str());
		const Imath::Box2i disp = file.header(0).displayWindow();
		out.kind = FileKind::Exr;
		out.width = disp.max.x - disp.min.x + 1;
		out.height = disp.max.y - disp.min.y + 1;
		const int parts = file.parts();
		std::vector<ViewDesc> groups, singles;
		for (int p = 0; p < parts; ++p)
		{
			const Imf::Header& h = file.header(p);
			if (h.hasType() && Imf::isDeepData(h.type()))
				continue;
			const std::string partName = h.hasName() ? h.name() : std::string();
			const std::string prefix = parts > 1 ? (partName.empty() ? "part " + std::to_string(p) : partName) + " / " : std::string();
			struct Chan { std::string base, full; bool float32; const char* type; };
			std::map<std::string, std::vector<Chan>> layers;
			for (auto it = h.channels().begin(); it != h.channels().end(); ++it)
			{
				const std::string full = it.name();
				const size_t dot = full.rfind('.');
				const Imf::PixelType pt = it.channel().type;
				const char* type = pt == Imf::HALF ? "16fp" : pt == Imf::FLOAT ? "32fp" : "32u";
				layers[dot == std::string::npos ? std::string() : full.substr(0, dot)].push_back({ dot == std::string::npos ? full : full.substr(dot + 1), full, pt != Imf::HALF, type });
				ViewDesc single;
				single.label = prefix + full;
				single.part = p;
				single.partName = partName;
				single.chans[0] = full;
				single.nbComps = 1;
				single.float32 = pt != Imf::HALF;
				single.type = type;
				singles.push_back(single);
			}
			for (auto& [layer, chans] : layers)
				for (int xyzw = 0; xyzw < (layer.empty() ? 1 : 2); ++xyzw)
				{
					ViewDesc v;
					bool color = false;
					for (const Chan& c : chans)
					{
						const int idx = CompIndex(c.base, xyzw != 0);
						if (idx < 0 || !v.chans[idx].empty())
							continue;
						v.chans[idx] = c.full;
						v.float32 |= c.float32;
						color |= idx < 3;
						if (v.type.find(c.type) == std::string::npos)
							v.type += (v.type.empty() ? "" : "/") + std::string(c.type);
					}
					if (!color)
						continue;
					std::string suffix;
					for (int c = 0; c < 4; ++c)
						if (!v.chans[c].empty())
							suffix += (xyzw ? "XYZW" : "RGBA")[c];
					v.label = prefix + (layer.empty() ? suffix : layer + "." + suffix);
					v.part = p;
					v.partName = partName;
					groups.push_back(v);
					break;
				}
		}
		if (groups.empty() && singles.empty())
			return "no displayable channels";
		out.views = groups;
		out.views.insert(out.views.end(), singles.begin(), singles.end());
		out.firstSingle = (int)groups.size();
		out.defaultView = 0;
		// tiled mip levels of the first part (a ripmap's levels of equal x and y), sized from the header by OpenEXR's rules; level 0 shows the display window as before
		const Imf::Header& h0 = file.header(0);
		if (h0.hasTileDescription() && h0.tileDescription().mode != Imf::ONE_LEVEL && !(h0.hasType() && Imf::isDeepData(h0.type())))
		{
			const Imf::TileDescription& td = h0.tileDescription();
			const bool up = td.roundingMode == Imf::ROUND_UP;
			const Imath::Box2i dw = h0.dataWindow();
			const int w = dw.max.x - dw.min.x + 1, h = dw.max.y - dw.min.y + 1;
			const auto count = [up](int size) { int n = 0; while ((1 << (n + 1)) <= size) ++n; return n + 1 + (up && (1 << (n)) < size ? 1 : 0); };
			const auto sized = [up](int size, int l) { return std::max(up ? (size + (1 << l) - 1) >> l : size >> l, 1); };
			const int levels = td.mode == Imf::MIPMAP_LEVELS ? count(std::max(w, h)) : std::min(count(w), count(h));
			if (levels > 1)
			{
				out.mips.emplace_back(out.width, out.height);
				for (int l = 1; l < levels; ++l)
					out.mips.emplace_back(sized(w, l), sized(h, l));
				out.notes = { "mip-mapped, " + std::to_string(levels) + " levels" };
			}
		}
		return {};
	}

	std::string DecodeExr(const std::wstring& path, const ViewDesc& view, const Layout& layout, uint8_t* dst, const Parallel& par)
	{
		Stream stream(path);
		if (stream.file == INVALID_HANDLE_VALUE)
			return "cannot open file";
		exr_context_initializer_t init = EXR_DEFAULT_CONTEXT_INITIALIZER;
		init.user_data = &stream;
		init.read_fn = ReadStream;
		init.size_fn = StreamSize;
		init.error_handler_fn = OnError;
		exr_context_t ctx = nullptr;
		const auto failed = [&](const char* what) { std::lock_guard lock(stream.mutex); return stream.error.empty() ? std::string(what) : stream.error; };
		if (exr_start_read(&ctx, ToUtf8(path).c_str(), &init) != EXR_ERR_SUCCESS)
		{
			exr_finish(&ctx);
			return failed("cannot read the EXR header");
		}
		struct Finish { exr_context_t& c; ~Finish() { exr_finish(&c); } } finish{ ctx };
		int parts = 0, p = -1;
		exr_get_count(ctx, &parts);
		for (int i = 0; i < parts && p < 0 && !view.partName.empty(); ++i)
		{
			const char* name = nullptr;
			if (exr_get_name(ctx, i, &name) == EXR_ERR_SUCCESS && name && view.partName == name)
				p = i;
		}
		if (p < 0)
			p = view.part < parts ? view.part : 0;
		exr_storage_t storage = EXR_STORAGE_SCANLINE;
		exr_attr_box2i_t dw = {}, disp = {};
		const exr_attr_chlist_t* channels = nullptr;
		if (exr_get_storage(ctx, p, &storage) != EXR_ERR_SUCCESS || exr_get_data_window(ctx, p, &dw) != EXR_ERR_SUCCESS || exr_get_display_window(ctx, p, &disp) != EXR_ERR_SUCCESS || exr_get_channels(ctx, p, &channels) != EXR_ERR_SUCCESS)
			return failed("cannot read the EXR part header");
		if (storage == EXR_STORAGE_DEEP_SCANLINE || storage == EXR_STORAGE_DEEP_TILED)
			return "deep data is not supported";
		// a mip level: its data window from the part's origin, shown on its own
		const int level = view.mip;
		if (level > 0)
		{
			int32_t lw = 0, lh = 0;
			if (storage != EXR_STORAGE_TILED || exr_get_level_sizes(ctx, p, level, level, &lw, &lh) != EXR_ERR_SUCCESS)
				return "no mip level " + std::to_string(level);
			dw.max.x = dw.min.x + lw - 1, dw.max.y = dw.min.y + lh - 1;
			disp = dw;
		}

		Target t;
		t.f32 = layout.format == DXGI_FORMAT_R32_FLOAT || layout.format == DXGI_FORMAT_R32G32B32A32_FLOAT;
		t.comps = layout.packing == Packing::Rgb ? 3 : view.nbComps;
		t.bpp = layout.bytesPerPixel;
		t.direct = true;
		for (int c = 0; c < t.comps; ++c)
		{
			const double fill = view.nbComps == 4 && c == 3 ? 1.0 : 0.0;
			t.fillF[c] = (float)fill;
			t.fillH[c] = imath_float_to_half((float)fill);
			for (int i = 0; i < channels->num_channels && !view.chans[c].empty(); ++i)
				if (view.chans[c] == channels->entries[i].name.str)
				{
					const exr_attr_chlist_entry_t& e = channels->entries[i];
					if (e.x_sampling != 1 || e.y_sampling != 1)
						return "subsampled channels are not supported";
					t.chan[c] = i;
					t.type[c] = (uint16_t)e.pixel_type;
					t.direct &= e.pixel_type == (t.f32 ? EXR_PIXEL_FLOAT : EXR_PIXEL_HALF);
				}
		}

		const int dwW = dw.max.x - dw.min.x + 1, dwH = dw.max.y - dw.min.y + 1;
		const int ox = dw.min.x - disp.min.x, oy = dw.min.y - disp.min.y;
		const bool inside = ox >= 0 && oy >= 0 && ox + dwW <= layout.width && oy + dwH <= layout.height;
		const bool full = inside && ox == 0 && oy == 0 && dwW == layout.width && dwH == layout.height;
		if (dwW <= 0 || dwH <= 0)
			return "empty data window";
		if (!full)
			memset(dst, 0, layout.size);
		std::vector<uint8_t> tmp;
		t.pitch = layout.rowPitch;
		t.base = inside ? dst + (size_t)oy * layout.rowPitch + (size_t)ox * t.bpp : nullptr;
		if (!inside)
		{
			t.pitch = (size_t)dwW * t.bpp;
			tmp.resize(t.pitch * dwH);
			t.base = tmp.data();
		}
		t.x0 = dw.min.x, t.y0 = dw.min.y, t.w = dwW, t.h = dwH;

		const bool tiled = storage == EXR_STORAGE_TILED;
		int32_t tilesX = 0, tilesY = 0, lines = 1;
		int64_t chunks = 0;
		if (tiled)
		{
			uint32_t tw = 0, th = 0;
			exr_tile_level_mode_t levels;
			exr_tile_round_mode_t round;
			if (exr_get_tile_descriptor(ctx, p, &tw, &th, &levels, &round) != EXR_ERR_SUCCESS || exr_get_tile_counts(ctx, p, level, level, &tilesX, &tilesY) != EXR_ERR_SUCCESS)
				return failed("cannot read the EXR tile layout");
			t.tileW = (int)tw, t.tileH = (int)th;
			chunks = (int64_t)tilesX * tilesY;
		}
		else
		{
			if (exr_get_scanlines_per_chunk(ctx, p, &lines) != EXR_ERR_SUCCESS || lines <= 0)
				return failed("cannot read the EXR chunk layout");
			chunks = (dwH + lines - 1) / lines;
		}

		// one pipeline per worker, set up at its first chunk and updated for the next (buffers and routines reused); pipelines must not move once set up
		const int workers = par.MaxWorkers();
		std::vector<exr_decode_pipeline_t> pipes(workers);
		std::vector<char> ready(workers, 0), bad(workers, 0);
		par.For(chunks, std::max<int64_t>(1, chunks / ((int64_t)workers * 8)), [&](int64_t a, int64_t b, int worker)
		{
			exr_decode_pipeline_t& d = pipes[worker];
			for (int64_t i = a; i < b && !bad[worker]; ++i)
			{
				exr_chunk_info_t ci;
				exr_result_t rv = tiled ? exr_read_tile_chunk_info(ctx, p, (int)(i % tilesX), (int)(i / tilesX), level, level, &ci) : exr_read_scanline_chunk_info(ctx, p, dw.min.y + (int)i * lines, &ci);
				if (rv == EXR_ERR_SUCCESS && !ready[worker])
				{
					rv = exr_decoding_initialize(ctx, p, &ci, &d);
					if (rv == EXR_ERR_SUCCESS)
						rv = exr_decoding_choose_default_routines(ctx, p, &d);
					ready[worker] = 1;
					d.decoding_user_data = &t;
					d.unpack_and_convert_fn = Unpack;
				}
				else if (rv == EXR_ERR_SUCCESS)
					rv = exr_decoding_update(ctx, p, &ci, &d);
				if (rv == EXR_ERR_SUCCESS)
					rv = exr_decoding_run(ctx, p, &d);
				bad[worker] = rv != EXR_ERR_SUCCESS;
			}
		});
		for (int i = 0; i < workers; ++i)
			if (ready[i])
				exr_decoding_destroy(ctx, &pipes[i]);
		if (std::any_of(bad.begin(), bad.end(), [](char v) { return v != 0; }))
			return failed("EXR decode error");
		if (!inside)
		{
			const int x0 = std::max(ox, 0), x1 = std::min(ox + dwW, layout.width);
			const int y0 = std::max(oy, 0), y1 = std::min(oy + dwH, layout.height);
			for (int y = y0; y < y1 && x1 > x0; ++y)
				memcpy(dst + (size_t)y * layout.rowPitch + (size_t)x0 * t.bpp, tmp.data() + (size_t)(y - oy) * t.pitch + (size_t)(x0 - ox) * t.bpp, (size_t)(x1 - x0) * t.bpp);
		}
		return {};
	}
}
