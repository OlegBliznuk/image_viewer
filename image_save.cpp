#include "image_save.h"
#include "image_formats.h"
#include "picture.h"

#include <OpenEXR/ImfChannelList.h>
#include <OpenEXR/ImfCompression.h>
#include <OpenEXR/ImfFrameBuffer.h>
#include <OpenEXR/ImfHeader.h>
#include <OpenEXR/ImfOutputFile.h>
#include <Imath/half.h>
#include <libdeflate.h>
#include <stb_image_write.h>
#include <tiffio.h>
#include <turbojpeg.h>
#include <webp/encode.h>

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <thread>

#pragma comment(lib, "deflate.lib")

namespace iv
{
	namespace
	{
		// the whole frame as tight RGBA samples of `bytes` bytes: display values (grade, exposure, view transform, display curve, display adjustments) for integers,
		// graded linear floats otherwise; alpha tells whether any pixel is not opaque
		std::vector<uint8_t> Convert(const SourceRows& s, bool display, int bytes, bool& alpha, const AdjustState& adjust, const Parallel& par)
		{
			const int w = s.l.width, h = s.l.height;
			std::vector<uint8_t> out((size_t)w * h * 4 * bytes);
			std::vector<char> translucent(par.MaxWorkers(), 0);
			std::vector<std::vector<float>> rows(par.MaxWorkers());
			const float gain = std::exp2(s.p.exposure);
			par.For(h, 16, [&](int64_t y0, int64_t y1, int part)
			{
				std::vector<float>& row = rows[part];
				row.resize((size_t)w * 4);
				for (int y = (int)y0; y < (int)y1; ++y)
				{
					s.Row(y, row.data());
					for (int x = 0; x < w; ++x)
					{
						float* c = &row[(size_t)x * 4];
						translucent[part] |= c[3] < 0.9995f;
						ApplyGrade(adjust, c);
						if (!display)
							continue;
						DisplayEncode(c, gain, s.p.view);
						ApplyDisplayAdjust(adjust, c);
						c[3] = Saturate(c[3]);
					}
					uint8_t* o = out.data() + (size_t)y * w * 4 * bytes;
					for (int i = 0; i < w * 4; ++i)
						if (bytes == 1)
							o[i] = (uint8_t)std::lround(row[i] * 255.0f);
						else if (bytes == 2)
							((uint16_t*)o)[i] = (uint16_t)std::lround(row[i] * 65535.0f);
						else
							((float*)o)[i] = row[i];
				}
			});
			alpha = std::any_of(translucent.begin(), translucent.end(), [](char v) { return v != 0; });
			return out;
		}

		// RGBA to RGB in place
		void DropAlpha(std::vector<uint8_t>& px, size_t pixels, int bytes)
		{
			const size_t in = 4 * (size_t)bytes, out = 3 * (size_t)bytes;
			for (size_t i = 0; i < pixels; ++i)
				memmove(px.data() + i * out, px.data() + i * in, out);
			px.resize(pixels * out);
		}

		std::string WriteFile(const std::wstring& path, const void* data, size_t size)
		{
			FILE* f = _wfopen(path.c_str(), L"wb");
			if (!f)
				return "cannot create the file";
			const bool ok = fwrite(data, 1, size, f) == size;
			return fclose(f) == 0 && ok ? std::string() : "write error";
		}

		void Put32(std::vector<uint8_t>& o, uint32_t v)
		{
			const uint8_t b[4] = { uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v) };
			o.insert(o.end(), b, b + 4);
		}

		void Chunk(std::vector<uint8_t>& o, const char* type, const uint8_t* data, size_t n)
		{
			Put32(o, (uint32_t)n);
			const size_t at = o.size();
			o.insert(o.end(), type, type + 4);
			o.insert(o.end(), data, data + n);
			Put32(o, libdeflate_crc32(0, o.data() + at, n + 4));
		}

