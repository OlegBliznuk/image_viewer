#pragma once

#include "image_io.h"
#include "streamer.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace iv
{
	// decodes the first frame of the next browse item on its own thread, straight into a cache buffer the Streamer adopts
	class Prefetcher
	{
	public:
		struct Result
		{
			std::wstring path;
			ImageInfo info;
			Layout layout;
			CacheBuffer pixels;
		};

		~Prefetcher();
		void Request(const std::wstring& path, Streamer& streamer, const Parallel& par);
		enum class Taken { No, Yes, Abandoned };
		// the prefetched decode of path, waiting while it is under way (onWait runs every 40 ms of it; false gives up and cancels it); a decode of another path is cancelled
		Taken Take(const std::wstring& path, Result& out, const std::function<bool()>& onWait);
		// a finished decode the GPU may read ahead of the step, without taking it (Open waits for the GPU before a buffer changes hands)
		bool Ready(std::wstring& path, Layout& layout, ID3D12Resource*& pixels, bool& srgb);
	private:
		void Loop();

		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		Streamer* m_streamer = nullptr;
		Parallel m_par;
		std::wstring m_request;
		std::wstring m_working;
		Result m_result;
		CancelToken m_cancel;   // the decode under way
		bool m_stop = false;
	};
}
