#include "parallel.h"

#include <immintrin.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace iv
{
	namespace
	{
		// a worker woken from the atomic wait needs tens of microseconds, so loops that follow each other closely are caught by spinning first
		constexpr std::chrono::microseconds kSpin{ 50 };

		template <class F> bool Spin(F&& done)
		{
			const auto until = std::chrono::steady_clock::now() + kSpin;
			for (int i = 1;; ++i)
			{
				if (done())
					return true;
				_mm_pause();
				if ((i & 63) == 0 && std::chrono::steady_clock::now() > until)
					return false;
			}
		}
	}

	ThreadPool::ThreadPool(int threads)
	{
		const int n = threads > 0 ? threads : std::max(1, (int)std::thread::hardware_concurrency());
		m_workers.reserve((size_t)n - 1);
		for (int i = 1; i < n; ++i)
			m_workers.emplace_back([this, i] { Loop(i); });
	}

	ThreadPool::~ThreadPool()
	{
		m_stop = true;
		m_epoch.fetch_add(1, std::memory_order_release);
		m_epoch.notify_all();
		for (std::thread& t : m_workers)
			t.join();
	}

	void ThreadPool::Loop(int worker)
	{
		uint32_t seen = 0;
		for (;;)
		{
			if (!Spin([&] { return m_epoch.load(std::memory_order_acquire) != seen; }))
				m_epoch.wait(seen, std::memory_order_acquire);
			seen = m_epoch.load(std::memory_order_acquire);
			if (m_stop)
				return;
			Work(worker);
			if (m_busy.fetch_sub(1, std::memory_order_acq_rel) == 1)
				m_busy.notify_one();
		}
	}

	void ThreadPool::Work(int worker)
	{
		try
		{
			for (;;)
			{
				const int64_t c = m_next.fetch_add(1, std::memory_order_relaxed);
				if (c >= m_chunks || (m_cancel && m_cancel->Cancelled()))
					return;
				(*m_fn)(c * m_grain, std::min(m_count, (c + 1) * m_grain), worker);
			}
		}
		catch (...)
		{
			m_failed.store(true, std::memory_order_relaxed);
			m_next.store(m_chunks, std::memory_order_relaxed);
		}
	}

	void ThreadPool::Run(int64_t count, int64_t grain, FnRef<void(int64_t, int64_t, int)> fn, const CancelToken* cancel)
	{
		m_fn = &fn;
		m_cancel = cancel;
		m_count = count;
		m_grain = grain;
		m_chunks = (count + grain - 1) / grain;
		m_next.store(0, std::memory_order_relaxed);
		m_failed.store(false, std::memory_order_relaxed);
		m_busy.store((int)m_workers.size(), std::memory_order_relaxed);
		m_epoch.fetch_add(1, std::memory_order_release);
		m_epoch.notify_all();
		Work(0);
		for (int busy; (busy = m_busy.load(std::memory_order_acquire)) != 0;)
			if (!Spin([&] { return m_busy.load(std::memory_order_acquire) == 0; }))
				m_busy.wait(busy, std::memory_order_acquire);
		if (m_failed.load(std::memory_order_relaxed))
			throw std::runtime_error("parallel loop failed");
	}

	int Parallel::MaxWorkers() const
	{
		return std::max(pool ? pool->Size() : 0, std::max(threads, 1));
	}

	void Parallel::For(int64_t count, int64_t grain, FnRef<void(int64_t, int64_t, int)> fn) const
	{
		if (count <= 0)
			return;
		grain = std::max<int64_t>(grain, 1);
		const int64_t chunks = (count + grain - 1) / grain;
		if (chunks == 1 || (threads <= 1 && !cancel))
			return fn(0, count, 0);
		if (threads <= 1)
		{
			for (int64_t c = 0; c < chunks && !Cancelled(); ++c)
				fn(c * grain, std::min(count, (c + 1) * grain), 0);
			return;
		}
		if (pool && poolMutex)
		{
			// one loop at a time on the pool; a requester that finds it busy spawns its own threads below
			std::unique_lock lock(*poolMutex, std::try_to_lock);
			if (lock.owns_lock())
				return pool->Run(count, grain, fn, cancel);
		}
		std::mutex m;
		bool failed = false;
		const int n = (int)std::min<int64_t>(threads, chunks);
		int64_t next = 0;
		auto work = [&](int worker)
		{
			try
			{
				for (;;)
				{
					int64_t c;
					{
						std::lock_guard l(m);
						c = next++;
					}
					if (c >= chunks || Cancelled())
						return;
					fn(c * grain, std::min(count, (c + 1) * grain), worker);
				}
			}
			catch (...)
			{
				std::lock_guard l(m);
				failed = true, next = chunks;
			}
		};
		std::vector<std::thread> spawned;
		spawned.reserve((size_t)n - 1);
		for (int t = 1; t < n; ++t)
			spawned.emplace_back(work, t);
		work(0);
		for (std::thread& t : spawned)
			t.join();
		if (failed)
			throw std::runtime_error("parallel loop failed");
	}
}
