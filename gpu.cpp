#include "gpu.h"

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace iv
{
	void SrvAllocator::Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
	{
		const D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
		m_cpu0 = heap->GetCPUDescriptorHandleForHeapStart();
		m_gpu0 = heap->GetGPUDescriptorHandleForHeapStart();
		m_inc = device->GetDescriptorHandleIncrementSize(desc.Type);
		m_free.clear();
		for (int i = (int)desc.NumDescriptors; i > 0; --i)
			m_free.push_back(i - 1);
	}

	void SrvAllocator::Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
	{
		const int idx = m_free.back();
		m_free.pop_back();
		cpu->ptr = m_cpu0.ptr + (SIZE_T)idx * m_inc;
		gpu->ptr = m_gpu0.ptr + (UINT64)idx * m_inc;
	}

	void SrvAllocator::Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
	{
		m_free.push_back((int)((cpu.ptr - m_cpu0.ptr) / m_inc));
	}

	bool Gpu::CreateDevice(bool debugLayer, HANDLE deviceReady)
	{
		if (debugLayer)
		{
			ComPtr<ID3D12Debug> dbg;
			if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
				dbg->EnableDebugLayer();
		}
		if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
			return false;
		IV_MARK(timing, "D3D12CreateDevice");
		if (debugLayer && SUCCEEDED(device.As(&m_infoQueue)))
			printf("[d3d12] debug layer on\n");

		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBackBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
		D3D12_DESCRIPTOR_HEAP_DESC srvDesc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
		if (FAILED(device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap))) || FAILED(device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&srvHeap))))
			return false;
		srvAlloc.Create(device.Get(), srvHeap.Get());
		const UINT rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		for (int i = 0; i < kBackBuffers; ++i)
			m_rtv[i].ptr = m_rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)i * rtvInc;
		SetEvent(deviceReady);
		IV_MARK(timing, "descriptor heaps");

		D3D12_COMMAND_QUEUE_DESC queueDesc = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
		if (FAILED(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue))))
			return false;
		IV_MARK(timing, "command queue");
		for (auto& fc : m_frames)
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fc.allocator))))
				return false;
		IV_MARK(timing, "command allocators");
		if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&m_cmd))) || FAILED(m_cmd->Close()))
			return false;
		IV_MARK(timing, "command list");
		if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence))))
			return false;
		m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		IV_MARK(timing, "fence");

		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory))))
			return false;
		return true;
	}

	// premultiplied composition swap chain: DWM always composes it; an HWND swap chain gets independent flip, and under windowed G-SYNC the desktop then refreshes only when the viewer presents
	bool Gpu::CreateSwapChain(HWND hwnd)
	{
		RECT rc = {};
		GetClientRect(hwnd, &rc);
		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width = (UINT)std::max<LONG>(rc.right - rc.left, 8);
		sd.Height = (UINT)std::max<LONG>(rc.bottom - rc.top, 8);
		sd.BufferCount = kBackBuffers;
		sd.Format = kBackBufferFormat;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
		ComPtr<IDXGISwapChain1> sc1;
		if (FAILED(m_factory->CreateSwapChainForComposition(queue.Get(), &sd, nullptr, &sc1)) || FAILED(sc1.As(&m_swapChain)))
			return false;
		m_swapChain->SetMaximumFrameLatency(kFramesInFlight);
		m_waitable = m_swapChain->GetFrameLatencyWaitableObject();
		IV_MARK(timing, "swap chain");
		if (FAILED(DCompositionCreateDevice(nullptr, IID_PPV_ARGS(&m_dcomp))) || FAILED(m_dcomp->CreateTargetForHwnd(hwnd, TRUE, &m_dcompTarget)) || FAILED(m_dcomp->CreateVisual(&m_dcompVisual)))
			return false;
		m_dcompVisual->SetContent(m_swapChain.Get());
		m_dcompTarget->SetRoot(m_dcompVisual.Get());
		if (FAILED(m_dcomp->Commit()))
			return false;
		IV_MARK(timing, "DirectComposition visual");
		CreateRenderTargets();
		IV_MARK(timing, "back buffer RTVs");
		return true;
	}

	void Gpu::Destroy()
	{
		if (queue)
			WaitIdle();
		for (auto& b : m_backBuffers)
			b.Reset();
		m_dcompVisual.Reset();
		m_dcompTarget.Reset();
		m_dcomp.Reset();
		m_swapChain.Reset();
		m_factory.Reset();
		if (m_waitable)
			CloseHandle(m_waitable);
		if (m_fenceEvent)
			CloseHandle(m_fenceEvent);
		m_waitable = m_fenceEvent = nullptr;
		for (auto& fc : m_frames)
			fc.allocator.Reset();
		m_cmd.Reset();
		m_fence.Reset();
		m_rtvHeap.Reset();
		srvHeap.Reset();
		queue.Reset();
		m_infoQueue.Reset();
		device.Reset();
	}

	void Gpu::CreateRenderTargets()
	{
		for (int i = 0; i < kBackBuffers; ++i)
		{
			m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
			device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, m_rtv[i]);
		}
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		m_swapChain->GetDesc1(&desc);
		m_width = desc.Width;
		m_height = desc.Height;
	}

	void Gpu::Resize(UINT width, UINT height)
	{
		if (!m_swapChain || width == 0 || height == 0 || (width == m_width && height == m_height))
			return;
		WaitIdle();
		for (auto& b : m_backBuffers)
			b.Reset();
		DXGI_SWAP_CHAIN_DESC1 desc = {};
		m_swapChain->GetDesc1(&desc);
		m_swapChain->ResizeBuffers(0, width, height, desc.Format, desc.Flags);
		CreateRenderTargets();
	}

	void Gpu::WaitIdle()
	{
		if (!queue || !m_fence)
			return;
		queue->Signal(m_fence.Get(), ++m_fenceLast);
		m_fence->SetEventOnCompletion(m_fenceLast, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, INFINITE);
	}

	ID3D12GraphicsCommandList* Gpu::BeginFrame(const float clearColor[4])
	{
		FrameContext& fc = m_frames[m_frameIndex % kFramesInFlight];
		if (m_fence->GetCompletedValue() < fc.fence)
		{
			m_fence->SetEventOnCompletion(fc.fence, m_fenceEvent);
			HANDLE handles[] = { m_waitable, m_fenceEvent };
			WaitForMultipleObjects(2, handles, TRUE, INFINITE);
		}
		else
			WaitForSingleObject(m_waitable, INFINITE);
		m_current = &fc;
		fc.allocator->Reset();
		m_cmd->Reset(fc.allocator.Get(), nullptr);

		m_backBuffer = m_swapChain->GetCurrentBackBufferIndex();
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = m_backBuffers[m_backBuffer].Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		m_cmd->ResourceBarrier(1, &barrier);
		m_cmd->ClearRenderTargetView(m_rtv[m_backBuffer], clearColor, 0, nullptr);
		m_cmd->OMSetRenderTargets(1, &m_rtv[m_backBuffer], FALSE, nullptr);
		ID3D12DescriptorHeap* heaps[] = { srvHeap.Get() };
		m_cmd->SetDescriptorHeaps(1, heaps);
		return m_cmd.Get();
	}

	void Gpu::EndFrame(bool vsync)
	{
		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = m_backBuffers[m_backBuffer].Get();
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		m_cmd->ResourceBarrier(1, &barrier);
		m_cmd->Close();
		ID3D12CommandList* lists[] = { m_cmd.Get() };
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(m_fence.Get(), ++m_fenceLast);
		m_current->fence = m_fenceLast;
		const HRESULT hr = m_swapChain->Present(vsync ? 1 : 0, 0);
		m_occluded = hr == DXGI_STATUS_OCCLUDED;
		m_frameIndex++;
	}

	bool Gpu::IsOccluded()
	{
		if (m_occluded && m_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
			return true;
		m_occluded = false;
		return false;
	}

	void Gpu::FlushDebugMessages()
	{
		if (!m_infoQueue)
			return;
		const UINT64 n = m_infoQueue->GetNumStoredMessages();
		for (UINT64 i = 0; i < n; ++i)
		{
			SIZE_T len = 0;
			m_infoQueue->GetMessage(i, nullptr, &len);
			std::vector<char> buf(len);
			auto* msg = (D3D12_MESSAGE*)buf.data();
			if (SUCCEEDED(m_infoQueue->GetMessage(i, msg, &len)))
				printf("[d3d12] %s\n", msg->pDescription);
		}
		m_infoQueue->ClearStoredMessages();
		if (n)
			fflush(stdout);
	}
}
