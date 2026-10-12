#pragma once

#include "parallel.h"
#include "yuv.h"

#include <dxgiformat.h>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace iv
{
	enum class FileKind { Exr, Png, Jpeg, Bmp, Tga, Hdr, Cr3, Gif, Webp, Svg, Ico, Heic, Tiff, Count };

	// one supported extension; Explorer and the Open dialog take the name, DefaultIcon the icon resource (0: the file is its own icon)
	struct FormatDesc
	{
		const char* ext;
		FileKind kind;
		const char* name;
		int icon;
	};

	std::span<const FormatDesc> Formats();
	// lower-case extensions, space separated
	std::string SupportedExtensions();
	bool KindFromExtension(const std::wstring& path, FileKind& kind);
	// numbered photos (camera shot counters) open as single images until the Sequences checkbox is turned on
	inline bool SequencesByDefault(FileKind k) { return k != FileKind::Jpeg && k != FileKind::Cr3 && k != FileKind::Heic; }

	// one entry of the channel combo: an RGBA layer of a part or a single channel
	struct ViewDesc
	{
		std::string label;
		int part = 0;
		std::string partName;
		std::string chans[4];
		int nbComps = 4;
		bool float32 = false;
		int comp = -1;
		std::string type;   // stored data type: 8u, 16u, 16fp, 32fp, 32u, rgbe ("16fp/32fp" for mixed layers)
		int mip = 0;   // the level decoded from a mip-mapped file (ImageInfo::mips)
	};

	struct ImageInfo
	{
		FileKind kind = FileKind::Exr;
		int width = 0;
		int height = 0;
		int bits = 8;   // bits per sample of the decoded frame: 8 or 16 (integer), 16 or 32 (float)
		int channels = 4;   // 3: RGB without alpha, decoded as 3-channel pixels
		bool floatSamples = false;   // scene-linear float samples; integer samples are sRGB-encoded
		bool colorMatrix = false;   // toRec709 takes linear RGB in the file's primaries to sRGB / Rec.709 primaries
		float toRec709[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		std::vector<ViewDesc> views;
		int firstSingle = 0;
		int defaultView = 0;
		int frames = 1;   // frames of a multi-frame file (animation, icon entries)
		std::vector<float> delays;   // animations: seconds each frame stays on screen
		std::vector<std::string> notes;   // per frame, shown in the status bar (icon entries, TIFF pages)
		std::vector<std::pair<int, int>> mips;   // level sizes of a mip-mapped file (TIFF pyramid, tiled EXR), level 0 first; empty otherwise
		YuvShape yuv;   // planes > 0: frames stay YCbCr planes (Packing::Yuv)
	};

	// sRGB-encoded integer samples; their channel views only swizzle in the shader
	inline bool IsDisplayReferred(const ImageInfo& i) { return i.kind != FileKind::Exr && !i.floatSamples; }

	// how the frame cache holds a frame when it is not plain texels: Radiance RGBE, RGB without alpha, or YCbCr planes behind a YuvHeader; the GPU expands them into the RGBA texture
	enum class Packing : uint8_t { None, Rgbe, Rgb, Yuv };

	struct Layout
	{
		int width = 0;
		int height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		uint32_t bytesPerPixel = 0;
		uint32_t rowPitch = 0;
		uint64_t size = 0;
		Packing packing = Packing::None;   // format stays the texture's; bytesPerPixel and rowPitch describe the packed pixels (none for Yuv)
		YuvShape yuv;
		bool operator==(const Layout&) const = default;
	};

	// one RGBE pixel as linear RGBA, exactly as the GPU expands it: 2^(e - 136) from the exponent bits, 0 below e = 10
	inline void DecodeRgbe(const uint8_t* p, float out[4])
	{
		const uint32_t bits = p[3] > 9 ? uint32_t(p[3] - 9) << 23 : 0;
		float s;
		memcpy(&s, &bits, 4);
		out[0] = p[0] * s, out[1] = p[1] * s, out[2] = p[2] * s, out[3] = 1.0f;
	}

	void InitImageIo();
	std::string ProbeImage(const std::wstring& path, ImageInfo& out);
	// rows aligned to 256 bytes, as texture copies from buffers need
	Layout PitchedLayout(int width, int height, DXGI_FORMAT format, uint32_t bytesPerPixel);
	Layout MakeLayout(const ImageInfo& info, const ViewDesc& view);

	// all frames of a multi-frame file, decoded at the first request and copied out by the decode jobs of the single frames
	class FrameSet
	{
	public:
		std::string Frame(const std::wstring& path, FileKind kind, int frame, const Layout& layout, uint8_t* dst);
		void Clear();
	private:
		std::mutex m_mutex;
		std::wstring m_path;
		int m_width = 0;
		int m_height = 0;
		int m_count = 0;
		std::vector<uint8_t> m_pixels;
		std::string m_error;
	};

	// what DecodeImage returns once par.cancel was set during the decode (the frame holds whatever was written by then)
	inline constexpr char kDecodeCancelled[] = "decode cancelled";
	// loops spread by par; with a FrameSet the given frame of a multi-frame file, without one the first frame
	std::string DecodeImage(const std::wstring& path, FileKind kind, const ViewDesc& view, const Layout& layout, uint8_t* dst, const Parallel& par, int frame = 0, FrameSet* frames = nullptr);
	int ReadPixel(const Layout& layout, const uint8_t* data, int x, int y, float out[4]);
	// BGRA8 sRGB of the default view for the GDI preview; dst pixel (x,y) samples source ((x+0.5-ox)/zoom, (y+0.5-oy)/zoom)
	void RenderPreview(const Layout& layout, const uint8_t* data, float zoom, float ox, float oy, int x0, int y0, int x1, int y1, uint32_t* out, const Parallel& par);
	std::string ToUtf8(const std::wstring& s);
	std::wstring FromUtf8(const std::string& s);
}
