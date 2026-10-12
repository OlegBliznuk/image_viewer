#include "yuv.h"

#include <algorithm>
#include <cstring>

namespace iv
{
	namespace
	{
		uint32_t Align64(uint32_t v) { return (v + 63) & ~63u; }

		// planes as libjpeg-turbo sizes them (tj3YUVPlaneWidth / Height): luma padded to whole chroma samples
		struct Planes
		{
			uint32_t yPitch, yRows, cWidth, cHeight, cPitch;
			Planes(int w, int h, const YuvShape& s)
			{
				const uint32_t pw = ((uint32_t)w + (1u << s.sx) - 1) >> s.sx << s.sx, ph = ((uint32_t)h + (1u << s.sy) - 1) >> s.sy << s.sy;
				yPitch = Align64(pw), yRows = ph;
				cWidth = s.planes == 3 ? pw >> s.sx : 0, cHeight = s.planes == 3 ? ph >> s.sy : 0;
				cPitch = Align64(cWidth);
			}
		};

		int Clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
	}

	uint64_t YuvFrameSize(int w, int h, const YuvShape& s)
	{
		const Planes p(w, h, s);
		return kYuvHeaderBytes + (uint64_t)p.yPitch * p.yRows + 2 * (uint64_t)p.cPitch * p.cHeight;
	}

	YuvHeader& InitYuvFrame(uint8_t* frame, int w, int h, const YuvShape& s, YuvMode mode)
	{
		const Planes p(w, h, s);
		memset(frame, 0, kYuvHeaderBytes);
		YuvHeader& hd = *(YuvHeader*)frame;
		hd.width = (uint32_t)w, hd.height = (uint32_t)h;
		hd.yOffset = kYuvHeaderBytes, hd.yPitch = p.yPitch;
		hd.cbOffset = s.planes == 3 ? hd.yOffset + p.yPitch * p.yRows : 0;
		hd.crOffset = s.planes == 3 ? hd.cbOffset + p.cPitch * p.cHeight : 0;
		hd.cPitch = p.cPitch, hd.cWidth = p.cWidth, hd.cHeight = p.cHeight;
		hd.sx = s.sx, hd.sy = s.sy, hd.orientation = s.orientation, hd.planes = s.planes;
		hd.mode = (uint32_t)mode;
		// libjpeg-turbo (jdsample.c) interpolates 2h1v and 2h2v chroma wider than 2 samples and 1h2v always; it replicates everything else
		hd.fancy = s.planes == 3 && ((s.sx == 1 && s.sy <= 1 && p.cWidth > 2) || (s.sx == 0 && s.sy == 1));
		return hd;
	}

	YuvReader::YuvReader(const uint8_t* frame)
	{
		memset(&m_h, 0, sizeof(m_h));
		if (frame)
			memcpy(&m_h, frame, sizeof(m_h));
		m_y = frame + m_h.yOffset;
		m_cb = frame + m_h.cbOffset;
		m_cr = frame + m_h.crOffset;
	}

	bool YuvReader::Fits(uint64_t size, int dispW, int dispH) const
	{
		const YuvHeader& h = m_h;
		if (h.orientation < 1 || h.orientation > 8 || (h.planes != 1 && h.planes != 3) || h.width == 0 || h.height == 0 || h.sx > 2 || h.sy > 2)
			return false;
		const bool turned = h.orientation >= 5;
		if ((int)(turned ? h.height : h.width) != dispW || (int)(turned ? h.width : h.height) != dispH)
			return false;
		if (h.yPitch < h.width || h.yOffset < kYuvHeaderBytes || h.yOffset + (uint64_t)h.yPitch * h.height > size)
			return false;
		if (h.planes == 1)
			return true;
		const uint32_t cw = (h.width + (1u << h.sx) - 1) >> h.sx, ch = (h.height + (1u << h.sy) - 1) >> h.sy;
		return h.cWidth >= cw && h.cHeight >= ch && h.cPitch >= h.cWidth && h.cbOffset + (uint64_t)h.cPitch * h.cHeight <= size && h.crOffset + (uint64_t)h.cPitch * h.cHeight <= size;
	}

