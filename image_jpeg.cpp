#include "image_formats.h"

#include <turbojpeg.h>

#include <cstring>

#pragma comment(lib, "turbojpeg.lib")

// JPEG through libjpeg-turbo (vcpkg, turbojpeg.dll is delay-loaded)
namespace iv::fmt
{
	namespace
	{
		struct Handle
		{
			tjhandle h = tj3Init(TJINIT_DECOMPRESS);
			~Handle() { if (h) tj3Destroy(h); }
		};

		std::string Header(Handle& tj, const uint8_t* data, size_t size, int& w, int& h)
		{
			if (!tj.h)
				return "libjpeg-turbo init failed";
			if (tj3DecompressHeader(tj.h, data, size) != 0)
				return std::string("jpeg: ") + tj3GetErrorStr(tj.h);
			w = tj3Get(tj.h, TJPARAM_JPEGWIDTH);
			h = tj3Get(tj.h, TJPARAM_JPEGHEIGHT);
			const int cs = tj3Get(tj.h, TJPARAM_COLORSPACE);
			if (cs == TJCS_CMYK || cs == TJCS_YCCK)
				return "jpeg: CMYK is not supported";
			return w > 0 && h > 0 ? std::string() : "jpeg: bad size";
		}

		// the planes tj3DecompressToYUVPlanes8 gives; none for RGB / lossless / 12-bit files and unknown subsamplings
		YuvShape Shape(tjhandle h)
		{
			const int cs = tj3Get(h, TJPARAM_COLORSPACE);
			if (tj3Get(h, TJPARAM_LOSSLESS) || tj3Get(h, TJPARAM_PRECISION) != 8 || (cs != TJCS_YCbCr && cs != TJCS_GRAY))
				return {};
			switch (tj3Get(h, TJPARAM_SUBSAMP))
			{
			case TJSAMP_444: return { 3, 0, 0 };
			case TJSAMP_422: return { 3, 1, 0 };
			case TJSAMP_420: return { 3, 1, 1 };
			case TJSAMP_440: return { 3, 0, 1 };
			case TJSAMP_411: return { 3, 2, 0 };
			case TJSAMP_441: return { 3, 0, 2 };
			case TJSAMP_GRAY: return { 1, 0, 0 };
			default: return {};
			}
		}

		int Subsamp(const YuvShape& s)
		{
			if (s.planes != 3)
				return TJSAMP_GRAY;
			return s.sx == 2 ? TJSAMP_411 : s.sy == 2 ? TJSAMP_441 : s.sx ? (s.sy ? TJSAMP_420 : TJSAMP_422) : s.sy ? TJSAMP_440 : TJSAMP_444;
		}
	}

	std::string ProbeJpeg(const uint8_t* data, size_t size, Probe& out)
	{
		Handle tj;
		std::string err = Header(tj, data, size, out.w, out.h);
		if (err.empty() && (out.yuv = Shape(tj.h)).planes)
			out.channels = 3;
		return err;
	}

	std::string DecodeJpegYuv(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int dispW, int dispH, int orientation)
	{
		Handle tj;
		int w = 0, h = 0;
		if (std::string err = Header(tj, data, size, w, h); !err.empty())
			return err;
		YuvShape s = Shape(tj.h);
		s.orientation = (uint8_t)orientation;
		if (!s.planes || (orientation >= 5 ? h : w) != dispW || (orientation >= 5 ? w : h) != dispH || YuvFrameSize(w, h, s) > capacity)
			return kYuvMismatch;
		const YuvHeader& hd = InitYuvFrame(frame, w, h, s, YuvMode::Jpeg);
		unsigned char* planes[3] = { frame + hd.yOffset, frame + hd.cbOffset, frame + hd.crOffset };
		int strides[3] = { (int)hd.yPitch, (int)hd.cPitch, (int)hd.cPitch };
		if (tj3DecompressToYUVPlanes8(tj.h, data, size, planes, strides) != 0)
			return std::string("jpeg: ") + tj3GetErrorStr(tj.h);
		return {};
	}

	std::string EncodeYuv(const uint8_t* rgba, int w, int h, const YuvShape& shape, uint8_t* frame, uint64_t capacity)
	{
		// stored as the frame's orientation would have them, so every frame of a layout has the same planes
		const int sw = shape.orientation >= 5 ? h : w, sh = shape.orientation >= 5 ? w : h;
		if (YuvFrameSize(sw, sh, shape) > capacity)
			return "frame does not fit the cache slot";
		std::vector<uint8_t> stored;
		if (shape.orientation != 1)
		{
			stored.resize((size_t)sw * sh * 4);
			for (int r = 0; r < sh; ++r)
				for (int c = 0; c < sw; ++c)
				{
					int x, y;
					YuvShown(shape.orientation, sw, sh, c, r, x, y);
					memcpy(&stored[((size_t)r * sw + c) * 4], rgba + ((size_t)y * w + x) * 4, 4);
				}
			rgba = stored.data();
		}
		tjhandle tj = tj3Init(TJINIT_COMPRESS);
		if (!tj)
			return "libjpeg-turbo init failed";
		tj3Set(tj, TJPARAM_SUBSAMP, Subsamp(shape));
		const YuvHeader& hd = InitYuvFrame(frame, sw, sh, shape, YuvMode::Jpeg);
		unsigned char* planes[3] = { frame + hd.yOffset, frame + hd.cbOffset, frame + hd.crOffset };
		int strides[3] = { (int)hd.yPitch, (int)hd.cPitch, (int)hd.cPitch };
		const bool ok = tj3EncodeYUVPlanes8(tj, rgba, sw, sw * 4, sh, TJPF_RGBA, planes, strides) == 0;
		const std::string err = ok ? std::string() : std::string("jpeg: ") + tj3GetErrorStr(tj);
		tj3Destroy(tj);
		return err;
	}

	std::string DecodeJpeg(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel&)
	{
		Handle tj;
		int w = 0, h = 0;
		if (std::string err = Header(tj, data, size, w, h); !err.empty())
			return err;
		if (w <= maxW && h <= maxH)
		{
			if (tj3Decompress8(tj.h, data, size, dst, (int)pitch, TJPF_RGBA) != 0)
				return std::string("jpeg: ") + tj3GetErrorStr(tj.h);
			return {};
		}
		std::vector<uint8_t> tmp((size_t)w * h * 4);
		if (tj3Decompress8(tj.h, data, size, tmp.data(), w * 4, TJPF_RGBA) != 0)
			return std::string("jpeg: ") + tj3GetErrorStr(tj.h);
		CopyRows(tmp.data(), w, h, dst, pitch, maxW, maxH);
		return {};
	}
}
