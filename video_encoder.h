#pragma once

#include "parallel.h"
#include "yuv.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct IMFSinkWriter;
struct EbComponentType;

namespace iv
{
	enum class VideoCodec { Hevc, H264, Av1 };

	struct VideoEncoderDesc
	{
		VideoCodec codec = VideoCodec::Hevc;
		std::string name;
		bool hardware = false;
	};

	// the Media Foundation encoders for H.265 and H.264, hardware first, then SVT-AV1 when SvtAv1Enc.dll sits next to the exe (mfplat, mfreadwrite and SvtAv1Enc are delay-loaded:
	// this or VideoEncoder::Open loads them); empty with err set when Media Foundation is missing
	std::vector<VideoEncoderDesc> ListVideoEncoders(std::string& err);

	struct VideoSettings
	{
		VideoCodec codec = VideoCodec::Hevc;
		int width = 0;   // even
		int height = 0;   // even
		uint32_t fpsNum = 24;
		uint32_t fpsDen = 1;
		int quality = 70;   // 1..100: quality-based rate control (H.265 / H.264), an SVT-AV1 CRF giving about the same picture (AV1)
		int gop = 48;   // frames between key frames
		bool hardware = true;   // false: software encoders only (H.265 / H.264)
	};

	// an MP4 file from Media Foundation's sink writer: H.265 / H.264 encoded by the encoder it picks (hardware first) from NV12, AV1 encoded by SVT-AV1 from I420 and passed through;
	// frames are BT.709 limited range with the sRGB curve they carry, and tagged so
	class VideoEncoder
	{
	public:
		VideoEncoder() = default;
		VideoEncoder(const VideoEncoder&) = delete;
		VideoEncoder& operator=(const VideoEncoder&) = delete;
		~VideoEncoder();
		// the calling thread must have COM initialised
		std::string Open(const std::wstring& path, const VideoSettings& s);
		// fill writes the frame into the target the encoder takes; frames in presentation order, times in 100 ns units
		std::string Write(int64_t start, int64_t duration, FnRef<void(const Yuv420Target&)> fill);
		std::string Finish();
		// closes without finishing and deletes the file
		void Abort();
	private:
		void Close();
		std::string OpenAv1();
		std::string DrainAv1(bool final);
		std::string WriteSample(const uint8_t* data, size_t size, int64_t start, int64_t duration, bool key);

		IMFSinkWriter* m_writer = nullptr;
		unsigned long m_stream = 0;
		bool m_started = false;
		bool m_created = false;   // the file exists and is not finished
		std::wstring m_path;
		VideoSettings m_settings;
		EbComponentType* m_svt = nullptr;
		std::vector<uint8_t> m_frame;   // the I420 frame SVT-AV1 copies in
		std::vector<std::pair<int64_t, int64_t>> m_times;   // start and duration per frame: SVT-AV1 hands packets back later, numbered by frame
	};
}
