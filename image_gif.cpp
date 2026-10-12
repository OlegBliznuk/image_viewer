#include "image_formats.h"

#include <stb_image.h>

#include <cstring>

// GIF through stb_image (built with GIF support in stb_impl.cpp); the frame count and delays come from a walk over the blocks
namespace iv::fmt
{
	namespace
	{
		// browsers show delays of 0 and 10 ms as 100 ms
		float FrameDelay(int centiseconds) { return centiseconds <= 1 ? 0.1f : centiseconds / 100.0f; }

		bool SkipSubBlocks(const uint8_t* p, size_t size, size_t& at)
		{
			while (at < size)
			{
				const uint8_t n = p[at++];
				if (n == 0)
					return true;
				at += n;
			}
			return false;
		}
	}

	std::string ProbeGif(const uint8_t* p, size_t size, Probe& out)
	{
		if (size < 13 || memcmp(p, "GIF8", 4) != 0)
			return "gif: bad header";
		out.w = Le16(p + 6);
		out.h = Le16(p + 8);
		std::vector<float>& delays = out.delays;
		size_t at = 13;
		if (p[10] & 0x80)
			at += (size_t)3 << ((p[10] & 7) + 1);
		int pending = 0;
		delays.clear();
		while (at < size)
		{
			const uint8_t block = p[at++];
			if (block == 0x21 && at < size)
			{
				const uint8_t label = p[at++];
				if (label == 0xF9 && at + 4 < size && p[at] >= 4)
					pending = Le16(p + at + 2);
				if (!SkipSubBlocks(p, size, at))
					break;
			}
			else if (block == 0x2C && at + 9 < size)
			{
				const uint8_t flags = p[at + 8];
				at += 9;
				if (flags & 0x80)
					at += (size_t)3 << ((flags & 7) + 1);
				if (++at > size || !SkipSubBlocks(p, size, at))
					break;
				delays.push_back(FrameDelay(pending));
				pending = 0;
			}
			else
				break;
		}
		if (delays.empty())
			return "gif: no frames";
		return out.w > 0 && out.h > 0 ? std::string() : "gif: bad size";
	}

	std::string DecodeGif(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel&)
	{
		int w = 0, h = 0, comp = 0;
		stbi_uc* px = stbi_load_from_memory(data, (int)size, &w, &h, &comp, 4);
		if (!px)
			return std::string("gif: ") + stbi_failure_reason();
		CopyRows(px, w, h, dst, pitch, maxW, maxH);
		stbi_image_free(px);
		return {};
	}

	std::string DecodeGifFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames)
	{
		int* delays = nullptr;
		int comp = 0;
		stbi_uc* px = stbi_load_gif_from_memory(data, (int)size, &delays, &w, &h, &count, &comp, 4);
		if (!px)
			return std::string("gif: ") + stbi_failure_reason();
		frames.assign(px, px + (size_t)w * h * 4 * count);
		stbi_image_free(px);
		stbi_image_free(delays);
		return {};
	}
}
