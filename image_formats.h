#pragma once

// dedicated readers behind image_io; each decodes into a pitched destination of at most maxW x maxH pixels, loops spread by `par`
#include "parallel.h"
#include "yuv.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace iv::fmt
{
	// what a probe learns: the size and, per frame of a multi-frame file, its time on screen (animations) or a description (icon entries)
	struct Probe
	{
		int w = 0;
		int h = 0;
		int bits = 8;   // as ImageInfo: 8 / 16 integer, 16 / 32 float
		int channels = 4;   // 3: the decoder writes RGB pixels (no alpha)
		bool floatSamples = false;
		bool colorMatrix = false;
		float toRec709[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
		std::vector<float> delays;
		std::vector<std::string> notes;
		std::vector<std::pair<int, int>> mips;   // level sizes of a mip pyramid, level 0 first (the page reader takes the level as its page)
		YuvShape yuv;   // planes > 0: the frame can stay YCbCr planes (Decode*Yuv)
	};

	// a Decode*Yuv result: this file's planes are not the frame's (another subsampling or size in a sequence); the caller decodes RGBA and encodes the planes
	inline constexpr char kYuvMismatch[] = "yuv frame mismatch";
	// tight RGBA rows to the planes of the shape (orientation 1) behind a YuvHeader
	std::string EncodeYuv(const uint8_t* rgba, int w, int h, const YuvShape& shape, uint8_t* frame, uint64_t capacity);

	inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
	inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
	inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
	inline uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

	// file contents; resize leaves new bytes uninitialised (the read overwrites them)
	template <class T> struct NoInitAllocator : std::allocator<T>
	{
		template <class U> struct rebind { using other = NoInitAllocator<U>; };
		template <class U> void construct(U* p) noexcept { ::new ((void*)p) U; }
	};
	using Bytes = std::vector<uint8_t, NoInitAllocator<uint8_t>>;
	std::string ReadFile(const std::wstring& path, Bytes& out, size_t limit = SIZE_MAX);
	// a read-only view of a whole file; data is null when it cannot be mapped
	struct MappedFile
	{
		const uint8_t* data = nullptr;
		size_t size = 0;

		explicit MappedFile(const std::wstring& path);
		~MappedFile();
		MappedFile(const MappedFile&) = delete;
	private:
		void* m_file = nullptr;
		void* m_mapping = nullptr;
	};
	// tightly packed rows into the destination, clipped to maxW x maxH
	void CopyRows(const uint8_t* src, int w, int h, uint8_t* dst, size_t pitch, int maxW, int maxH, size_t bytesPerPixel = 4);

	// PNG through libdeflate: RGB without a colour key decodes to 3-channel pixels, 16-bit files to 16-bit samples, everything else to RGBA
	std::string ProbePng(const uint8_t* data, size_t size, Probe& out);
	std::string DecodePng(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});

	// 8-bit RGBA
	std::string ProbeBmp(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeBmp(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	std::string ProbeTga(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeTga(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	// Softimage PIC: RGB (3-channel pixels) or RGBA, 16-bit samples when any channel packet is 16-bit
	std::string ProbePic(const uint8_t* data, size_t size, Probe& out);
	std::string DecodePic(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	// JPEG through libjpeg-turbo: 8-bit YCbCr and gray files of the known subsamplings decode to their planes (Decode*Yuv), anything else to RGBA
	std::string ProbeJpeg(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeJpeg(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	// the planes behind a YuvHeader at frame, shown dispW x dispH after the orientation
	std::string DecodeJpegYuv(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int dispW, int dispH, int orientation = 1);
	// Canon CR3: its embedded full-size JPEG preview, oriented; reads only the moov box and the JPEG
	std::string ProbeCr3(const std::wstring& path, Probe& out);
	std::string DecodeCr3(const std::wstring& path, uint8_t* dst, size_t pitch, int maxW, int maxH);
	std::string DecodeCr3Yuv(const std::wstring& path, uint8_t* frame, uint64_t capacity, int dispW, int dispH);
	// Canon's metadata boxes CMT1-CMT4 (TIFF structures: IFD0, Exif IFD, maker notes, GPS IFD); missing ones stay empty
	std::string ReadCr3Metadata(const std::wstring& path, std::vector<uint8_t> cmt[4]);

	// multi-frame formats: Decode* gives the first frame, Decode*Frames all of them on one canvas, w * h * 4 bytes each
	std::string ProbeGif(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeGif(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	std::string DecodeGifFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames);
	std::string ProbeWebp(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeWebp(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	std::string DecodeWebpFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames);
	// ICO / CUR: one frame per entry, largest first, each centred on the largest entry's canvas
	std::string ProbeIco(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeIco(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	std::string DecodeIcoFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames);

	// SVG: rendered over white at its own size, the long side capped at 4096 (SvgImageSize)
	void SvgImageSize(float docW, float docH, int& w, int& h);
	// lunasvg (plutovg built without its font-face-cache feature) only draws text in faces it was given: the common Windows families, memory-mapped; call once before the first SVG decode
	void RegisterSvgFonts();
	std::string ProbeSvg(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeSvg(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});

	// HEIC / HEIF through libheif: the primary image, rotated and mirrored as stored; RGBA8, or RGBA16 for deeper images;
	// 8-bit 4:2:0 full-range images libheif would convert by its nearest-chroma 8.8 path keep their planes (DecodeHeicYuv)
	std::string ProbeHeic(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeHeic(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	std::string DecodeHeicYuv(const uint8_t* data, size_t size, uint8_t* frame, uint64_t capacity, int dispW, int dispH, const Parallel& par = {});
	// TIFF (and .tx textures) through libtiff: 8 / 16-bit integer or 16 / 32-bit float RGBA; pages are frames centred on the largest, the levels of a mip pyramid pages of their own size
	std::string ProbeTiff(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeTiffPage(const uint8_t* data, size_t size, int page, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
	// linear RGB to Rec.709 primaries from an ICC profile's colorants (D50 PCS); false when it has none
	bool IccToRec709(const uint8_t* icc, size_t size, float m[9]);
	// the same from the chromaticities of red, green, blue and white
	bool PrimariesToRec709(const float xy[8], float m[9]);

	// Radiance HDR: 4-byte RGBE pixels (DecodeRgbe); XYZE files keep XYZ with the matrix to sRGB in Probe
	std::string ProbeHdr(const uint8_t* data, size_t size, Probe& out);
	std::string DecodeHdr(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel& par = {});
}
