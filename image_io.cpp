#include "image_io.h"
#include "image_exr.h"
#include "image_formats.h"

#include <OpenEXR/ImfThreading.h>
#include <Imath/half.h>

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <thread>

namespace iv
{
	namespace
	{
		constexpr FormatDesc kFormats[] = { { "exr", FileKind::Exr, "OpenEXR image", 101 }, { "png", FileKind::Png, "PNG image", 102 }, { "jpg", FileKind::Jpeg, "JPEG image", 103 }, { "jpeg", FileKind::Jpeg, "JPEG image", 103 }, { "hdr", FileKind::Hdr, "Radiance HDR image", 104 }, { "bmp", FileKind::Bmp, "Bitmap image", 105 }, { "tga", FileKind::Tga, "Targa image", 106 }, { "cr3", FileKind::Cr3, "Canon RAW image", 107 }, { "gif", FileKind::Gif, "GIF image", 108 }, { "webp", FileKind::Webp, "WebP image", 109 }, { "svg", FileKind::Svg, "SVG image", 110 }, { "ico", FileKind::Ico, "Icon", 0 }, { "cur", FileKind::Ico, "Cursor", 0 }, { "heic", FileKind::Heic, "HEIC image", 111 }, { "heif", FileKind::Heic, "HEIF image", 111 }, { "tif", FileKind::Tiff, "TIFF image", 112 }, { "tiff", FileKind::Tiff, "TIFF image", 112 }, { "tx", FileKind::Tiff, "TX texture", 113 } };

		// the readers that take the file's bytes; EXR and CR3 read from the path
		struct Reader
		{
			std::string (*probe)(const uint8_t* data, size_t size, fmt::Probe& out) = nullptr;
			std::string (*decode)(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par) = nullptr;
			std::string (*frames)(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames) = nullptr;
			bool wholeFile = false;   // the probe needs the whole file, not only its head
			std::string (*page)(const uint8_t* data, size_t size, int page, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par) = nullptr;   // decodes any frame on its own
			bool mapped = false;   // reads through a file mapping: probing touches only the headers, a page only its own data
			std::string (*yuv)(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int w, int h, const Parallel& par) = nullptr;   // the YCbCr planes behind a YuvHeader (Probe::yuv)
		};

		std::string JpegYuv(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int w, int h, const Parallel&) { return fmt::DecodeJpegYuv(data, size, frame, capacity, w, h); }

		Reader ReaderFor(FileKind k)
		{
			switch (k)
			{
			case FileKind::Png: return { fmt::ProbePng, fmt::DecodePng };
			case FileKind::Jpeg: return { .probe = fmt::ProbeJpeg, .decode = fmt::DecodeJpeg, .yuv = JpegYuv };
			case FileKind::Bmp: return { fmt::ProbeBmp, fmt::DecodeBmp };
			case FileKind::Tga: return { fmt::ProbeTga, fmt::DecodeTga };
			case FileKind::Hdr: return { fmt::ProbeHdr, fmt::DecodeHdr };
			case FileKind::Gif: return { fmt::ProbeGif, fmt::DecodeGif, fmt::DecodeGifFrames, true };
			case FileKind::Webp: return { fmt::ProbeWebp, fmt::DecodeWebp, fmt::DecodeWebpFrames, true };
			case FileKind::Svg: return { fmt::ProbeSvg, fmt::DecodeSvg, nullptr, true };
			case FileKind::Ico: return { fmt::ProbeIco, fmt::DecodeIco, fmt::DecodeIcoFrames, true };
			case FileKind::Heic: return { .probe = fmt::ProbeHeic, .decode = fmt::DecodeHeic, .wholeFile = true, .yuv = fmt::DecodeHeicYuv };
			case FileKind::Tiff: return { fmt::ProbeTiff, nullptr, nullptr, true, fmt::DecodeTiffPage, true };
			default: return {};
			}
		}

		void ComponentViews(ImageInfo& out, bool alpha, const char* type)
		{
			ViewDesc color;
			color.label = alpha ? "RGBA" : "RGB";
			color.type = type;
			out.views.push_back(color);
			const char* names[] = { "R", "G", "B", "A" };
			for (int c = 0; c < (alpha ? 4 : 3); ++c)
			{
				ViewDesc v;
				v.label = names[c];
				v.nbComps = 1;
				v.comp = c;
				v.type = type;
				out.views.push_back(v);
			}
			out.firstSingle = 1;
			out.defaultView = 0;
		}

