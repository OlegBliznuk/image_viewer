#include "prefetch.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace iv
{
	Prefetcher::~Prefetcher()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void Prefetcher::Request(const std::wstring& path, Streamer& streamer, const Parallel& par)
	{
		std::lock_guard lock(m_mutex);
		m_streamer = &streamer;
		m_par = par;
		if (path == m_working || path == m_result.path)
		{
			m_request.clear();
			return;
		}
		m_request = path;
		if (!m_working.empty())
			m_cancel.Cancel();
		if (!m_thread.joinable())
			m_thread = std::thread(&Prefetcher::Loop, this);
		m_cv.notify_all();
	}

	Prefetcher::Taken Prefetcher::Take(const std::wstring& path, Result& out, const std::function<bool()>& onWait)
	{
		std::unique_lock lock(m_mutex);
		if (m_request == path)
			m_request.clear();
		if (!m_working.empty() && m_working != path)
			m_cancel.Cancel();
		while (m_working == path)
			if (m_cv.wait_for(lock, std::chrono::milliseconds(40)) == std::cv_status::timeout && onWait)
			{
				lock.unlock();
				const bool wait = onWait();
				lock.lock();
				if (!wait && m_working == path)
				{
					m_cancel.Cancel();
					return Taken::Abandoned;
				}
			}
		if (m_result.path != path || !m_result.pixels.res)
			return Taken::No;
		out = std::exchange(m_result, {});
		return Taken::Yes;
	}

	bool Prefetcher::Ready(std::wstring& path, Layout& layout, ID3D12Resource*& pixels, bool& srgb)
	{
		std::lock_guard lock(m_mutex);
		if (!m_working.empty() || m_result.path.empty() || !m_result.pixels.res)
			return false;
		path = m_result.path;
		layout = m_result.layout;
		pixels = m_result.pixels.res.Get();
		srgb = IsDisplayReferred(m_result.info);
		return true;
	}

	void Prefetcher::Loop()
	{
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_cv.wait(lock, [&] { return m_stop || !m_request.empty(); });
			if (m_stop)
				return;
			m_working = std::move(m_request);
			m_request.clear();
			Result r;
			r.path = m_working;
			// an untaken result hands its buffer on
			r.pixels = std::exchange(m_result.pixels, {});
			Streamer* streamer = m_streamer;
			m_cancel.Reset();
			Parallel par = m_par;
			par.cancel = &m_cancel;
			lock.unlock();
			bool ok = ProbeImage(r.path, r.info).empty();
			if (ok)
			{
				const ViewDesc& view = r.info.views[r.info.defaultView];
				r.layout = MakeLayout(r.info, view);
				ok = streamer->TakeBuffer(r.pixels, r.layout.size).empty() && DecodeImage(r.path, r.info.kind, view, r.layout, r.pixels.cpu, par).empty();
			}
			ok &= !par.Cancelled();
			if (!ok)
				streamer->GiveBuffer(r.pixels);
			lock.lock();
			m_result = std::move(r);
			m_working.clear();
			m_cv.notify_all();
			// wakes the UI loop to upload it ahead
			if (ok)
				SetEvent(streamer->ReadyEvent());
		}
	}
}
