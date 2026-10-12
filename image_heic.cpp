#include "image_formats.h"

#include <libheif/heif.h>
#include <libheif/heif_items.h>
#include <libheif/heif_properties.h>
#include <libheif/heif_tiling.h>

#include <immintrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>

#pragma comment(lib, "heif.lib")

// HEIC / HEIF through libheif (HEVC by libde265, heif.dll delay-loaded): the primary image, rotated and mirrored as stored
namespace iv::fmt
{
	namespace
	{
		struct Heif
		{
			heif_context* ctx = heif_context_alloc();
			heif_image_handle* handle = nullptr;
			heif_image* image = nullptr;

			~Heif()
			{
				if (image)
					heif_image_release(image);
				if (handle)
					heif_image_handle_release(handle);
				heif_context_free(ctx);
			}

			std::string Open(const uint8_t* data, size_t size)
			{
				heif_error e = heif_context_read_from_memory_without_copy(ctx, data, size, nullptr);
				if (e.code == heif_error_Ok)
					e = heif_context_get_primary_image_handle(ctx, &handle);
				return e.code == heif_error_Ok ? std::string() : std::string("heic: ") + e.message;
			}
		};

		void Colors(heif_image_handle* h, Probe& out)
		{
			const heif_color_profile_type type = heif_image_handle_get_color_profile_type(h);
			if (type == heif_color_profile_type_prof || type == heif_color_profile_type_rICC)
			{
				std::vector<uint8_t> icc(heif_image_handle_get_raw_color_profile_size(h));
				if (!icc.empty() && heif_image_handle_get_raw_color_profile(h, icc.data()).code == heif_error_Ok)
					out.colorMatrix = IccToRec709(icc.data(), icc.size(), out.toRec709);
			}
			else if (type == heif_color_profile_type_nclx)
			{
				heif_color_profile_nclx* nclx = nullptr;
				if (heif_image_handle_get_nclx_color_profile(h, &nclx).code == heif_error_Ok && nclx)
				{
					const float xy[8] = { nclx->color_primary_red_x, nclx->color_primary_red_y, nclx->color_primary_green_x, nclx->color_primary_green_y, nclx->color_primary_blue_x, nclx->color_primary_blue_y, nclx->color_primary_white_x, nclx->color_primary_white_y };
					out.colorMatrix = PrimariesToRec709(xy, out.toRec709);
					heif_nclx_color_profile_free(nclx);
				}
			}
		}

		// `bits`-bit samples in 16-bit words to the full 16-bit range by bit replication (max stays max), 16 per step
		void Widen(const uint16_t* src, int count, int bits, uint16_t* dst)
		{
			const int up = 16 - bits, down = bits - up;
			int i = 0;
			for (; i + 16 <= count; i += 16)
			{
				const __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
				_mm256_storeu_si256((__m256i*)(dst + i), _mm256_or_si256(_mm256_slli_epi16(v, up), _mm256_srli_epi16(v, down)));
			}
			for (; i < count; ++i)
				dst[i] = uint16_t(src[i] << up | src[i] >> down);
		}

		constexpr uint32_t Tag(const char (&s)[5]) { return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3]; }

		// child boxes of [b, e) as fn(type, body, end); fn returns false to stop
		template <class F> void Boxes(const uint8_t* p, size_t b, size_t e, F&& fn)
		{
			while (b + 8 <= e)
			{
				uint64_t size = Be32(p + b);
				size_t body = b + 8;
				if (size == 1)
				{
					if (b + 16 > e)
						return;
					size = (uint64_t)Be32(p + b + 8) << 32 | Be32(p + b + 12), body = b + 16;
				}
				else if (size == 0)
					size = e - b;
				if (size < body - b || size > e - b || !fn(Be32(p + b + 4), body, b + (size_t)size))
					return;
				b += (size_t)size;
			}
		}

