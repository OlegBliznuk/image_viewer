#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace iv
{
	// a non-owning callable reference (no allocation)
	template <class Sig> class FnRef;
	template <class R, class... A> class FnRef<R(A...)>
	{
	public:
		template <class F, class = std::enable_if_t<!std::is_same_v<std::decay_t<F>, FnRef>>>
		FnRef(F&& f) : m_obj((void*)&f), m_call([](void* o, A... a) -> R { return (*(std::remove_reference_t<F>*)o)(std::forward<A>(a)...); }) {}
		R operator()(A... a) const { return m_call(m_obj, std::forward<A>(a)...); }
	private:
		void* m_obj;
		R (*m_call)(void*, A...);
	};

	// set once a decode's result is no longer wanted (the image was scrolled past); loops stop handing out chunks and DecodeImage reports it
	class CancelToken
	{
	public:
		void Cancel() { std::lock_guard lock(m_mutex); m_cancelled = true; }
		void Reset() { std::lock_guard lock(m_mutex); m_cancelled = false; }
		bool Cancelled() const { std::lock_guard lock(m_mutex); return m_cancelled; }
	private:
		mutable std::mutex m_mutex;
		bool m_cancelled = false;
	};

	// persistent workers running one parallel loop at a time with the caller as worker 0: chunks go out by an atomic counter,
	// a new loop is announced by an epoch counter; idle workers spin briefly for back-to-back loops, then sleep in an atomic wait
	class ThreadPool
	{
	public:
		explicit ThreadPool(int threads = 0);   // 0: one per hardware thread
		~ThreadPool();
		ThreadPool(const ThreadPool&) = delete;
		ThreadPool& operator=(const ThreadPool&) = delete;
		int Size() const { return (int)m_workers.size() + 1; }
		// fn(begin, end, worker) over [0, count) in chunks of grain on every thread; one caller at a time; returns after all chunks ran, or once cancelled after the chunks under way
		void Run(int64_t count, int64_t grain, FnRef<void(int64_t, int64_t, int)> fn, const CancelToken* cancel);
	private:
		void Loop(int worker);
		void Work(int worker);

		std::vector<std::thread> m_workers;
		const FnRef<void(int64_t, int64_t, int)>* m_fn = nullptr;   // the job, written before the epoch moves on
		const CancelToken* m_cancel = nullptr;
		int64_t m_count = 0;
		int64_t m_grain = 1;
		int64_t m_chunks = 0;
		bool m_stop = false;
		alignas(64) std::atomic<int64_t> m_next{ 0 };
		alignas(64) std::atomic<uint32_t> m_epoch{ 0 };
		alignas(64) std::atomic<int> m_busy{ 0 };   // workers still in the loop
		std::atomic<bool> m_failed{ false };
	};

	// where a decode's parallel loops run: the shared ThreadPool while it is free (dynamic chunks over all its threads), otherwise up to `threads` threads spawned for the loop taking chunks; threads <= 1 runs on the caller
	struct Parallel
	{
		ThreadPool* pool = nullptr;
		std::mutex* poolMutex = nullptr;
		int threads = 1;
		const CancelToken* cancel = nullptr;

		bool Cancelled() const { return cancel && cancel->Cancelled(); }
		Parallel Width(int n) const { Parallel p = *this; p.threads = n; return p; }
		// the worker indices For passes are below this
		int MaxWorkers() const;
		// fn(begin, end, worker) over [0, count) in chunks of at most grain items; returns after all chunks ran, or once cancelled after the chunks under way
		void For(int64_t count, int64_t grain, FnRef<void(int64_t, int64_t, int)> fn) const;
	};
}
