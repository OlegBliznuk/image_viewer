#include "image_formats.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <cstring>

#pragma comment(lib, "libwebp.lib")
#pragma comment(lib, "libwebpdemux.lib")

// WebP through libwebp, animations through libwebpdemux (vcpkg, both DLLs delay-loaded)
namespace iv::fmt
{
	namespace
	{
		// browsers show durations of 10 ms and less as 100 ms
		float FrameDelay(int ms) { return ms <= 10 ? 0.1f : ms / 1000.0f; }

		struct AnimDecoder
		{
			WebPAnimDecoder* dec = nullptr;
			WebPAnimInfo info = {};
			~AnimDecoder() { if (dec) WebPAnimDecoderDelete(dec); }

			std::string Open(const uint8_t* data, size_t size)
			{
				WebPAnimDecoderOptions opts;
				if (!WebPAnimDecoderOptionsInit(&opts))
					return "webp: decoder version mismatch";
				opts.color_mode = MODE_RGBA;
				opts.use_threads = 1;
				const WebPData wd = { data, size };
				dec = WebPAnimDecoderNew(&wd, &opts);
				return dec && WebPAnimDecoderGetInfo(dec, &info) && info.frame_count > 0 ? std::string() : "webp: bad animation";
			}
		};

	}

	std::string ProbeWebp(const uint8_t* data, size_t size, Probe& out)
	{
		WebPBitstreamFeatures f;
		if (WebPGetFeatures(data, size, &f) != VP8_STATUS_OK)
			return "webp: bad header";
		out.w = f.width;
		out.h = f.height;
		if (!f.has_animation)
			return {};
		const WebPData wd = { data, size };
		WebPDemuxer* demux = WebPDemux(&wd);
		if (!demux)
			return "webp: bad container";
		out.w = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH);
		out.h = (int)WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT);
		WebPIterator it;
		if (WebPDemuxGetFrame(demux, 1, &it))
		{
			do
				out.delays.push_back(FrameDelay(it.duration));
			while (WebPDemuxNextFrame(&it));
			WebPDemuxReleaseIterator(&it);
		}
		WebPDemuxDelete(demux);
		return out.delays.empty() ? "webp: no frames" : std::string();
	}

	std::string DecodeWebp(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel&)
	{
		WebPBitstreamFeatures f;
		if (WebPGetFeatures(data, size, &f) != VP8_STATUS_OK)
			return "webp: bad header";
		if (f.has_animation)
		{
			AnimDecoder a;
			if (std::string err = a.Open(data, size); !err.empty())
				return err;
			uint8_t* canvas = nullptr;
			int timestamp = 0;
			if (!WebPAnimDecoderGetNext(a.dec, &canvas, &timestamp))
				return "webp: frame decode failed";
			CopyRows(canvas, (int)a.info.canvas_width, (int)a.info.canvas_height, dst, pitch, maxW, maxH);
			return {};
		}
		if (f.width <= maxW && f.height <= maxH)
			return WebPDecodeRGBAInto(data, size, dst, pitch * f.height, (int)pitch) ? std::string() : "webp: decode failed";
		int w = 0, h = 0;
		uint8_t* px = WebPDecodeRGBA(data, size, &w, &h);
		if (!px)
			return "webp: decode failed";
		CopyRows(px, w, h, dst, pitch, maxW, maxH);
		WebPFree(px);
		return {};
	}

	std::string DecodeWebpFrames(const uint8_t* data, size_t size, int& w, int& h, int& count, std::vector<uint8_t>& frames)
	{
		AnimDecoder a;
		if (std::string err = a.Open(data, size); !err.empty())
			return err;
		w = (int)a.info.canvas_width;
		h = (int)a.info.canvas_height;
		const size_t frameBytes = (size_t)w * h * 4;
		frames.resize(frameBytes * a.info.frame_count);
		count = 0;
		while (count < (int)a.info.frame_count && WebPAnimDecoderHasMoreFrames(a.dec))
		{
			uint8_t* canvas = nullptr;
			int timestamp = 0;
			if (!WebPAnimDecoderGetNext(a.dec, &canvas, &timestamp))
				break;
			memcpy(frames.data() + frameBytes * count++, canvas, frameBytes);
		}
		frames.resize(frameBytes * count);
		return count ? std::string() : "webp: frame decode failed";
	}
}
