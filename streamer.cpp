#include "streamer.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace iv
{
	namespace
	{
		constexpr uint64_t kPlacement = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
		constexpr uint64_t kChunk = 512ull << 20;

		D3D12_HEAP_PROPERTIES CacheHeapProperties(bool writeBack)
		{
			D3D12_HEAP_PROPERTIES hp = {};
			if (writeBack)
			{
				hp.Type = D3D12_HEAP_TYPE_CUSTOM;
				hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
				hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
			}
			else
				hp.Type = D3D12_HEAP_TYPE_UPLOAD;
			return hp;
		}
	}

	CacheBuffer& CacheBuffer::operator=(CacheBuffer&& o) noexcept
	{
		if (this != &o)
		{
			Release();
			res = std::move(o.res);
			cpu = std::exchange(o.cpu, nullptr);
			capacity = std::exchange(o.capacity, 0);
			m_owner = std::exchange(o.m_owner, nullptr);
			m_heap = o.m_heap;
			m_offset = o.m_offset;
			m_bytes = std::exchange(o.m_bytes, 0);
			m_imported = std::move(o.m_imported);
			m_memory = std::move(o.m_memory);
			m_disposer = std::exchange(o.m_disposer, nullptr);
		}
		return *this;
	}

	void CacheBuffer::Release()
	{
		res.Reset();
		if (m_owner)
			m_owner->Free(m_heap, m_offset, m_bytes);
		if (m_imported && m_disposer)
			m_disposer->Post([heap = std::move(m_imported), memory = std::move(m_memory)]() mutable { heap.Reset(); memory.reset(); });
		m_imported.Reset(), m_memory.reset();
		m_owner = nullptr, m_disposer = nullptr, cpu = nullptr, capacity = 0, m_bytes = 0;
	}

	void CacheHeaps::Init(ID3D12Device* device, bool writeBack, Disposer* disposer)
	{
		m_device = device;
		m_writeBack = writeBack;
		m_disposer = disposer;
	}

	void CacheHeaps::SetBudget(uint64_t bytes)
	{
		std::lock_guard lock(m_mutex);
		m_budget = bytes;
	}

	std::string CacheHeaps::Allocate(CacheBuffer& out, uint64_t size)
	{
		out.Release();
		// slack: the GPU expansion of packed pixels reads whole words past the last one
		const uint64_t width = size + 64, bytes = (width + kPlacement - 1) / kPlacement * kPlacement;
		std::unique_lock lock(m_mutex);
		size_t heap = SIZE_MAX, range = 0;
		uint64_t best = UINT64_MAX;
		const auto bestFit = [&]
		{
			for (size_t h = 0; h < m_heaps.size(); ++h)
				for (size_t r = 0; r < m_heaps[h].free.size(); ++r)
					if (m_heaps[h].free[r].second >= bytes && m_heaps[h].free[r].second < best)
						heap = h, range = r, best = m_heaps[h].free[r].second;
		};
		bestFit();
		while (heap == SIZE_MAX && m_creating >= 2)
		{
			m_created.wait(lock);
			best = UINT64_MAX;
			bestFit();
		}
		if (heap == SIZE_MAX)
		{
			// the first heap is the frame's own size (startup creates nothing larger), later ones take several frames
			const bool first = m_creating == 0 && std::none_of(m_heaps.begin(), m_heaps.end(), [](const Heap& h) { return h.heap != nullptr; });
			const uint64_t heapBytes = first ? bytes : std::max(bytes, kChunk);
			TrimLocked(heapBytes, m_budget);
			m_creating++;
			lock.unlock();
			const D3D12_HEAP_DESC hd = { heapBytes, CacheHeapProperties(m_writeBack), kPlacement, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS };
			ComPtr<ID3D12Heap> created;
			const bool ok = SUCCEEDED(m_device->CreateHeap(&hd, IID_PPV_ARGS(&created)));
			lock.lock();
			m_creating--;
			m_created.notify_all();
			if (!ok)
				return "out of memory for the frame cache";
			auto slot = std::find_if(m_heaps.begin(), m_heaps.end(), [](const Heap& h) { return h.heap == nullptr; });
			if (slot == m_heaps.end())
				slot = m_heaps.insert(m_heaps.end(), Heap());
			slot->heap = std::move(created);
			slot->size = heapBytes;
			slot->free = { { 0, heapBytes } };
			heap = (size_t)(slot - m_heaps.begin()), range = 0;
		}
		Heap& h = m_heaps[heap];
		const uint64_t offset = h.free[range].first;
		h.free[range].first += bytes, h.free[range].second -= bytes;
		if (!h.free[range].second)
			h.free.erase(h.free.begin() + range);
		ID3D12Heap* d3dHeap = h.heap.Get();
		lock.unlock();
		CacheBuffer buf;
		buf.m_owner = this, buf.m_heap = heap, buf.m_offset = offset, buf.m_bytes = bytes;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = width;
		rd.Height = 1, rd.DepthOrArraySize = 1, rd.MipLevels = 1, rd.SampleDesc.Count = 1, rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		const D3D12_RESOURCE_STATES state = m_writeBack ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_GENERIC_READ;
		if (FAILED(m_device->CreatePlacedResource(d3dHeap, offset, &rd, state, nullptr, IID_PPV_ARGS(&buf.res))))
			return "cannot place a frame cache buffer";
		const D3D12_RANGE none = { 0, 0 };
		void* p = nullptr;
		if (FAILED(buf.res->Map(0, &none, &p)))
			return "cannot map a frame cache buffer";
		buf.cpu = (uint8_t*)p;
		buf.capacity = size;
		out = std::move(buf);
		return {};
	}

	std::string CacheHeaps::Import(CacheBuffer& out, PageMemory& memory, uint64_t size)
	{
		out.Release();
		ComPtr<ID3D12Device3> device3;
		ComPtr<ID3D12Heap> heap;
		if (!memory || FAILED(m_device->QueryInterface(IID_PPV_ARGS(&device3))) || FAILED(device3->OpenExistingHeapFromAddress(memory.get(), IID_PPV_ARGS(&heap))))
			return "cannot import the frame";
		CacheBuffer buf;
		D3D12_RESOURCE_DESC rd = {};
		rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		rd.Width = size + 64;
		rd.Height = 1, rd.DepthOrArraySize = 1, rd.MipLevels = 1, rd.SampleDesc.Count = 1, rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
		if (FAILED(m_device->CreatePlacedResource(heap.Get(), 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buf.res))))
			return "cannot place the imported frame";
		buf.cpu = memory.get();
		buf.capacity = size;
		buf.m_imported = std::move(heap);
		buf.m_memory = std::move(memory);
		buf.m_disposer = m_disposer;
		out = std::move(buf);
		return {};
	}

	void CacheHeaps::Free(size_t heap, uint64_t offset, uint64_t bytes)
	{
		std::lock_guard lock(m_mutex);
		if (heap >= m_heaps.size() || !m_heaps[heap].heap)
			return;
		auto& free = m_heaps[heap].free;
		auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(offset, uint64_t(0)));
		it = free.insert(it, { offset, bytes });
		if (it + 1 != free.end() && it->first + it->second == (it + 1)->first)
			it->second += (it + 1)->second, free.erase(it + 1);
		if (it != free.begin() && (it - 1)->first + (it - 1)->second == it->first)
			(it - 1)->second += it->second, free.erase(it);
	}

	void CacheHeaps::Trim(uint64_t keep)
	{
		std::lock_guard lock(m_mutex);
		TrimLocked(0, keep);
	}

	// empty heaps go (on the Disposer thread) while the heaps plus the one about to be created exceed limit
	void CacheHeaps::TrimLocked(uint64_t incoming, uint64_t limit)
	{
		uint64_t total = incoming;
		for (const Heap& h : m_heaps)
			total += h.size;
		for (Heap& h : m_heaps)
		{
			if (total <= limit)
				break;
			if (!h.heap || h.free.size() != 1 || h.free[0].second != h.size)
				continue;
			total -= h.size;
			if (m_disposer)
				m_disposer->Post([dead = std::move(h.heap)]() mutable { dead.Reset(); });
			h = Heap();
		}
	}

	void Streamer::Init(ID3D12Device* device, Disposer* disposer)
	{
		m_device = device;
		m_readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		// write-back CPU pages where the device takes them (decoders read back what they wrote), else upload heaps
		const D3D12_HEAP_DESC probe = { kPlacement, CacheHeapProperties(true), kPlacement, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS };
		ComPtr<ID3D12Heap> heap;
		m_writeBack = SUCCEEDED(device->CreateHeap(&probe, IID_PPV_ARGS(&heap)));
		m_heaps.Init(device, m_writeBack, disposer);
		IV_MARK(timing, "streamer: custom-heap probe");
	}

	void Streamer::StartWorkers()
	{
		const int workers = std::max(2, (int)std::thread::hardware_concurrency() - 2);
		for (int i = 0; i < workers; ++i)
			m_workers.emplace_back([this] { WorkerLoop(); });
		IV_MARK(timing, "streamer: worker threads spawned");
	}

	void Streamer::Stop()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		for (auto& t : m_workers)
			t.join();
		m_workers.clear();
		m_slots.clear();
		m_retiring.clear();
		m_pool.clear();
		if (m_readyEvent)
			CloseHandle(m_readyEvent);
		m_readyEvent = nullptr;
	}

	std::string Streamer::TakeBuffer(CacheBuffer& out, uint64_t size)
	{
		if (out.res && out.capacity >= size)
			return {};
		GiveBuffer(out);
		{
			std::lock_guard lock(m_poolMutex);
			auto fit = m_pool.end();
			for (auto it = m_pool.begin(); it != m_pool.end(); ++it)
				if (it->capacity >= size && (fit == m_pool.end() || it->capacity < fit->capacity))
					fit = it;
			if (fit != m_pool.end())
			{
				out = std::move(*fit);
				m_pool.erase(fit);
				return {};
			}
		}
		return m_heaps.Allocate(out, size);
	}

	void Streamer::GiveBuffer(CacheBuffer& buffer)
	{
		if (buffer.res)
		{
			std::lock_guard lock(m_poolMutex);
			m_pool.push_back(std::exchange(buffer, {}));
			std::sort(m_pool.begin(), m_pool.end(), [](const CacheBuffer& a, const CacheBuffer& b) { return a.capacity > b.capacity; });
			if (m_pool.size() > 2)
				m_pool.resize(2);
		}
		buffer = {};
	}

	void Streamer::Reset(const std::vector<std::wstring>& files, FileKind kind, const ViewDesc& view, const Layout& layout, uint64_t budget, int playhead, int rangeFirst, int rangeLast, bool multiFrame, const Parallel& par, const uint8_t* seed, CacheBuffer* adopt)
	{
		std::unique_lock lock(m_mutex);
		m_generation++;
		m_files.clear();
		// jobs under way are cancelled and keep their slots until they end, so Open never waits for a decode nobody wants (PNG inflate, JPEG and WebP cannot stop early)
		for (auto& s : m_slots)
			if (s->busy)
				s->cancel.Cancel(), m_retiring.push_back(std::move(s));
		std::erase(m_slots, nullptr);
		for (auto& s : m_slots)
			s->frame = -1, s->gpuFence = 0;
		m_files = files;
		m_kind = kind;
		m_multiFrame = multiFrame;
		if (!multiFrame)
			m_frameSet.Clear();
		m_view = view;
		m_layout = layout;
		m_par = par;
		m_maxSlots = (size_t)std::clamp<uint64_t>(budget / std::max<uint64_t>(layout.size, 1), 1, std::max<size_t>(files.size(), 1));
		m_heaps.SetBudget(budget);
		if (m_slots.size() > m_maxSlots)
			m_slots.resize(m_maxSlots);
		m_heaps.Trim(std::min<uint64_t>(budget, (uint64_t)m_maxSlots * layout.size) + kChunk);
		m_state.assign(files.size(), kNone);
		m_frameSlot.assign(files.size(), -1);
		m_errors.assign(files.size(), {});
		m_playhead = files.empty() ? 0 : std::clamp(playhead, 0, (int)files.size() - 1);
		m_rangeFirst = files.empty() ? 0 : std::clamp(rangeFirst, 0, (int)files.size() - 1);
		m_rangeLast = files.empty() ? 0 : std::clamp(rangeLast, m_rangeFirst, (int)files.size() - 1);
		m_pinned = -1;
		if (adopt && adopt->res && adopt->capacity >= layout.size && !files.empty())
		{
			if (m_slots.empty())
				m_slots.push_back(std::make_unique<Slot>());
			std::swap(m_slots[0]->buf, *adopt);
			GiveBuffer(*adopt);
			m_slots[0]->frame = m_playhead;
			m_state[m_playhead] = kReady;
			m_frameSlot[m_playhead] = 0;
			SetEvent(m_readyEvent);
			lock.unlock();
			m_cv.notify_all();
			return;
		}
		Slot* seeded = nullptr;
		if (seed && !files.empty())
		{
			int pick = -1;
			for (int i = 0; i < (int)m_slots.size() && pick < 0; ++i)
				if (m_slots[i]->frame < 0)
					pick = i;
			if (pick < 0)
				m_slots.push_back(std::make_unique<Slot>()), pick = (int)m_slots.size() - 1;
			seeded = m_slots[pick].get();
			seeded->frame = m_playhead;
			seeded->busy = true;
			m_state[m_playhead] = kDecoding;
			m_frameSlot[m_playhead] = pick;
		}
		const uint64_t gen = m_generation;
		lock.unlock();
		m_cv.notify_all();
		if (!seeded)
			return;
		const std::string err = TakeBuffer(seeded->buf, layout.size);
		if (err.empty())
			par.Width((int)std::thread::hardware_concurrency()).For((int64_t)layout.size, 8 << 20, [&](int64_t a, int64_t b, int) { memcpy(seeded->buf.cpu + a, seed + a, (size_t)(b - a)); });
		lock.lock();
		seeded->busy = false;
		if (gen == m_generation)
		{
			m_state[seeded->frame] = err.empty() ? kReady : kNone;
			if (!err.empty())
				m_frameSlot[seeded->frame] = -1, seeded->frame = -1;
		}
		else
			seeded->frame = -1;
		SetEvent(m_readyEvent);
		lock.unlock();
		m_cv.notify_all();
	}

	int Streamer::Distance(int frame) const
	{
		const int n = (int)m_files.size(), len = m_rangeLast - m_rangeFirst + 1;
		if (frame == m_playhead)
			return 0;
		if (frame < m_rangeFirst || frame > m_rangeLast)
			return len + 1 + (frame - m_playhead + n) % n;
		return PlayheadInRange() ? (frame - m_playhead + len) % len : frame - m_rangeFirst + 1;
	}

	bool Streamer::PickJob(int& frame, int& slot)
	{
		if (m_files.empty())
			return false;
		const int len = m_rangeLast - m_rangeFirst + 1;
		const bool inRange = PlayheadInRange();
		const int window = (int)std::min<size_t>(inRange ? len : len + 1, m_maxSlots);
		for (int d = 0; d < window; ++d)
		{
			const int f = inRange ? m_rangeFirst + (m_playhead - m_rangeFirst + d) % len : d == 0 ? m_playhead : m_rangeFirst + d - 1;
			if (m_state[f] != kNone)
				continue;
			int pick = -1;
			for (int i = 0; i < (int)m_slots.size() && pick < 0; ++i)
				if (!m_slots[i]->busy && m_slots[i]->frame < 0)
					pick = i;
			if (pick < 0 && m_slots.size() < m_maxSlots)
			{
				m_slots.push_back(std::make_unique<Slot>());
				pick = (int)m_slots.size() - 1;
			}
			if (pick < 0)
			{
				int worst = d;
				for (int i = 0; i < (int)m_slots.size(); ++i)
				{
					const Slot& s = *m_slots[i];
					if (s.busy || s.frame == m_pinned || s.gpuFence > m_completed)
						continue;
					const int sd = Distance(s.frame);
					if (sd > worst)
						worst = sd, pick = i;
				}
			}
			if (pick < 0)
				return false;
			Slot& s = *m_slots[pick];
			if (s.frame >= 0)
				m_state[s.frame] = kNone, m_frameSlot[s.frame] = -1;
			s.frame = f;
			s.busy = true;
			m_state[f] = kDecoding;
			m_frameSlot[f] = pick;
			frame = f;
			slot = pick;
			return true;
		}
		return false;
	}

	void Streamer::WorkerLoop()
	{
		std::unique_lock lock(m_mutex);
		while (true)
		{
			int frame = -1, si = -1;
			m_cv.wait(lock, [&] { return m_stop || PickJob(frame, si); });
			if (m_stop)
				return;
			Slot& s = *m_slots[si];
			const uint64_t gen = m_generation;
			const std::wstring path = m_files[frame];
			const ViewDesc view = m_view;
			const Layout layout = m_layout;
			const FileKind kind = m_kind;
			const bool multiFrame = m_multiFrame;
			Parallel par = m_par.Width(m_files.size() == 1 || frame == m_playhead ? (int)std::thread::hardware_concurrency() : 0);
			s.cancel.Reset();
			par.cancel = &s.cancel;
#if defined(IV_STARTUP_TIMING)
			StartupTiming* timed = m_timedFirstJob ? nullptr : timing;
			m_timedFirstJob = true;
#endif
			lock.unlock();
			IV_MARK(timed, "streamer: first job picked");
			std::string err = TakeBuffer(s.buf, layout.size);
			IV_MARK(timed, "streamer: first slot buffer created");
			if (err.empty())
				err = DecodeImage(path, kind, view, layout, s.buf.cpu, par, frame, multiFrame ? &m_frameSet : nullptr);
			IV_MARK(timed, "streamer: first job decoded");
			lock.lock();
			s.busy = false;
				if (gen == m_generation)
			{
				m_state[frame] = err.empty() ? kReady : kFailed;
				m_errors[frame] = err;
				if (!err.empty())
					s.frame = -1, m_frameSlot[frame] = -1;
			}
			else
				s.frame = -1;
			std::erase_if(m_retiring, [&](const std::unique_ptr<Slot>& r) { return r.get() == &s; });
			SetEvent(m_readyEvent);
			m_cv.notify_all();
		}
	}

	void Streamer::SetPlayhead(int frame)
	{
		{
			std::lock_guard lock(m_mutex);
			if (frame == m_playhead || frame < 0 || frame >= (int)m_files.size())
				return;
			m_playhead = frame;
		}
		m_cv.notify_all();
	}

	void Streamer::SetRange(int first, int last)
	{
		{
			std::lock_guard lock(m_mutex);
			const int n = (int)m_files.size();
			if (n == 0)
				return;
			first = std::clamp(first, 0, n - 1);
			last = std::clamp(last, first, n - 1);
			if (first == m_rangeFirst && last == m_rangeLast)
				return;
			m_rangeFirst = first, m_rangeLast = last;
		}
		m_cv.notify_all();
	}

	void Streamer::SetCompletedFence(uint64_t value)
	{
		{
			std::lock_guard lock(m_mutex);
			if (value == m_completed)
				return;
			m_completed = value;
		}
		m_cv.notify_all();
	}

	ID3D12Resource* Streamer::AcquireForGpu(int frame, uint64_t fence)
	{
		std::lock_guard lock(m_mutex);
		if (frame < 0 || frame >= (int)m_state.size() || m_state[frame] != kReady)
			return nullptr;
		Slot& s = *m_slots[m_frameSlot[frame]];
		s.gpuFence = fence;
		m_pinned = frame;
		return s.buf.res.Get();
	}

	uint8_t Streamer::State(int frame)
	{
		std::lock_guard lock(m_mutex);
		return frame >= 0 && frame < (int)m_state.size() ? m_state[frame] : (uint8_t)kNone;
	}

	int Streamer::ReadPixel(int frame, int x, int y, float out[4])
	{
		std::lock_guard lock(m_mutex);
		if (frame < 0 || frame >= (int)m_state.size() || m_state[frame] != kReady)
			return 0;
		return iv::ReadPixel(m_layout, m_slots[m_frameSlot[frame]]->buf.cpu, x, y, out);
	}

	bool Streamer::CopyFrame(int frame, std::vector<uint8_t>& out, uint64_t generation)
	{
		std::lock_guard lock(m_mutex);
		if ((generation && generation != m_generation) || frame < 0 || frame >= (int)m_state.size() || m_state[frame] != kReady)
			return false;
		const uint8_t* src = m_slots[m_frameSlot[frame]]->buf.cpu;
		out.assign(src, src + m_layout.size);
		return true;
	}

	bool Streamer::WithFrame(int frame, FnRef<void(const uint8_t*)> fn)
	{
		std::lock_guard lock(m_mutex);
		if (frame < 0 || frame >= (int)m_state.size() || m_state[frame] != kReady)
			return false;
		fn(m_slots[m_frameSlot[frame]]->buf.cpu);
		return true;
	}

	uint64_t Streamer::Generation()
	{
		std::lock_guard lock(m_mutex);
		return m_generation;
	}

	void Streamer::States(std::vector<uint8_t>& out)
	{
		std::lock_guard lock(m_mutex);
		out = m_state;
	}

	std::string Streamer::Error(int frame)
	{
		std::lock_guard lock(m_mutex);
		return frame >= 0 && frame < (int)m_errors.size() ? m_errors[frame] : std::string();
	}
}