		std::string ProbeOther(const std::wstring& path, FileKind kind, ImageInfo& out)
		{
			constexpr size_t kHead = 256 * 1024;
			const Reader r = ReaderFor(kind);
			fmt::Probe p;
			fmt::Bytes head;
			if (r.mapped)
			{
				const fmt::MappedFile m(path);
				if (!m.data)
					return "cannot open file";
				if (std::string err = r.probe(m.data, m.size, p); !err.empty())
					return err;
			}
			std::string err = kind == FileKind::Cr3 ? fmt::ProbeCr3(path, p) : r.mapped ? std::string() : fmt::ReadFile(path, head, r.wholeFile ? SIZE_MAX : kHead);
			if (err.empty() && r.probe && !r.mapped)
			{
				err = r.probe(head.data(), head.size(), p);
				// the header can sit behind large metadata (JPEG EXIF / ICC segments)
				if (!err.empty() && head.size() == kHead && fmt::ReadFile(path, head).empty())
					err = r.probe(head.data(), head.size(), p);
			}
			if (!err.empty())
				return err;
			out.kind = kind;
			out.width = p.w;
			out.height = p.h;
			out.bits = p.bits;
			out.channels = p.channels;
			out.floatSamples = p.floatSamples;
			out.colorMatrix = p.colorMatrix;
			std::copy(p.toRec709, p.toRec709 + 9, out.toRec709);
			out.frames = (int)std::max<size_t>({ 1, p.delays.size(), p.notes.size() });
			if (p.delays.size() > 1)
				out.delays = std::move(p.delays);
			out.notes = std::move(p.notes);
			out.mips = std::move(p.mips);
			out.yuv = p.yuv;
			ComponentViews(out, kind != FileKind::Hdr && p.channels != 3, kind == FileKind::Hdr ? "rgbe" : p.floatSamples ? (p.bits == 16 ? "16fp" : "32fp") : p.bits == 16 ? "16u" : "8u");
			return {};
		}

		std::string DecodeOther(const std::wstring& path, FileKind kind, const Layout& layout, uint8_t* dst, const Parallel& par, int frame, int mip)
		{
			const Reader r = ReaderFor(kind);
			fmt::Probe p;
			// a level of a mip pyramid is the page with that index
			const int page = mip > 0 ? mip : frame;
			if (r.mapped)
			{
				const fmt::MappedFile m(path);
				if (!m.data)
					return "cannot open file";
				if (std::string err = r.probe(m.data, m.size, p); !err.empty())
					return err;
				if (mip > 0 && mip >= (int)p.mips.size())
					return "no mip level " + std::to_string(mip);
				const auto [w, h] = mip > 0 ? p.mips[mip] : std::make_pair(p.w, p.h);
				if (w != layout.width || h != layout.height)
					memset(dst, 0, layout.size);
				return r.page(m.data, m.size, page, dst, layout.rowPitch, layout.width, layout.height, par);
			}
			fmt::Bytes file;
			std::string err = kind == FileKind::Cr3 ? fmt::ProbeCr3(path, p) : fmt::ReadFile(path, file);
			if (err.empty() && r.probe)
				err = r.probe(file.data(), file.size(), p);
			if (!err.empty())
				return err;
			if (layout.packing == Packing::Yuv)
			{
				err = kind == FileKind::Cr3 ? fmt::DecodeCr3Yuv(path, dst, layout.size, layout.width, layout.height) : r.yuv(file.data(), file.size(), dst, layout.size, layout.width, layout.height, par);
				if (err != fmt::kYuvMismatch)
					return err;
				// a sequence frame whose planes differ from the first's: RGBA, clipped or padded as other formats are, encoded to the frame's planes
				if (p.bits != 8)
					return "frame format differs from the first frame";
				std::vector<uint8_t> rgba((size_t)layout.width * layout.height * 4, 0);
				const size_t pitch = (size_t)layout.width * 4;
				err = kind == FileKind::Cr3 ? fmt::DecodeCr3(path, rgba.data(), pitch, layout.width, layout.height) : r.decode(file.data(), file.size(), rgba.data(), pitch, layout.width, layout.height, par);
				return err.empty() ? fmt::EncodeYuv(rgba.data(), layout.width, layout.height, layout.yuv, dst, layout.size) : err;
			}
			// a sequence frame smaller than the first leaves no stale pixels
			if (p.w != layout.width || p.h != layout.height)
				memset(dst, 0, layout.size);
			if (kind == FileKind::Cr3)
				return fmt::DecodeCr3(path, dst, layout.rowPitch, layout.width, layout.height);
			if (r.page)
				return r.page(file.data(), file.size(), page, dst, layout.rowPitch, layout.width, layout.height, par);
			return r.decode(file.data(), file.size(), dst, layout.rowPitch, layout.width, layout.height, par);
		}
	}

