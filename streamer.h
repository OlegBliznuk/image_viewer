#pragma once

#include "disposer.h"
#include "gpu.h"
#include "image_io.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iv
{
	class CacheHeaps;

	// page-aligned memory a CacheBuffer can take over as an imported heap (the startup frame, decoded before the device exists)
	struct PageFree
	{
		void operator()(uint8_t* p) const { VirtualFree(p, 0, MEM_RELEASE); }
	};
	using PageMemory = std::unique_ptr<uint8_t, PageFree>;
	inline PageMemory AllocatePages(size_t bytes) { return PageMemory((uint8_t*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)); }

	// a CPU-visible buffer the GPU copies a decoded frame from, placed in a CacheHeaps heap (releasing it gives the range back) or in imported memory
	struct CacheBuffer
	{
		ComPtr<ID3D12Resource> res;
		uint8_t* cpu = nullptr;
		uint64_t capacity = 0;

		CacheBuffer() = default;
		CacheBuffer(CacheBuffer&& o) noexcept { *this = std::move(o); }
		CacheBuffer& operator=(CacheBuffer&& o) noexcept;
		~CacheBuffer() { Release(); }
		void Release();
	private:
		friend class CacheHeaps;
		CacheHeaps* m_owner = nullptr;
		size_t m_heap = 0;
		uint64_t m_offset = 0;
		uint64_t m_bytes = 0;
		ComPtr<ID3D12Heap> m_imported;   // unpinning takes ~40 us per MB: released on the Disposer thread with its memory
		PageMemory m_memory;
		Disposer* m_disposer = nullptr;
	};

	// large CPU-visible heaps the frame buffers are placed in: a buffer costs no page zeroing once a heap has room and releasing one costs nothing
	// (committed buffers: 6.3 ms per 66 MB to create, 170 ms to free 60); heaps left empty beyond the budget are released on the Disposer thread
	class CacheHeaps
	{
	public:
		void Init(ID3D12Device* device, bool writeBack, Disposer* disposer);
		void SetBudget(uint64_t bytes);
		// empty heaps beyond keep bytes in all go (a cached sequence left for single images gives its memory back)
		void Trim(uint64_t keep);
		// thread-safe; out is released first
		std::string Allocate(CacheBuffer& out, uint64_t size);
		// memory (size + 64 bytes, already written) pinned as the buffer's heap, owned by out on success: ~37 us per MB, no page zeroing or copy
		std::string Import(CacheBuffer& out, PageMemory& memory, uint64_t size);
	private:
		friend struct CacheBuffer;
		struct Heap
		{
			ComPtr<ID3D12Heap> heap;
			uint64_t size = 0;
			std::vector<std::pair<uint64_t, uint64_t>> free;   // offset, bytes; sorted and coalesced
		};

		void Free(size_t heap, uint64_t offset, uint64_t bytes);
		void TrimLocked(uint64_t incoming, uint64_t limit);

		ID3D12Device* m_device = nullptr;
		bool m_writeBack = true;
		Disposer* m_disposer = nullptr;
		uint64_t m_budget = 0;
		std::mutex m_mutex;
		std::condition_variable m_created;
		int m_creating = 0;   // heaps being created; at most two, so a cache filling on 22 workers cannot create 22 at once
		std::vector<Heap> m_heaps;   // released heaps leave empty entries, so live buffers keep their index
	};

	// decoded-frame cache in CacheBuffers; workers decode ahead of the playhead; buffers outgrown or given back wait in a spare pool
	class Streamer
	{
	public:
		enum : uint8_t { kNone, kDecoding, kReady, kFailed };

		void Init(ID3D12Device* device, Disposer* disposer);
		// thread-safe: out keeps its buffer when it holds size bytes, else trades it for the smallest spare that does or a new one
		std::string TakeBuffer(CacheBuffer& out, uint64_t size);
		// thread-safe: into the spare pool, which keeps the two largest
		void GiveBuffer(CacheBuffer& buffer);
		std::string Import(CacheBuffer& out, PageMemory& memory, uint64_t size) { return m_heaps.Import(out, memory, size); }
		void StartWorkers();
		void Stop();
		// multiFrame: files repeats one multi-frame file, entry i is its frame i; the playhead frame comes copied from seed or as the decoded adopt buffer, taken over
		void Reset(const std::vector<std::wstring>& files, FileKind kind, const ViewDesc& view, const Layout& layout, uint64_t budget, int playhead, bool multiFrame, const Parallel& par, const uint8_t* seed = nullptr, CacheBuffer* adopt = nullptr);
		void SetPlayhead(int frame);
		void SetCompletedFence(uint64_t value);
		ID3D12Resource* AcquireForGpu(int frame, uint64_t fence);
		uint8_t State(int frame);
		int ReadPixel(int frame, int x, int y, float out[4]);
		// a copy of a decoded frame, laid out as the current layout; false when it is not in the cache
		bool CopyFrame(int frame, std::vector<uint8_t>& out);
		void States(std::vector<uint8_t>& out);
		std::string Error(int frame);
		HANDLE ReadyEvent() const { return m_readyEvent; }
#if defined(IV_STARTUP_TIMING)
		StartupTiming* timing = nullptr;
#endif
	private:
		struct Slot
		{
			CacheBuffer buf;
			int frame = -1;
			bool busy = false;
			uint64_t gpuFence = 0;
			CancelToken cancel;   // the job decoding into it
		};

		void WorkerLoop();
		bool PickJob(int& frame, int& slot);
		int Distance(int frame) const;

		ID3D12Device* m_device = nullptr;
		CacheHeaps m_heaps;
		std::vector<std::thread> m_workers;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		bool m_stop = false;
		bool m_writeBack = true;
		uint64_t m_generation = 0;
		std::vector<std::wstring> m_files;
		FileKind m_kind = FileKind::Exr;
		bool m_multiFrame = false;
		FrameSet m_frameSet;
		ViewDesc m_view;
		Layout m_layout;
		Parallel m_par;
		std::vector<std::unique_ptr<Slot>> m_slots;
		std::vector<std::unique_ptr<Slot>> m_retiring;   // slots Reset took away from jobs still decoding (cancelled); each job frees its own when it ends
		size_t m_maxSlots = 1;
		std::vector<uint8_t> m_state;
		std::vector<int> m_frameSlot;
		std::vector<std::string> m_errors;
		int m_playhead = 0;
		int m_pinned = -1;
		uint64_t m_completed = 0;
		HANDLE m_readyEvent = nullptr;
		std::mutex m_poolMutex;
		std::vector<CacheBuffer> m_pool;
#if defined(IV_STARTUP_TIMING)
		bool m_timedFirstJob = false;
#endif
	};
}