		// the hvcC among the item's properties (meta / iprp: ipma indexes the ipco children from 1)
		bool FindHvcc(const uint8_t* p, size_t n, uint32_t item, size_t& hb, size_t& he)
		{
			size_t ipco = 0, ipcoEnd = 0, ipma = 0, ipmaEnd = 0;
			Boxes(p, 0, n, [&](uint32_t t, size_t b, size_t e)
			{
				if (t != Tag("meta"))
					return true;
				Boxes(p, b + 4, e, [&](uint32_t t2, size_t b2, size_t e2)
				{
					if (t2 == Tag("iprp"))
						Boxes(p, b2, e2, [&](uint32_t t3, size_t b3, size_t e3)
						{
							if (t3 == Tag("ipco"))
								ipco = b3, ipcoEnd = e3;
							else if (t3 == Tag("ipma") && !ipma)
								ipma = b3, ipmaEnd = e3;
							return true;
						});
					return true;
				});
				return false;
			});
			if (!ipco || ipmaEnd < ipma + 8)
				return false;
			const int version = p[ipma];
			const bool wide = p[ipma + 3] & 1;
			size_t o = ipma + 8;
			for (uint32_t i = 0, count = Be32(p + ipma + 4); i < count; ++i)
			{
				if (o + (version < 1 ? 3 : 5) > ipmaEnd)
					return false;
				const uint32_t id = version < 1 ? uint32_t(p[o] << 8 | p[o + 1]) : Be32(p + o);
				o += version < 1 ? 2 : 4;
				const int assoc = p[o++];
				for (int a = 0; a < assoc; ++a)
				{
					if (o + (wide ? 2 : 1) > ipmaEnd)
						return false;
					const int index = wide ? (p[o] & 0x7f) << 8 | p[o + 1] : p[o] & 0x7f;
					o += wide ? 2 : 1;
					if (id != item || index == 0)
						continue;
					int k = 0;
					bool hit = false;
					Boxes(p, ipco, ipcoEnd, [&](uint32_t t, size_t b, size_t e)
					{
						if (++k != index)
							return true;
						if (t == Tag("hvcC"))
							hb = b, he = e, hit = true;
						return false;
					});
					if (hit)
						return true;
				}
			}
			return false;
		}

		struct Bits
		{
			std::vector<uint8_t> rbsp;
			size_t pos = 0;
			bool bad = false;

			uint32_t U(int n)
			{
				uint32_t v = 0;
				for (int i = 0; i < n; ++i, ++pos)
				{
					if (pos >= rbsp.size() * 8)
						return bad = true, 0;
					v = v << 1 | (rbsp[pos >> 3] >> (7 - (pos & 7)) & 1);
				}
				return v;
			}
			uint32_t Ue()
			{
				int zeros = 0;
				while (!U(1))
					if (bad || ++zeros > 31)
						return bad = true, 0;
				return zeros ? (1u << zeros) - 1 + U(zeros) : 0;
			}
		};

