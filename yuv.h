#pragma once

#include <cstdint>

// frames kept as YCbCr planes (JPEG, CR3 previews, 8-bit 4:2:0 HEIC): a header, then the Y, Cb and Cr planes;
// unpack_cs.hlsl and YuvReader give the RGB the decoding library would have produced
namespace iv
{
	struct YuvShape
	{
		uint8_t planes = 0;   // 0: the frame is not YCbCr, 1: gray, 3: Y, Cb, Cr
		uint8_t sx = 0;   // chroma subsampling as shifts
		uint8_t sy = 0;
		uint8_t orientation = 1;   // EXIF orientation the planes are shown with
		bool operator==(const YuvShape&) const = default;
	};

	// Jpeg: libjpeg-turbo's upsampling (fancy where it uses it) and fixed-point YCbCr; Heif: libheif's Op_YCbCr420_to_RGB32, nearest chroma and 8.8 coefficients
	enum class YuvMode : uint32_t { Jpeg = 0, Heif = 1 };

	// the frame's first bytes; unpack_cs.hlsl reads the same words
	struct YuvHeader
	{
		uint32_t width, height;   // stored luma size, before the orientation
		uint32_t yOffset, yPitch;
		uint32_t cbOffset, crOffset, cPitch;
		uint32_t cWidth, cHeight;   // chroma samples the upsampling reads; edges clamp to them
		uint32_t sx, sy, orientation, planes;
		uint32_t mode;
		uint32_t fancy;   // Jpeg: libjpeg-turbo interpolates the chroma
		uint32_t pad;
		int32_t k[4];   // Heif: r_cr, g_cb, g_cr, b_cb in 8.8
	};
	constexpr uint32_t kYuvHeaderBytes = 256;

	// where stored pixel (c, r) of W x H planes shows under the EXIF orientation (unpack_cs.hlsl Oriented)
	inline void YuvShown(int orientation, int W, int H, int c, int r, int& x, int& y)
	{
		switch (orientation)
		{
		case 2: x = W - 1 - c, y = r; break;
		case 3: x = W - 1 - c, y = H - 1 - r; break;
		case 4: x = c, y = H - 1 - r; break;
		case 5: x = r, y = c; break;
		case 6: x = H - 1 - r, y = c; break;
		case 7: x = H - 1 - r, y = W - 1 - c; break;
		case 8: x = r, y = W - 1 - c; break;
		default: x = c, y = r; break;
		}
	}

	// where an encoded video frame's 4:2:0 samples go: Y rows, then Cb and Cr cStep bytes apart (NV12: Cr = Cb + 1, step 2; I420: separate planes, step 1)
	struct Yuv420Target
	{
		uint8_t* y;
		uint8_t* cb;
		uint8_t* cr;
		int yStride;
		int cStride;
		int cStep;
	};
	inline Yuv420Target Nv12Target(uint8_t* p, int w, int h) { return { p, p + (size_t)w * h, p + (size_t)w * h + 1, w, w, 2 }; }
	inline Yuv420Target I420Target(uint8_t* p, int w, int h) { return { p, p + (size_t)w * h, p + (size_t)w * h * 5 / 4, w, w / 2, 1 }; }

	// bytes of a frame of stored size w x h
	uint64_t YuvFrameSize(int w, int h, const YuvShape& s);
	// writes the header with the plane offsets; the planes follow it
	YuvHeader& InitYuvFrame(uint8_t* frame, int w, int h, const YuvShape& s, YuvMode mode);

	class YuvReader
	{
	public:
		// null reads as an empty header
		explicit YuvReader(const uint8_t* frame);
		// the header describes planes within size bytes that show dispW x dispH pixels (a slot whose decode failed may hold anything)
		bool Fits(uint64_t size, int dispW, int dispH) const;
		// RGB of displayed pixel (x, y), as unpack_cs.hlsl computes it
		void Pixel(int x, int y, uint8_t rgb[3]) const;
	private:
		int Chroma(const uint8_t* p, int c, int r) const;
		YuvHeader m_h;
		const uint8_t* m_y;
		const uint8_t* m_cb;
		const uint8_t* m_cr;
	};
}