		// tight RGB / RGBA rows of 8 or 16-bit samples (native order) to a PNG; each row takes the filter with the smallest sum of absolute bytes
		std::vector<uint8_t> EncodePng(const uint8_t* px, int w, int h, int channels, bool sixteen, const Parallel& par)
		{
			const size_t bpp = (size_t)channels * (sixteen ? 2 : 1), rowBytes = (size_t)w * bpp;
			std::vector<uint8_t> raw(rowBytes * h), filtered((rowBytes + 1) * h);
			par.For(h, 32, [&](int64_t y0, int64_t y1, int)
			{
				for (int y = (int)y0; y < (int)y1; ++y)
				{
					uint8_t* r = raw.data() + rowBytes * y;
					const uint8_t* s = px + rowBytes * y;
					if (sixteen)
						for (size_t i = 0; i < rowBytes; i += 2)
							r[i] = s[i + 1], r[i + 1] = s[i];
					else
						memcpy(r, s, rowBytes);
				}
			});
			const std::vector<uint8_t> zero(rowBytes, 0);
			std::vector<std::vector<uint8_t>> trials(par.MaxWorkers());
			par.For(h, 16, [&](int64_t y0, int64_t y1, int worker)
			{
				std::vector<uint8_t>& trial = trials[worker];
				trial.resize(rowBytes * 4);
				for (int y = (int)y0; y < (int)y1; ++y)
				{
					const uint8_t* cur = raw.data() + rowBytes * y;
					const uint8_t* up = y ? cur - rowBytes : zero.data();
					uint64_t best = UINT64_MAX;
					int pick = 0;
					const int kinds[4] = { 0, 1, 2, 4 };
					for (int k = 0; k < 4; ++k)
					{
						uint8_t* t = trial.data() + rowBytes * k;
						uint64_t sum = 0;
						for (size_t i = 0; i < rowBytes; ++i)
						{
							const int a = i >= bpp ? cur[i - bpp] : 0, b = up[i], c = i >= bpp ? up[i - bpp] : 0;
							int pred = 0;
							if (kinds[k] == 1)
								pred = a;
							else if (kinds[k] == 2)
								pred = b;
							else if (kinds[k] == 4)
							{
								const int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
								pred = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
							}
							t[i] = uint8_t(cur[i] - pred);
							sum += std::abs((int)(int8_t)t[i]);
						}
						if (sum < best)
							best = sum, pick = k;
					}
					uint8_t* o = filtered.data() + (rowBytes + 1) * y;
					o[0] = (uint8_t)kinds[pick];
					memcpy(o + 1, trial.data() + rowBytes * pick, rowBytes);
				}
			});
			libdeflate_compressor* c = libdeflate_alloc_compressor(6);
			std::vector<uint8_t> z(libdeflate_zlib_compress_bound(c, filtered.size()));
			z.resize(libdeflate_zlib_compress(c, filtered.data(), filtered.size(), z.data(), z.size()));
			libdeflate_free_compressor(c);
			std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
			std::vector<uint8_t> ihdr;
			Put32(ihdr, (uint32_t)w);
			Put32(ihdr, (uint32_t)h);
			const uint8_t rest[5] = { uint8_t(sixteen ? 16 : 8), uint8_t(channels == 4 ? 6 : 2), 0, 0, 0 };
			ihdr.insert(ihdr.end(), rest, rest + 5);
			Chunk(png, "IHDR", ihdr.data(), ihdr.size());
			Chunk(png, "IDAT", z.data(), z.size());
			Chunk(png, "IEND", nullptr, 0);
			return png;
		}

		void Append(void* ctx, void* data, int size)
		{
			auto& v = *(std::vector<uint8_t>*)ctx;
			v.insert(v.end(), (const uint8_t*)data, (const uint8_t*)data + size);
		}