		// what libde265 reads from the SPS VUI (defaults without one, reserved values remapped as vui.cc does); false when the SPS cannot be walked
		bool SpsColour(const uint8_t* nal, size_t n, heif_color_profile_nclx& out)
		{
			Bits b;
			for (size_t i = 2, zeros = 0; i < n; ++i)
			{
				if (zeros >= 2 && nal[i] == 3)
				{
					zeros = 0;
					continue;
				}
				b.rbsp.push_back(nal[i]);
				zeros = nal[i] ? 0 : zeros + 1;
			}
			b.U(4);
			const uint32_t subLayers = b.U(3);
			b.U(1);
			b.U(32), b.U(32), b.U(32);
			bool profile[8] = {}, level[8] = {};
			for (uint32_t i = 0; i < subLayers; ++i)
				profile[i] = b.U(1), level[i] = b.U(1);
			for (uint32_t i = subLayers; subLayers && i < 8; ++i)
				b.U(2);
			for (uint32_t i = 0; i < subLayers; ++i)
			{
				if (profile[i])
					b.U(32), b.U(32), b.U(24);
				if (level[i])
					b.U(8);
			}
			b.Ue();
			if (b.Ue() == 3)
				b.U(1);
			b.Ue(), b.Ue();
			if (b.U(1))
				b.Ue(), b.Ue(), b.Ue(), b.Ue();
			b.Ue(), b.Ue();
			const uint32_t pocBits = b.Ue() + 4;
			for (uint32_t i = b.U(1) ? 0 : subLayers; i <= subLayers && !b.bad; ++i)
				b.Ue(), b.Ue(), b.Ue();
			for (int i = 0; i < 6; ++i)
				b.Ue();
			if (b.U(1) && b.U(1))
			{
				for (int size = 0; size < 4; ++size)
					for (int matrix = 0; matrix < 6 && !b.bad; matrix += size == 3 ? 3 : 1)
					{
						if (!b.U(1))
							b.Ue();
						else
							for (int k = 0, coefs = std::min(64, 1 << (4 + (size << 1))) + (size > 1); k < coefs && !b.bad; ++k)
								b.Ue();
					}
			}
			b.U(1), b.U(1);
			if (b.U(1))
				b.U(4), b.U(4), b.Ue(), b.Ue(), b.U(1);
			const uint32_t sets = b.Ue();
			if (sets > 64)
				return false;
			uint32_t pocs[64] = {};
			for (uint32_t s = 0; s < sets && !b.bad; ++s)
				if (s && b.U(1))
				{
					b.U(1), b.Ue();
					for (uint32_t j = 0; j <= pocs[s - 1] && !b.bad; ++j)
						pocs[s] += b.U(1) || b.U(1);
				}
				else
				{
					const uint32_t neg = b.Ue(), pos = b.Ue();
					if (neg > 16 || pos > 16)
						return false;
					for (uint32_t k = 0; k < neg + pos; ++k)
						b.Ue(), b.U(1);
					pocs[s] = neg + pos;
				}
			if (b.U(1))
			{
				const uint32_t lt = b.Ue();
				if (lt > 32)
					return false;
				for (uint32_t k = 0; k < lt; ++k)
					b.U((int)pocBits), b.U(1);
			}
			b.U(1), b.U(1);
			out.color_primaries = (heif_color_primaries)2;
			out.transfer_characteristics = (heif_transfer_characteristics)2;
			out.matrix_coefficients = (heif_matrix_coefficients)2;
			out.full_range_flag = 0;
			if (b.U(1))
			{
				if (b.U(1) && b.U(8) == 255)
					b.U(16), b.U(16);
				if (b.U(1))
					b.U(1);
				if (b.U(1))
				{
					b.U(3);
					out.full_range_flag = (uint8_t)b.U(1);
					if (b.U(1))
					{
						const uint32_t cp = b.U(8), tc = b.U(8), mc = b.U(8);
						out.color_primaries = (heif_color_primaries)(cp == 0 || cp == 3 || cp >= 11 ? 2 : cp);
						out.transfer_characteristics = (heif_transfer_characteristics)(tc == 0 || tc == 3 || tc >= 18 ? 2 : tc);
						out.matrix_coefficients = (heif_matrix_coefficients)(mc >= 11 ? 2 : mc);
					}
				}
			}
			return !b.bad;
		}

		bool HvccColour(const uint8_t* p, size_t b, size_t e, heif_color_profile_nclx& out)
		{
			if (e < b + 23)
				return false;
			size_t o = b + 23;
			for (int a = 0, arrays = p[b + 22]; a < arrays; ++a)
			{
				if (o + 3 > e)
					return false;
				const int type = p[o] & 0x3f, count = p[o + 1] << 8 | p[o + 2];
				o += 3;
				for (int k = 0; k < count; ++k)
				{
					if (o + 2 > e)
						return false;
					const size_t len = (size_t)(p[o] << 8 | p[o + 1]);
					o += 2;
					if (o + len > e)
						return false;
					if (type == 33)
						return SpsColour(p + o, len, out);
					o += len;
				}
			}
			return false;
		}

