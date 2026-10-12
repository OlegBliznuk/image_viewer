#include "disposer.h"

#include <utility>

namespace iv
{
	Disposer::~Disposer()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void Disposer::Post(std::move_only_function<void()> fn)
	{
		{
			std::lock_guard lock(m_mutex);
			m_queue.push_back(std::move(fn));
			if (!m_thread.joinable())
				m_thread = std::thread(&Disposer::Loop, this);
		}
		m_cv.notify_all();
	}

	void Disposer::Drain()
	{
		std::unique_lock lock(m_mutex);
		m_cv.wait(lock, [&] { return m_queue.empty() && !m_running; });
	}

	void Disposer::Loop()
	{
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_cv.wait(lock, [&] { return m_stop || !m_queue.empty(); });
			if (m_queue.empty())
				return;
			std::vector<std::move_only_function<void()>> batch = std::move(m_queue);
			m_queue.clear();
			m_running = true;
			lock.unlock();
			for (auto& fn : batch)
				fn();
			batch.clear();
			lock.lock();
			m_running = false;
			m_cv.notify_all();
		}
	}
}
