#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace iv
{
	// lets big allocations go on its own thread: D3D12 heaps and committed buffers take 40-60 us per MB to release (60 cached 4K frames: 170 ms)
	class Disposer
	{
	public:
		~Disposer();
		// fn runs on the disposer thread; it typically only drops what it captured
		void Post(std::move_only_function<void()> fn);
		// returns once everything posted so far has run
		void Drain();
	private:
		void Loop();

		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		std::vector<std::move_only_function<void()>> m_queue;
		bool m_running = false;   // the thread is working through a batch
		bool m_stop = false;
	};
}