		// planes the frame can keep with libheif's own RGBA reproduced exactly: 8-bit 4:2:0 without alpha, full range, a matrix its nearest-chroma 8.8 conversion
		// takes (Op_YCbCr420_to_RGB32), and no rotation, mirror or crop that sends libheif through 4:4:4 first (odd sizes, odd crop offsets);
		// nclx is the image's colour signalling (the colr box, else the SPS VUI), asked back from libheif so it hands the planes over unconverted
		bool YuvPlanes(const Heif& h, const uint8_t* data, size_t size, heif_color_profile_nclx& nclx)
		{
			heif_colorspace cs;
			heif_chroma ch;
			if (heif_image_handle_get_luma_bits_per_pixel(h.handle) != 8 || heif_image_handle_get_chroma_bits_per_pixel(h.handle) != 8 || heif_image_handle_has_alpha_channel(h.handle))
				return false;
			if (heif_image_handle_get_preferred_decoding_colorspace(h.handle, &cs, &ch).code != heif_error_Ok || cs != heif_colorspace_YCbCr || ch != heif_chroma_420)
				return false;
			const heif_item_id item = heif_image_handle_get_item_id(h.handle);
			nclx = {};
			nclx.version = 1;
			heif_color_profile_nclx* box = nullptr;
			if (heif_image_handle_get_nclx_color_profile(h.handle, &box).code == heif_error_Ok && box)
			{
				nclx = *box;
				heif_nclx_color_profile_free(box);
			}
			else
			{
				heif_item_id coded = item;
				size_t hb = 0, he = 0;
				if (heif_item_get_item_type(h.ctx, item) == heif_fourcc('g', 'r', 'i', 'd') && heif_image_handle_get_grid_image_tile_id(h.handle, 0, 0, 0, &coded).code != heif_error_Ok)
					return false;
				if (!FindHvcc(data, size, coded, hb, he) || !HvccColour(data, hb, he, nclx))
					return false;
			}
			const int m = nclx.matrix_coefficients;
			if (!nclx.full_range_flag || !(m == 1 || m == 2 || (m >= 4 && m <= 7) || m == 9 || m == 10))
				return false;
			int w = heif_image_handle_get_ispe_width(h.handle), hh = heif_image_handle_get_ispe_height(h.handle);
			heif_property_id props[16];
			const int n = heif_item_get_transformation_properties(h.ctx, item, props, 16);
			for (int i = 0; i < n && n <= 16; ++i)
				switch (heif_item_get_property_type(h.ctx, item, props[i]))
				{
				case heif_item_property_type_transform_rotation:
				{
					const int a = heif_item_get_property_transform_rotation_ccw(h.ctx, item, props[i]);
					if ((a == 90 && (w & 1)) || (a == 180 && ((w | hh) & 1)) || (a == 270 && (hh & 1)))
						return false;
					if (a == 90 || a == 270)
						std::swap(w, hh);
					break;
				}
				case heif_item_property_type_transform_mirror:
					if ((w | hh) & 1)
						return false;
					break;
				case heif_item_property_type_transform_crop:
				{
					int l = 0, t = 0, r = 0, b = 0;
					heif_item_get_property_transform_crop_borders(h.ctx, item, props[i], w, hh, &l, &t, &r, &b);
					if ((l | t) & 1)
						return false;
					w -= l + r, hh -= t + b;
					break;
				}
				default:
					return false;
				}
			return n <= 16;
		}

		// libheif polls this between tiles: a decode scrolled past stops early
		int Cancelled(void* token) { return token && ((const CancelToken*)token)->Cancelled(); }