	// h2v1 (3c + n + 1|2) >> 2, h1v2 the same down the column, h2v2 (3 colsum + colsum' + 8|7) >> 4 with colsum = 3 near row + far row (jdsample.c)
	int YuvReader::Chroma(const uint8_t* p, int c, int r) const
	{
		const int cw = (int)m_h.cWidth - 1, ch = (int)m_h.cHeight - 1, pitch = (int)m_h.cPitch;
		if (!m_h.fancy)
			return p[(size_t)(r >> m_h.sy) * pitch + (c >> m_h.sx)];
		if (m_h.sy == 0)
		{
			const uint8_t* row = p + (size_t)r * pitch;
			const int cc = c >> 1, v = 3 * row[cc];
			return c & 1 ? (v + row[std::min(cc + 1, cw)] + 2) >> 2 : (v + row[std::max(cc - 1, 0)] + 1) >> 2;
		}
		const int near = r >> 1, far = r & 1 ? std::min(near + 1, ch) : std::max(near - 1, 0);
		const uint8_t* n = p + (size_t)near * pitch;
		const uint8_t* f = p + (size_t)far * pitch;
		if (m_h.sx == 0)
			return (3 * n[c] + f[c] + (r & 1 ? 2 : 1)) >> 2;
		const int cc = c >> 1, side = c & 1 ? std::min(cc + 1, cw) : std::max(cc - 1, 0);
		return (3 * (3 * n[cc] + f[cc]) + 3 * n[side] + f[side] + (c & 1 ? 7 : 8)) >> 4;
	}

	void YuvReader::Pixel(int x, int y, uint8_t rgb[3]) const
	{
		const int W = (int)m_h.width, H = (int)m_h.height;
		int c = x, r = y;
		switch (m_h.orientation)
		{
		case 2: c = W - 1 - x; break;
		case 3: c = W - 1 - x, r = H - 1 - y; break;
		case 4: r = H - 1 - y; break;
		case 5: c = y, r = x; break;
		case 6: c = y, r = H - 1 - x; break;
		case 7: c = W - 1 - y, r = H - 1 - x; break;
		case 8: c = W - 1 - y, r = x; break;
		default: break;
		}
		const int Y = m_y[(size_t)r * m_h.yPitch + c];
		if (m_h.planes != 3)
		{
			rgb[0] = rgb[1] = rgb[2] = (uint8_t)Y;
			return;
		}
		if (m_h.mode == (uint32_t)YuvMode::Heif)
		{
			const size_t o = (size_t)(r >> m_h.sy) * m_h.cPitch + (c >> m_h.sx);
			const int cb = m_cb[o] - 128, cr = m_cr[o] - 128;
			rgb[0] = (uint8_t)Clamp255(Y + ((m_h.k[0] * cr + 128) >> 8));
			rgb[1] = (uint8_t)Clamp255(Y + ((m_h.k[1] * cb + m_h.k[2] * cr + 128) >> 8));
			rgb[2] = (uint8_t)Clamp255(Y + ((m_h.k[3] * cb + 128) >> 8));
			return;
		}
		// jdcolor.c: FIX(1.40200), FIX(1.77200), FIX(0.71414), FIX(0.34414) at 16 fraction bits, ONE_HALF folded in
		const int cb = Chroma(m_cb, c, r) - 128, cr = Chroma(m_cr, c, r) - 128;
		rgb[0] = (uint8_t)Clamp255(Y + ((91881 * cr + 32768) >> 16));
		rgb[1] = (uint8_t)Clamp255(Y + ((-22554 * cb - 46802 * cr + 32768) >> 16));
		rgb[2] = (uint8_t)Clamp255(Y + ((116130 * cb + 32768) >> 16));
	}
}