		std::string WriteTiff(const std::wstring& path, const uint8_t* px, int w, int h, int channels, int bits, bool floats)
		{
			TIFF* t = TIFFOpenW(path.c_str(), "w");
			if (!t)
				return "cannot create the file";
			TIFFSetField(t, TIFFTAG_IMAGEWIDTH, w);
			TIFFSetField(t, TIFFTAG_IMAGELENGTH, h);
			TIFFSetField(t, TIFFTAG_SAMPLESPERPIXEL, channels);
			TIFFSetField(t, TIFFTAG_BITSPERSAMPLE, bits);
			TIFFSetField(t, TIFFTAG_SAMPLEFORMAT, floats ? SAMPLEFORMAT_IEEEFP : SAMPLEFORMAT_UINT);
			TIFFSetField(t, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
			TIFFSetField(t, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
			TIFFSetField(t, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
			TIFFSetField(t, TIFFTAG_PREDICTOR, floats ? PREDICTOR_FLOATINGPOINT : PREDICTOR_HORIZONTAL);
			TIFFSetField(t, TIFFTAG_ROWSPERSTRIP, 16);
			if (channels == 4)
			{
				const uint16_t extra = EXTRASAMPLE_UNASSALPHA;
				TIFFSetField(t, TIFFTAG_EXTRASAMPLES, 1, &extra);
			}
			const size_t rowBytes = (size_t)w * channels * bits / 8;
			std::vector<uint8_t> strip(rowBytes * 16);
			bool ok = true;
			for (int y = 0; y < h && ok; y += 16)
			{
				const int rows = std::min(16, h - y);
				memcpy(strip.data(), px + rowBytes * y, rowBytes * rows);
				ok = TIFFWriteEncodedStrip(t, TIFFComputeStrip(t, y, 0), strip.data(), (tmsize_t)(rowBytes * rows)) >= 0;
			}
			TIFFClose(t);
			return ok ? std::string() : "tiff write error";
		}

		bool IsFloat(DXGI_FORMAT f) { return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R32_FLOAT; }
		bool Is32(DXGI_FORMAT f) { return f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R32_FLOAT; }

		std::wstring Extension(const std::wstring& path)
		{
			const size_t dot = path.rfind(L'.');
			std::wstring e = dot == std::wstring::npos ? std::wstring() : path.substr(dot + 1);
			for (auto& c : e)
				c = (wchar_t)towlower(c);
			return e;
		}
	}

	const char* SaveExtensions()
	{
		return "exr png jpg jpeg tif tiff webp hdr bmp tga ico";
	}

	bool CanSave(const std::wstring& path)
	{
		const std::string e = ToUtf8(Extension(path));
		const std::string all = std::string(" ") + SaveExtensions() + " ";
		return !e.empty() && all.find(" " + e + " ") != std::string::npos;
	}

	std::string SaveImage(const std::wstring& path, const Layout& layout, const uint8_t* data, const DrawParams& p, const AdjustState& adjust, const Parallel& par)
	{
		if (!data || layout.width <= 0 || layout.height <= 0)
			return "nothing to save";
		const std::wstring ext = Extension(path);
		const int w = layout.width, h = layout.height;
		const size_t pixels = (size_t)w * h;
		const SourceRows src(layout, data, p);
		const bool floatSource = IsFloat(layout.format), deep = layout.format != DXGI_FORMAT_R8G8B8A8_UNORM;
		bool alpha = false;
		try
		{
			if (ext == L"exr" || ext == L"hdr" || ((ext == L"tif" || ext == L"tiff") && floatSource))
			{
				std::vector<uint8_t> px = Convert(src, false, 4, alpha, adjust, par);
				const float* f = (const float*)px.data();
				if (ext == L"hdr")
				{
					DropAlpha(px, pixels, 4);
					std::vector<uint8_t> out;
					return stbi_write_hdr_to_func(Append, &out, w, h, 3, (const float*)px.data()) ? WriteFile(path, out.data(), out.size()) : "hdr encode failed";
				}
				if (ext == L"exr")
				{
					Imf::Header hd(w, h);
					hd.compression() = Imf::ZIP_COMPRESSION;
					// OpenEXR 3.4 writes a channel only from a slice of its own type
					const bool full = Is32(layout.format);
					std::vector<uint16_t> half(full ? 0 : pixels * 4);
					for (size_t i = 0; i < half.size(); ++i)
						half[i] = imath_float_to_half(f[i]);
					const char* names[4] = { "R", "G", "B", "A" };
					Imf::FrameBuffer fb;
					for (int c = 0; c < (alpha ? 4 : 3); ++c)
					{
						hd.channels().insert(names[c], Imf::Channel(full ? Imf::FLOAT : Imf::HALF));
						fb.insert(names[c], full ? Imf::Slice(Imf::FLOAT, (char*)(f + c), 16, (size_t)w * 16) : Imf::Slice(Imf::HALF, (char*)(half.data() + c), 8, (size_t)w * 8));
					}
					Imf::OutputFile out(ToUtf8(path).c_str(), hd, (int)std::thread::hardware_concurrency());
					out.setFrameBuffer(fb);
					out.writePixels(h);
					return {};
				}
				const int channels = alpha ? 4 : 3;
				if (!alpha)
					DropAlpha(px, pixels, 4);
				if (Is32(layout.format))
					return WriteTiff(path, px.data(), w, h, channels, 32, true);
				std::vector<uint16_t> half(pixels * channels);
				for (size_t i = 0; i < half.size(); ++i)
					half[i] = imath_float_to_half(((const float*)px.data())[i]);
				return WriteTiff(path, (const uint8_t*)half.data(), w, h, channels, 16, true);
			}
			const bool sixteen = deep && (ext == L"png" || ext == L"tif" || ext == L"tiff");
			std::vector<uint8_t> px = Convert(src, true, sixteen ? 2 : 1, alpha, adjust, par);
			const int bytes = sixteen ? 2 : 1;
			if (ext == L"jpg" || ext == L"jpeg")
			{
				tjhandle tj = tj3Init(TJINIT_COMPRESS);
				tj3Set(tj, TJPARAM_QUALITY, 95);
				tj3Set(tj, TJPARAM_SUBSAMP, TJSAMP_444);
				unsigned char* jpeg = nullptr;
				size_t size = 0;
				const bool ok = tj3Compress8(tj, px.data(), w, 0, h, TJPF_RGBA, &jpeg, &size) == 0;
				const std::string err = ok ? WriteFile(path, jpeg, size) : std::string("jpeg: ") + tj3GetErrorStr(tj);
				tj3Free(jpeg);
				tj3Destroy(tj);
				return err;
			}
			if (ext == L"webp")
			{
				uint8_t* webp = nullptr;
				const size_t size = WebPEncodeRGBA(px.data(), w, h, w * 4, 92.0f, &webp);
				const std::string err = size ? WriteFile(path, webp, size) : "webp encode failed";
				WebPFree(webp);
				return err;
			}
			if (ext == L"ico")
			{
				if (w > 256 || h > 256)
					return "icons are at most 256 x 256";
				const std::vector<uint8_t> png = EncodePng(px.data(), w, h, 4, false, par);
				std::vector<uint8_t> ico = { 0, 0, 1, 0, 1, 0, uint8_t(w % 256), uint8_t(h % 256), 0, 0, 1, 0, 32, 0 };
				for (uint32_t v : { (uint32_t)png.size(), 22u })
					for (int k = 0; k < 4; ++k)
						ico.push_back(uint8_t(v >> (8 * k)));
				ico.insert(ico.end(), png.begin(), png.end());
				return WriteFile(path, ico.data(), ico.size());
			}
			const bool keepAlpha = alpha && ext != L"bmp";
			if (!keepAlpha)
				DropAlpha(px, pixels, bytes);
			const int channels = keepAlpha ? 4 : 3;
			if (ext == L"png")
			{
				const std::vector<uint8_t> png = EncodePng(px.data(), w, h, channels, sixteen, par);
				return WriteFile(path, png.data(), png.size());
			}
			if (ext == L"tif" || ext == L"tiff")
				return WriteTiff(path, px.data(), w, h, channels, sixteen ? 16 : 8, false);
			std::vector<uint8_t> out;
			const int ok = ext == L"bmp" ? stbi_write_bmp_to_func(Append, &out, w, h, channels, px.data()) : ext == L"tga" ? stbi_write_tga_to_func(Append, &out, w, h, channels, px.data()) : 0;
			return ok ? WriteFile(path, out.data(), out.size()) : "unsupported file type for saving (" + std::string(SaveExtensions()) + ")";
		}
		catch (const std::exception& e) { return e.what(); }
		catch (...) { return "unknown error"; }
	}
}