		// get_Kr_Kb and get_YCbCr_to_RGB_coefficients (nclx.cc) in float, then lround(256 * c) as Op_YCbCr420_to_RGB32 does
		void Coefficients(int matrix, int32_t k[4])
		{
			float kr = 0.0f, kb = 0.0f;
			switch (matrix)
			{
			case 1: kr = 0.2126f, kb = 0.0722f; break;
			case 4: kr = 0.30f, kb = 0.11f; break;
			case 5: case 6: kr = 0.299f, kb = 0.114f; break;
			case 7: kr = 0.212f, kb = 0.087f; break;
			case 9: case 10: kr = 0.2627f, kb = 0.0593f; break;
			default: break;
			}
			float c[4] = { 1.402f, -0.344136f, -0.714136f, 1.772f };
			if (kr != 0.0f || kb != 0.0f)
				c[0] = 2 * (-kr + 1), c[1] = 2 * kb * (-kb + 1) / (kb + kr - 1), c[2] = 2 * kr * (-kr + 1) / (kb + kr - 1), c[3] = 2 * (-kb + 1);
			for (int i = 0; i < 4; ++i)
				k[i] = (int32_t)std::lround(256 * c[i]);
		}
	}

	std::string ProbeHeic(const uint8_t* data, size_t size, Probe& out)
	{
		Heif h;
		if (std::string err = h.Open(data, size); !err.empty())
			return err;
		out.w = heif_image_handle_get_width(h.handle);
		out.h = heif_image_handle_get_height(h.handle);
		out.bits = heif_image_handle_get_luma_bits_per_pixel(h.handle) > 8 ? 16 : 8;
		Colors(h.handle, out);
		heif_color_profile_nclx nclx;
		if (YuvPlanes(h, data, size, nclx))
			out.yuv = { 3, 1, 1 }, out.channels = 3;
		return out.w > 0 && out.h > 0 ? std::string() : "heic: bad size";
	}

	std::string DecodeHeicYuv(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int dispW, int dispH, const Parallel& par)
	{
		Heif h;
		if (std::string err = h.Open(data, size); !err.empty())
			return err;
		heif_color_profile_nclx nclx;
		const YuvShape s = { 3, 1, 1 };
		const int w = heif_image_handle_get_width(h.handle), hh = heif_image_handle_get_height(h.handle);
		if (w != dispW || hh != dispH || YuvFrameSize(w, hh, s) > capacity || !YuvPlanes(h, data, size, nclx))
			return kYuvMismatch;
		heif_context_set_max_decoding_threads(h.ctx, std::max(par.threads, 0));
		heif_decoding_options* opt = heif_decoding_options_alloc();
		opt->output_image_nclx_profile = &nclx;
		opt->progress_user_data = (void*)par.cancel;
		opt->cancel_decoding = Cancelled;
		const heif_error e = heif_decode_image(h.handle, &h.image, heif_colorspace_YCbCr, heif_chroma_420, opt);
		heif_decoding_options_free(opt);
		if (e.code != heif_error_Ok)
			return std::string("heic: ") + e.message;
		if (heif_image_get_chroma_format(h.image) != heif_chroma_420 || heif_image_get_width(h.image, heif_channel_Y) != w || heif_image_get_height(h.image, heif_channel_Y) != hh)
			return "heic: unexpected planes";
		YuvHeader& hd = InitYuvFrame(frame, w, hh, s, YuvMode::Heif);
		Coefficients(nclx.matrix_coefficients, hd.k);
		struct Plane { const uint8_t* src; size_t stride; uint8_t* dst; size_t pitch; size_t bytes; int rows; };
		Plane planes[3] = {};
		const heif_channel channels[3] = { heif_channel_Y, heif_channel_Cb, heif_channel_Cr };
		const uint32_t offsets[3] = { hd.yOffset, hd.cbOffset, hd.crOffset };
		for (int c = 0; c < 3; ++c)
		{
			Plane& p = planes[c];
			p.src = heif_image_get_plane_readonly2(h.image, channels[c], &p.stride);
			p.dst = frame + offsets[c];
			p.pitch = c ? hd.cPitch : hd.yPitch;
			p.bytes = c ? hd.cWidth : (size_t)w;
			p.rows = c ? (int)hd.cHeight : hh;
			if (!p.src || heif_image_get_width(h.image, channels[c]) < (int)p.bytes || heif_image_get_height(h.image, channels[c]) < p.rows)
				return "heic: unexpected planes";
		}
		par.For((int64_t)hh + 2 * hd.cHeight, 64, [&](int64_t a, int64_t b, int)
		{
			for (int64_t i = a; i < b; ++i)
			{
				const int c = i < hh ? 0 : i < hh + (int64_t)hd.cHeight ? 1 : 2;
				const int64_t row = c == 0 ? i : i - hh - (c - 1) * (int64_t)hd.cHeight;
				memcpy(planes[c].dst + row * planes[c].pitch, planes[c].src + row * planes[c].stride, planes[c].bytes);
			}
		});
		return {};
	}

	std::string DecodeHeic(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par)
	{
		Heif h;
		if (std::string err = h.Open(data, size); !err.empty())
			return err;
		heif_context_set_max_decoding_threads(h.ctx, std::max(par.threads, 0));
		const bool deep = heif_image_handle_get_luma_bits_per_pixel(h.handle) > 8;
		heif_decoding_options* opt = heif_decoding_options_alloc();
		opt->progress_user_data = (void*)par.cancel;
		opt->cancel_decoding = Cancelled;
		const heif_error e = heif_decode_image(h.handle, &h.image, heif_colorspace_RGB, deep ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RGBA, opt);
		heif_decoding_options_free(opt);
		if (e.code != heif_error_Ok)
			return std::string("heic: ") + e.message;
		size_t stride = 0;
		const uint8_t* p = heif_image_get_plane_readonly2(h.image, heif_channel_interleaved, &stride);
		const int w = std::min(heif_image_get_width(h.image, heif_channel_interleaved), maxW), rows = std::min(heif_image_get_height(h.image, heif_channel_interleaved), maxH);
		const int bits = heif_image_get_bits_per_pixel_range(h.image, heif_channel_interleaved);
		for (int y = 0; y < rows; ++y)
			if (deep && bits < 16)
				Widen((const uint16_t*)(p + y * stride), w * 4, bits, (uint16_t*)(dst + y * pitch));
			else
				memcpy(dst + y * pitch, p + y * stride, (size_t)w * (deep ? 8 : 4));
		return {};
	}
}