	fmt::MappedFile::MappedFile(const std::wstring& path)
	{
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
		m_file = file;
		LARGE_INTEGER s = {};
		if (file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &s) && s.QuadPart > 0 && (m_mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr)) != nullptr)
			if ((data = (const uint8_t*)MapViewOfFile(m_mapping, FILE_MAP_READ, 0, 0, 0)) != nullptr)
				size = (size_t)s.QuadPart;
	}

	fmt::MappedFile::~MappedFile()
	{
		if (data)
			UnmapViewOfFile(data);
		if (m_mapping)
			CloseHandle(m_mapping);
		if (m_file != INVALID_HANDLE_VALUE)
			CloseHandle(m_file);
	}

	// large files in parallel 16 MB+ pieces: positional reads on one handle are thread-safe, cached data copies on several cores and cold reads queue deeper
	std::string fmt::ReadFile(const std::wstring& path, fmt::Bytes& out, size_t limit)
	{
		out.clear();
		const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return "cannot open file";
		LARGE_INTEGER size = {};
		GetFileSizeEx(file, &size);
		out.resize(std::min((size_t)size.QuadPart, limit));
		const int pieces = (int)std::clamp<size_t>(out.size() >> 24, 1, 8);
		std::vector<char> ok(pieces, 0);
		auto read = [&](int i)
		{
			const size_t begin = out.size() * i / pieces, end = out.size() * (i + 1) / pieces;
			ok[i] = 1;
			for (size_t at = begin; at < end && ok[i];)
			{
				OVERLAPPED ov = {};
				ov.Offset = (DWORD)at, ov.OffsetHigh = (DWORD)(at >> 32);
				DWORD got = 0;
				ok[i] = ::ReadFile(file, out.data() + at, (DWORD)std::min<size_t>(end - at, 1u << 30), &got, &ov) && got > 0;
				at += got;
			}
		};
		std::vector<std::thread> pool;
		for (int i = 1; i < pieces; ++i)
			pool.emplace_back(read, i);
		read(0);
		for (auto& t : pool)
			t.join();
		CloseHandle(file);
		return std::all_of(ok.begin(), ok.end(), [](char v) { return v != 0; }) || out.empty() ? std::string() : "read error";
	}

	void fmt::CopyRows(const uint8_t* src, int w, int h, uint8_t* dst, size_t pitch, int maxW, int maxH, size_t bytesPerPixel)
	{
		const int rows = std::min(h, maxH), cols = std::min(w, maxW);
		for (int y = 0; y < rows && cols > 0; ++y)
			memcpy(dst + (size_t)y * pitch, src + (size_t)y * w * bytesPerPixel, (size_t)cols * bytesPerPixel);
	}

	std::span<const FormatDesc> Formats()
	{
		return kFormats;
	}

	std::string SupportedExtensions()
	{
		std::string out;
		for (const FormatDesc& f : kFormats)
			out += (out.empty() ? "" : " ") + std::string(f.ext);
		return out;
	}

	bool KindFromExtension(const std::wstring& path, FileKind& kind)
	{
		const size_t dot = path.rfind(L'.');
		if (dot == std::wstring::npos)
			return false;
		const std::wstring ext = path.substr(dot + 1);
		for (const FormatDesc& f : kFormats)
			if (std::equal(ext.begin(), ext.end(), f.ext, f.ext + strlen(f.ext), [](wchar_t a, char b) { return towlower(a) == (wchar_t)b; }))
				return kind = f.kind, true;
		return false;
	}

	void InitImageIo()
	{
		Imf::setGlobalThreadCount((int)std::thread::hardware_concurrency());
	}

	std::string ProbeImage(const std::wstring& path, ImageInfo& out)
	{
		out = ImageInfo();
		try
		{
			FileKind kind;
			if (!KindFromExtension(path, kind))
				return "unsupported file type (" + SupportedExtensions() + ")";
			// a .tx texture is a TIFF from maketx, or an EXR
			fmt::Bytes magic;
			if (kind == FileKind::Tiff && fmt::ReadFile(path, magic, 4).empty() && magic.size() == 4 && magic[0] == 0x76 && magic[1] == 0x2f && magic[2] == 0x31 && magic[3] == 0x01)
				kind = FileKind::Exr;
			if (kind == FileKind::Exr)
				return ProbeExr(ToUtf8(path), out);
			return ProbeOther(path, kind, out);
		}
		catch (const std::exception& e) { return e.what(); }
		catch (...) { return "unknown error"; }
	}

	Layout PitchedLayout(int width, int height, DXGI_FORMAT format, uint32_t bytesPerPixel)
	{
		Layout l;
		l.width = width;
		l.height = height;
		l.format = format;
		l.bytesPerPixel = bytesPerPixel;
		l.rowPitch = ((uint32_t)width * bytesPerPixel + 255) / 256 * 256;
		l.size = (uint64_t)l.rowPitch * (uint64_t)height;
		return l;
	}

	Layout MakeLayout(const ImageInfo& info, const ViewDesc& view)
	{
		const int w = info.width, h = info.height;
		const auto packed = [&](DXGI_FORMAT format, uint32_t bytes, Packing packing)
		{
			Layout l = PitchedLayout(w, h, format, bytes);
			l.packing = packing;
			return l;
		};
		if (info.yuv.planes)
		{
			Layout l;
			l.width = w, l.height = h, l.format = DXGI_FORMAT_R8G8B8A8_UNORM, l.packing = Packing::Yuv, l.yuv = info.yuv;
			l.size = info.yuv.orientation >= 5 ? YuvFrameSize(h, w, info.yuv) : YuvFrameSize(w, h, info.yuv);
			return l;
		}
		if (info.kind == FileKind::Hdr)
			return packed(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, Packing::Rgbe);
		// RGB without alpha of up to 16-bit samples stays 3 channels for the GPU to complete; 32-bit float RGB measured slower that way (25% less to move, slower shader reads)
		if (info.kind != FileKind::Exr && info.channels == 3)
			return info.floatSamples ? packed(DXGI_FORMAT_R16G16B16A16_FLOAT, 6, Packing::Rgb) : info.bits == 16 ? packed(DXGI_FORMAT_R16G16B16A16_UNORM, 6, Packing::Rgb) : packed(DXGI_FORMAT_R8G8B8A8_UNORM, 3, Packing::Rgb);
		if (info.kind == FileKind::Exr && view.nbComps == 4 && view.chans[3].empty() && !view.float32)
			return packed(DXGI_FORMAT_R16G16B16A16_FLOAT, 6, Packing::Rgb);
		if (info.kind != FileKind::Exr && info.floatSamples)
			return info.bits == 16 ? PitchedLayout(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8) : PitchedLayout(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, 16);
		if (info.kind != FileKind::Exr)
			return info.bits == 16 ? PitchedLayout(w, h, DXGI_FORMAT_R16G16B16A16_UNORM, 8) : PitchedLayout(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
		if (view.nbComps == 1)
			return view.float32 ? PitchedLayout(w, h, DXGI_FORMAT_R32_FLOAT, 4) : PitchedLayout(w, h, DXGI_FORMAT_R16_FLOAT, 2);
		return view.float32 ? PitchedLayout(w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, 16) : PitchedLayout(w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8);
	}

	std::string FrameSet::Frame(const std::wstring& path, FileKind kind, int frame, const Layout& layout, uint8_t* dst)
	{
		std::lock_guard lock(m_mutex);
		if (path != m_path)
		{
			m_path = path;
			m_pixels.clear();
			m_count = 0;
			fmt::Bytes file;
			const auto frames = ReaderFor(kind).frames;
			m_error = frames ? fmt::ReadFile(path, file) : "no frames in this format";
			if (m_error.empty())
				m_error = frames(file.data(), file.size(), m_width, m_height, m_count, m_pixels);
		}
		if (!m_error.empty())
			return m_error;
		if (frame < 0 || frame >= m_count)
			return "frame " + std::to_string(frame + 1) + " missing";
		if (m_width != layout.width || m_height != layout.height)
			memset(dst, 0, layout.size);
		fmt::CopyRows(m_pixels.data() + (size_t)m_width * m_height * 4 * frame, m_width, m_height, dst, layout.rowPitch, layout.width, layout.height);
		return {};
	}

	void FrameSet::Clear()
	{
		std::lock_guard lock(m_mutex);
		m_path.clear();
		m_pixels = {};
		m_count = 0;
		m_error.clear();
	}

	std::string DecodeImage(const std::wstring& path, FileKind kind, const ViewDesc& view, const Layout& layout, uint8_t* dst, const Parallel& par, int frame, FrameSet* frames)
	{
		try
		{
			if (frames && !ReaderFor(kind).page)
				return frames->Frame(path, kind, frame, layout, dst);
			const std::string err = kind == FileKind::Exr ? DecodeExr(path, view, layout, dst, par) : DecodeOther(path, kind, layout, dst, par, frame, view.mip);
			return par.Cancelled() ? std::string(kDecodeCancelled) : err;
		}
		catch (const std::exception& e) { return e.what(); }
		catch (...) { return "unknown error"; }
	}

	int ReadPixel(const Layout& l, const uint8_t* data, int x, int y, float out[4])
	{
		if (!data || x < 0 || y < 0 || x >= l.width || y >= l.height)
			return 0;
		if (l.packing == Packing::Yuv)
		{
			const YuvReader reader(data);
			uint8_t rgb[3];
			if (!reader.Fits(l.size, l.width, l.height))
				return 0;
			reader.Pixel(x, y, rgb);
			for (int c = 0; c < 3; ++c)
				out[c] = rgb[c] / 255.0f;
			out[3] = 1.0f;
			return 4;
		}
		const uint8_t* p = data + (size_t)y * l.rowPitch + (size_t)x * l.bytesPerPixel;
		const uint16_t* p16 = (const uint16_t*)p;
		if (l.packing == Packing::Rgbe)
			return DecodeRgbe(p, out), 4;
		if (l.packing == Packing::Rgb)
		{
			for (int c = 0; c < 3; ++c)
				out[c] = l.format == DXGI_FORMAT_R8G8B8A8_UNORM ? p[c] / 255.0f : l.format == DXGI_FORMAT_R16G16B16A16_UNORM ? p16[c] / 65535.0f : l.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? imath_half_to_float(p16[c]) : ((const float*)p)[c];
			out[3] = 1.0f;
			return 4;
		}
		switch (l.format)
		{
		case DXGI_FORMAT_R16G16B16A16_FLOAT: for (int c = 0; c < 4; ++c) out[c] = imath_half_to_float(p16[c]); return 4;
		case DXGI_FORMAT_R32G32B32A32_FLOAT: memcpy(out, p, 16); return 4;
		case DXGI_FORMAT_R16_FLOAT: out[0] = imath_half_to_float(p16[0]); return 1;
		case DXGI_FORMAT_R32_FLOAT: memcpy(out, p, 4); return 1;
		case DXGI_FORMAT_R8G8B8A8_UNORM: for (int c = 0; c < 4; ++c) out[c] = p[c] / 255.0f; return 4;
		case DXGI_FORMAT_R16G16B16A16_UNORM: for (int c = 0; c < 4; ++c) out[c] = p16[c] / 65535.0f; return 4;
		default: return 0;
		}
	}

	void RenderPreview(const Layout& l, const uint8_t* data, float zoom, float ox, float oy, int x0, int y0, int x1, int y1, uint32_t* out, const Parallel& par)
	{
		const int w = x1 - x0, h = y1 - y0;
		if (w <= 0 || h <= 0)
			return;
		// half bits -> sRGB byte, saturate(NaN) = 0 like the shader
		std::vector<uint8_t> lut(65536, 0);
		for (int i = 0; i <= 0x3C00; ++i)
		{
			const float v = imath_half_to_float((uint16_t)i);
			lut[i] = (uint8_t)std::lround(255.0f * (v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f));
		}
		std::fill(lut.begin() + 0x3C01, lut.begin() + 0x7C01, (uint8_t)255);
		std::vector<int> col(w);
		std::vector<size_t> off(w);
		for (int x = 0; x < w; ++x)
			col[x] = std::clamp((int)std::floor((x0 + x + 0.5f - ox) / zoom), 0, l.width - 1), off[x] = (size_t)col[x] * l.bytesPerPixel;
		const YuvReader yuv(l.packing == Packing::Yuv ? data : nullptr);
		const bool yuvOk = l.packing == Packing::Yuv && yuv.Fits(l.size, l.width, l.height);
		const uint8_t* lt = lut.data();
		auto rows = [&](int ya, int yb)
		{
			for (int y = ya; y < yb; ++y)
			{
				const int sy = std::clamp((int)std::floor((y0 + y + 0.5f - oy) / zoom), 0, l.height - 1);
				const uint8_t* row = data + (size_t)sy * l.rowPitch;
				uint32_t* dst = out + (size_t)y * w;
				if (l.packing == Packing::Yuv)
				{
					for (int x = 0; x < w; ++x)
					{
						uint8_t c[3] = {};
						if (yuvOk)
							yuv.Pixel(col[x], sy, c);
						dst[x] = c[2] | (c[1] << 8) | (c[0] << 16) | 0xFF000000u;
					}
					continue;
				}
				if (l.packing == Packing::Rgbe)
				{
					for (int x = 0; x < w; ++x)
					{
						float c[4];
						DecodeRgbe(row + off[x], c);
						dst[x] = lt[imath_float_to_half(c[2])] | (lt[imath_float_to_half(c[1])] << 8) | (lt[imath_float_to_half(c[0])] << 16) | 0xFF000000u;
					}
					continue;
				}
				switch (l.format)
				{
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
					for (int x = 0; x < w; ++x) { const uint16_t* p = (const uint16_t*)(row + off[x]); dst[x] = lt[p[2]] | (lt[p[1]] << 8) | (lt[p[0]] << 16) | 0xFF000000u; }
					break;
				case DXGI_FORMAT_R32G32B32A32_FLOAT:
					for (int x = 0; x < w; ++x) { const float* p = (const float*)(row + off[x]); dst[x] = lt[imath_float_to_half(p[2])] | (lt[imath_float_to_half(p[1])] << 8) | (lt[imath_float_to_half(p[0])] << 16) | 0xFF000000u; }
					break;
				case DXGI_FORMAT_R16_FLOAT:
					for (int x = 0; x < w; ++x) { const uint32_t g = lt[*(const uint16_t*)(row + off[x])]; dst[x] = g | (g << 8) | (g << 16) | 0xFF000000u; }
					break;
				case DXGI_FORMAT_R32_FLOAT:
					for (int x = 0; x < w; ++x) { const uint32_t g = lt[imath_float_to_half(*(const float*)(row + off[x]))]; dst[x] = g | (g << 8) | (g << 16) | 0xFF000000u; }
					break;
				case DXGI_FORMAT_R8G8B8A8_UNORM:
					for (int x = 0; x < w; ++x) { const uint8_t* p = row + off[x]; dst[x] = p[2] | (p[1] << 8) | (p[0] << 16) | 0xFF000000u; }
					break;
				case DXGI_FORMAT_R16G16B16A16_UNORM:
					for (int x = 0; x < w; ++x) { const uint16_t* p = (const uint16_t*)(row + off[x]); dst[x] = (p[2] >> 8) | ((p[1] >> 8) << 8) | ((p[0] >> 8) << 16) | 0xFF000000u; }
					break;
				default:
					std::fill(dst, dst + w, 0xFF000000u);
					break;
				}
			}
		};
		par.For(h, 16, [&](int64_t ya, int64_t yb, int) { rows((int)ya, (int)yb); });
	}

	std::string ToUtf8(const std::wstring& s)
	{
		if (s.empty())
			return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
		std::string out(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
		return out;
	}

	std::wstring FromUtf8(const std::string& s)
	{
		if (s.empty())
			return {};
		const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
		std::wstring out(n, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
		return out;
	}
}
