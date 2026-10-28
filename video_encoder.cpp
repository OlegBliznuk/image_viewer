#include "video_encoder.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <wrl/client.h>
#define EB_DLL
#include <svt-av1/EbSvtAv1Enc.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "SvtAv1Enc.lib")

using Microsoft::WRL::ComPtr;

namespace iv
{
	namespace
	{
		// both are delay-loaded: without them (Windows N lacking the Media Feature Pack) the first call would fault
		bool MediaFoundationPresent()
		{
			return LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) && LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		}

		constexpr char kNoMediaFoundation[] = "Media Foundation is not installed (Windows N editions: add the Media Feature Pack)";

		// SvtAv1Enc.dll ships next to the exe and is delay-loaded too
		std::wstring SvtPath()
		{
			wchar_t exe[MAX_PATH];
			const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
			std::wstring dir(exe, n);
			return dir.substr(0, dir.find_last_of(L'\\') + 1) + L"SvtAv1Enc.dll";
		}

		std::string Failed(const char* what, HRESULT hr)
		{
			char buf[128];
			snprintf(buf, sizeof(buf), "%s (0x%08X)", what, (unsigned)hr);
			return buf;
		}

		std::string Narrow(const wchar_t* s)
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
			std::string out(n > 0 ? n - 1 : 0, '\0');
			if (n > 1)
				WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
			return out;
		}

		const GUID& Subtype(VideoCodec c) { return c == VideoCodec::Hevc ? MFVideoFormat_HEVC : c == VideoCodec::H264 ? MFVideoFormat_H264 : MFVideoFormat_AV1; }

		// what the stream types share: size, rate, progressive, square pixels, BT.709 limited range with the sRGB curve the pixels carry
		void SetFrame(IMFMediaType* t, const VideoSettings& s)
		{
			t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
			MFSetAttributeSize(t, MF_MT_FRAME_SIZE, s.width, s.height);
			MFSetAttributeRatio(t, MF_MT_FRAME_RATE, s.fpsNum, s.fpsDen);
			MFSetAttributeRatio(t, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
			t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
			t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
			t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
			t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_sRGB);
		}

		// the slider as an SVT-AV1 CRF, placed so a position gives about the PSNR the NVIDIA H.265 encoder reaches there (1080p pan: q85 ~ CRF 30, 43.6 dB at a fifth of the bitrate)
		int Av1Crf(int quality)
		{
			const int q[] = { 1, 50, 70, 85, 95, 100 }, crf[] = { 63, 60, 49, 30, 20, 12 };
			quality = std::clamp(quality, 1, 100);
			int i = 1;
			while (i < 5 && quality > q[i])
				++i;
			return (int)std::lround(crf[i - 1] + (double)(crf[i] - crf[i - 1]) * (quality - q[i - 1]) / (q[i] - q[i - 1]));
		}

		// SVT-AV1 allocates its picture pools at init and they grow fast with the parallelism (commit at 3840 x 2160, levels 1-4: preset 6 2.0 / 2.6 / 3.6 / 5.2 GB,
		// preset 8 1.2 / 1.6 / 2.3 / 3.2 GB; the default 6 failed with 10 GB of commit free); frames up to 2.4 MP keep SVT's choice
		uint32_t Av1Parallelism(int preset, int width, int height)
		{
			const double mp = (double)width * height / 1e6;
			if (mp <= 2.4)
				return 0;
			const double at4k[2][4] = { { 1.97, 2.60, 3.59, 5.20 }, { 1.24, 1.59, 2.25, 3.22 } };
			MEMORYSTATUSEX m = { sizeof(m) };
			GlobalMemoryStatusEx(&m);
			const double avail = (double)m.ullAvailPageFile / 1073741824.0;
			for (int lp = 4; lp > 1; --lp)
				if (at4k[preset == 6 ? 0 : 1][lp - 1] * mp / 8.29 <= 0.6 * avail)
					return (uint32_t)lp;
			return 1;
		}
	}

	std::vector<VideoEncoderDesc> ListVideoEncoders(std::string& err)
	{
		std::vector<VideoEncoderDesc> out;
		if (!MediaFoundationPresent())
		{
			err = kNoMediaFoundation;
			return out;
		}
		for (VideoCodec codec : { VideoCodec::Hevc, VideoCodec::H264 })
			for (bool hardware : { true, false })
			{
				MFT_REGISTER_TYPE_INFO info = { MFMediaType_Video, Subtype(codec) };
				IMFActivate** acts = nullptr;
				UINT32 n = 0;
				if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, (hardware ? MFT_ENUM_FLAG_HARDWARE : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT) | MFT_ENUM_FLAG_SORTANDFILTER, nullptr, &info, &acts, &n)))
					continue;
				for (UINT32 i = 0; i < n; ++i)
				{
					WCHAR* name = nullptr;
					UINT32 len = 0;
					acts[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len);
					out.push_back({ codec, name ? Narrow(name) : std::string("unnamed encoder"), hardware });
					CoTaskMemFree(name);
					acts[i]->Release();
				}
				CoTaskMemFree(acts);
			}
		if (GetFileAttributesW(SvtPath().c_str()) != INVALID_FILE_ATTRIBUTES)
			out.push_back({ VideoCodec::Av1, "SVT-AV1 " + std::to_string(SVT_AV1_VERSION_MAJOR) + "." + std::to_string(SVT_AV1_VERSION_MINOR) + "." + std::to_string(SVT_AV1_VERSION_PATCHLEVEL), false });
		return out;
	}

	VideoEncoder::~VideoEncoder()
	{
		Close();
	}

	void VideoEncoder::Close()
	{
		if (m_svt)
		{
			svt_av1_enc_deinit(m_svt);
			svt_av1_enc_deinit_handle(m_svt);
			m_svt = nullptr;
		}
		if (m_writer)
			m_writer->Release(), m_writer = nullptr;
		if (m_started)
			MFShutdown(), m_started = false;
	}

	std::string VideoEncoder::Open(const std::wstring& path, const VideoSettings& s)
	{
		Close();
		if (!MediaFoundationPresent())
			return kNoMediaFoundation;
		HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		if (FAILED(hr))
			return Failed("Media Foundation did not start", hr);
		m_started = true, m_path = path, m_settings = s;
		if (s.codec == VideoCodec::Av1)
			return OpenAv1();
		ComPtr<IMFAttributes> attrs;
		MFCreateAttributes(&attrs, 2);
		attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, s.hardware ? TRUE : FALSE);
		attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
		if (FAILED(hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attrs.Get(), &m_writer)))
			return Failed("cannot create the file", hr);
		m_created = true;
		ComPtr<IMFMediaType> out, in;
		MFCreateMediaType(&out);
		SetFrame(out.Get(), s);
		out->SetGUID(MF_MT_SUBTYPE, Subtype(s.codec));
		out->SetUINT32(MF_MT_VIDEO_PROFILE, s.codec == VideoCodec::Hevc ? (UINT32)eAVEncH265VProfile_Main_420_8 : (UINT32)eAVEncH264VProfile_High);
		// quality mode ignores it, some encoders refuse a type without one
		out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)std::min<double>(4e9, (double)s.width * s.height * s.fpsNum / s.fpsDen * 0.2));
		if (FAILED(hr = m_writer->AddStream(out.Get(), &m_stream)))
			return Failed("no encoder for this codec and size", hr);
		MFCreateMediaType(&in);
		SetFrame(in.Get(), s);
		in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)s.width);
		ComPtr<IMFAttributes> enc;
		MFCreateAttributes(&enc, 4);
		enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
		enc->SetUINT32(CODECAPI_AVEncCommonQuality, (UINT32)std::clamp(s.quality, 1, 100));
		// 100 measured no better than 50 (NVIDIA HEVC, 1080p: same PSNR, 89 vs 198 fps)
		enc->SetUINT32(CODECAPI_AVEncCommonQualityVsSpeed, 50);
		enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, (UINT32)std::max(1, s.gop));
		if (FAILED(hr = m_writer->SetInputMediaType(m_stream, in.Get(), enc.Get())))
			return Failed("the encoder refused this frame size or rate", hr);
		if (FAILED(hr = m_writer->BeginWriting()))
			return Failed("the encoder did not start", hr);
		return {};
	}

	// SVT-AV1 (preset 6 up to 2.4 MP: 87 fps at 1080p; preset 8 above: 32-37 fps at 4K) with closed GOPs, its temporal units passed through to the MP4 sink
	std::string VideoEncoder::OpenAv1()
	{
		const VideoSettings& s = m_settings;
		if (s.width < 64 || s.height < 64)
			return "AV1 needs at least 64 x 64 pixels";
		if (!LoadLibraryExW(SvtPath().c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH))
			return "SvtAv1Enc.dll is missing next to image_viewer.exe";
		const int preset = (double)s.width * s.height <= 2.4e6 ? 6 : 8;
		uint32_t lp = Av1Parallelism(preset, s.width, s.height);
		while (true)
		{
			EbSvtAv1EncConfiguration c;
			if (svt_av1_enc_init_handle(&m_svt, &c) != EB_ErrorNone)
			{
				m_svt = nullptr;
				return "the AV1 encoder did not start";
			}
			c.enc_mode = (int8_t)preset;
			c.source_width = (uint32_t)s.width, c.source_height = (uint32_t)s.height;
			c.frame_rate_numerator = s.fpsNum, c.frame_rate_denominator = s.fpsDen;
			c.encoder_bit_depth = 8;
			c.rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
			c.qp = (uint32_t)Av1Crf(s.quality);
			c.intra_refresh_type = SVT_AV1_KF_REFRESH;
			c.intra_period_length = (std::max(s.gop, 1) + 31) / 32 * 32;
			c.color_primaries = EB_CICP_CP_BT_709, c.transfer_characteristics = EB_CICP_TC_SRGB, c.matrix_coefficients = EB_CICP_MC_BT_709, c.color_range = EB_CR_STUDIO_RANGE;
			c.level_of_parallelism = lp;
			if (svt_av1_enc_set_parameter(m_svt, &c) != EB_ErrorNone)
			{
				svt_av1_enc_deinit_handle(m_svt);
				m_svt = nullptr;
				return "the AV1 encoder refused these settings";
			}
			if (svt_av1_enc_init(m_svt) == EB_ErrorNone)
				break;
			svt_av1_enc_deinit(m_svt);
			svt_av1_enc_deinit_handle(m_svt);
			m_svt = nullptr;
			// out of memory for the picture pools: fewer pictures in flight
			if (lp == 1)
				return "not enough memory for the AV1 encoder at this size";
			lp = lp == 0 ? 3 : lp - 1;
		}
		EbBufferHeaderType* header = nullptr;
		if (svt_av1_enc_stream_header(m_svt, &header) != EB_ErrorNone || !header)
			return "the AV1 encoder gave no sequence header";
		ComPtr<IMFMediaType> t;
		MFCreateMediaType(&t);
		SetFrame(t.Get(), s);
		t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AV1);
		t->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header->p_buffer, header->n_filled_len);
		svt_av1_enc_stream_header_release(header);
		ComPtr<IMFAttributes> attrs;
		MFCreateAttributes(&attrs, 1);
		attrs->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
		HRESULT hr;
		if (FAILED(hr = MFCreateSinkWriterFromURL(m_path.c_str(), nullptr, attrs.Get(), &m_writer)))
			return Failed("cannot create the file", hr);
		m_created = true;
		if (FAILED(hr = m_writer->AddStream(t.Get(), &m_stream)) || FAILED(hr = m_writer->SetInputMediaType(m_stream, t.Get(), nullptr)))
			return Failed("this Windows cannot write AV1 into MP4", hr);
		if (FAILED(hr = m_writer->BeginWriting()))
			return Failed("the MP4 writer did not start", hr);
		m_frame.resize((size_t)s.width * s.height * 3 / 2);
		m_times.clear();
		return {};
	}

	std::string VideoEncoder::WriteSample(const uint8_t* data, size_t size, int64_t start, int64_t duration, bool key)
	{
		ComPtr<IMFMediaBuffer> buffer;
		HRESULT hr = MFCreateMemoryBuffer((DWORD)size, &buffer);
		BYTE* p = nullptr;
		if (SUCCEEDED(hr))
			hr = buffer->Lock(&p, nullptr, nullptr);
		if (FAILED(hr))
			return Failed("no memory for a frame", hr);
		memcpy(p, data, size);
		buffer->Unlock();
		buffer->SetCurrentLength((DWORD)size);
		ComPtr<IMFSample> sample;
		MFCreateSample(&sample);
		sample->AddBuffer(buffer.Get());
		sample->SetSampleTime(start);
		sample->SetSampleDuration(duration);
		sample->SetUINT32(MFSampleExtension_CleanPoint, key ? TRUE : FALSE);
		if (FAILED(hr = m_writer->WriteSample(m_stream, sample.Get())))
			return Failed("writing a frame failed", hr);
		return {};
	}

	// the packets SVT-AV1 has ready (final: all of them, waiting for each); each is one temporal unit, numbered by the frame it shows
	std::string VideoEncoder::DrainAv1(bool final)
	{
		while (true)
		{
			EbBufferHeaderType* out = nullptr;
			const EbErrorType r = svt_av1_enc_get_packet(m_svt, &out, final ? 1 : 0);
			if (r == EB_ErrorMax)
				return "AV1 encoding failed";
			if (r != EB_ErrorNone || !out)
				return {};
			const bool last = (out->flags & EB_BUFFERFLAG_EOS) != 0;
			std::string err;
			if (out->n_filled_len)
			{
				// MP4 samples carry no temporal delimiter (the OBU 0x12 0x00 that starts each unit)
				const uint8_t* p = out->p_buffer;
				size_t n = out->n_filled_len;
				if (n >= 2 && p[0] == 0x12 && p[1] == 0)
					p += 2, n -= 2;
				const std::pair<int64_t, int64_t> t = out->pts >= 0 && out->pts < (int64_t)m_times.size() ? m_times[(size_t)out->pts] : std::pair<int64_t, int64_t>(0, 0);
				err = WriteSample(p, n, t.first, t.second, out->pic_type == EB_AV1_KEY_PICTURE);
			}
			svt_av1_enc_release_out_buffer(&out);
			if (!err.empty())
				return err;
			if (last)
				return {};
		}
	}

	std::string VideoEncoder::Write(int64_t start, int64_t duration, FnRef<void(const Yuv420Target&)> fill)
	{
		if (!m_writer)
			return "no video file open";
		const int w = m_settings.width, h = m_settings.height;
		if (m_svt)
		{
			fill(I420Target(m_frame.data(), w, h));
			EbSvtIOFormat io = { m_frame.data(), m_frame.data() + (size_t)w * h, m_frame.data() + (size_t)w * h * 5 / 4, (uint32_t)w, (uint32_t)w / 2, (uint32_t)w / 2 };
			EbBufferHeaderType in = {};
			in.size = sizeof(in);
			in.p_buffer = (uint8_t*)&io;
			in.n_filled_len = in.n_alloc_len = (uint32_t)m_frame.size();
			in.pts = (int64_t)m_times.size();
			in.pic_type = EB_AV1_INVALID_PICTURE;
			m_times.push_back({ start, duration });
			if (svt_av1_enc_send_picture(m_svt, &in) != EB_ErrorNone)
				return "the AV1 encoder refused a frame";
			return DrainAv1(false);
		}
		const DWORD size = (DWORD)((size_t)w * h * 3 / 2);
		ComPtr<IMFMediaBuffer> buffer;
		HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
		BYTE* p = nullptr;
		if (SUCCEEDED(hr))
			hr = buffer->Lock(&p, nullptr, nullptr);
		if (FAILED(hr))
			return Failed("no memory for a frame", hr);
		fill(Nv12Target(p, w, h));
		buffer->Unlock();
		buffer->SetCurrentLength(size);
		ComPtr<IMFSample> sample;
		MFCreateSample(&sample);
		sample->AddBuffer(buffer.Get());
		sample->SetSampleTime(start);
		sample->SetSampleDuration(duration);
		if (FAILED(hr = m_writer->WriteSample(m_stream, sample.Get())))
			return Failed("encoding failed", hr);
		return {};
	}

	std::string VideoEncoder::Finish()
	{
		if (!m_writer)
			return "no video file open";
		if (m_svt)
		{
			EbBufferHeaderType eos = {};
			eos.size = sizeof(eos);
			eos.flags = EB_BUFFERFLAG_EOS;
			svt_av1_enc_send_picture(m_svt, &eos);
			const std::string err = DrainAv1(true);
			if (!err.empty())
				return err;
		}
		const HRESULT hr = m_writer->Finalize();
		Close();
		if (FAILED(hr))
			return Failed("finishing the file failed", hr);
		m_created = false;
		return {};
	}

	void VideoEncoder::Abort()
	{
		Close();
		if (m_created)
			DeleteFileW(m_path.c_str()), m_created = false;
	}
}
