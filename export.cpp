#include "export.h"
#include "picture.h"
#include "streamer.h"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <filesystem>

namespace iv
{
	namespace
	{
		double Now()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	void VideoExport::Start(ExportJob job, Streamer* streamer, uint64_t generation, const Parallel& par)
	{
		Stop();
		m_cancel.Reset();
		{
			std::lock_guard lock(m_mutex);
			m_status = {};
			m_status.running = true;
			m_status.total = (int)job.files.size();
		}
		Parallel p = par;
		p.cancel = &m_cancel;
		m_thread = std::thread([this, job = std::move(job), streamer, generation, p]() mutable { Run(std::move(job), streamer, generation, p); });
	}

	void VideoExport::Stop()
	{
		m_cancel.Cancel();
		if (m_thread.joinable())
			m_thread.join();
	}

	VideoExport::Status VideoExport::Get() const
	{
		std::lock_guard lock(m_mutex);
		return m_status;
	}

	bool VideoExport::Running() const
	{
		std::lock_guard lock(m_mutex);
		return m_status.running;
	}

	bool VideoExport::Busy() const
	{
		std::lock_guard lock(m_mutex);
		return m_status.running || m_status.finished;
	}

	bool VideoExport::TakeResult(Status& out)
	{
		std::lock_guard lock(m_mutex);
		if (!m_status.finished)
			return false;
		out = m_status;
		m_status.finished = false;
		return true;
	}

	void VideoExport::Run(ExportJob job, Streamer* streamer, uint64_t generation, Parallel par)
	{
		const double start = Now();
		const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		std::string err;
		bool cancelled = false;
		{
			VideoEncoder enc;
			err = enc.Open(job.path, job.video);
			std::vector<uint8_t> pixels;
			FrameSet frames;
			double t = 0.0;
			for (int i = 0; i < (int)job.files.size() && err.empty() && !m_cancel.Cancelled(); ++i)
			{
				if (!streamer || !streamer->CopyFrame(job.indices[i], pixels, generation))
				{
					pixels.resize(job.layout.size + 64);
					err = job.multiFrame ? DecodeImage(job.files[i], job.kind, job.view, job.layout, pixels.data(), par, job.indices[i], &frames) : DecodeImage(job.files[i], job.kind, job.view, job.layout, pixels.data(), par);
					if (!err.empty())
					{
						if (err != kDecodeCancelled)
							err = ToUtf8(std::filesystem::path(job.files[i]).filename().wstring()) + ": " + err;
						break;
					}
				}
				const SourceRows src(job.layout, pixels.data(), job.params);
				const int64_t at = std::llround(t * 1e7);
				t += job.durations[i];
				err = enc.Write(at, std::llround(t * 1e7) - at, [&](const Yuv420Target& target) { ToYuv420(src, job.scale, job.video.width, job.video.height, target, job.adjust, par); });
				std::lock_guard lock(m_mutex);
				m_status.done = i + 1, m_status.seconds = Now() - start;
			}
			cancelled = m_cancel.Cancelled();
			if (err.empty() && !cancelled)
				err = enc.Finish();
			if (!err.empty() || cancelled)
				enc.Abort();
		}
		if (SUCCEEDED(co))
			CoUninitialize();
		std::lock_guard lock(m_mutex);
		m_status.running = false, m_status.finished = true, m_status.cancelled = cancelled;
		m_status.error = cancelled ? std::string() : err;
		m_status.seconds = Now() - start;
	}
}
