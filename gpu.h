#pragma once

#include "startup_timing.h"

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace iv
{
	using Microsoft::WRL::ComPtr;

	constexpr int kFramesInFlight = 2;
	constexpr int kBackBuffers = 3;
	constexpr int kSrvHeapSize = 256;
	constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

	class SrvAllocator
	{
	public:
		void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap);
		void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu);
		void Free(D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu);
	private:
		D3D12_CPU_DESCRIPTOR_HANDLE m_cpu0{};
		D3D12_GPU_DESCRIPTOR_HANDLE m_gpu0{};
		UINT m_inc = 0;
		std::vector<int> m_free;
	};

	class Gpu
	{
	public:
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12DescriptorHeap> srvHeap;
		SrvAllocator srvAlloc;
#if defined(IV_STARTUP_TIMING)
		StartupTiming* timing = nullptr;
#endif

		bool CreateDevice(bool debugLayer, HANDLE deviceReady);
		bool CreateSwapChain(HWND hwnd);
		void Destroy();
		void Resize(UINT width, UINT height);
		void WaitIdle();
		ID3D12GraphicsCommandList* BeginFrame(const float clearColor[4]);
		void EndFrame(bool vsync);
		bool IsOccluded();
		void FlushDebugMessages();
		uint64_t RecordingFence() const { return m_fenceLast + 1; }
		uint64_t CompletedFence() const { return m_fence->GetCompletedValue(); }
		UINT Width() const { return m_width; }
		UINT Height() const { return m_height; }
	private:
		struct FrameContext
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			uint64_t fence = 0;
		};

		void CreateRenderTargets();

		ComPtr<ID3D12GraphicsCommandList> m_cmd;
		ComPtr<IDXGIFactory2> m_factory;
		ComPtr<IDXGISwapChain3> m_swapChain;
		ComPtr<IDCompositionDevice> m_dcomp;
		ComPtr<IDCompositionTarget> m_dcompTarget;
		ComPtr<IDCompositionVisual> m_dcompVisual;
		ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
		ComPtr<ID3D12Resource> m_backBuffers[kBackBuffers];
		D3D12_CPU_DESCRIPTOR_HANDLE m_rtv[kBackBuffers]{};
		FrameContext m_frames[kFramesInFlight];
		FrameContext* m_current = nullptr;
		UINT m_backBuffer = 0;
		ComPtr<ID3D12Fence> m_fence;
		ComPtr<ID3D12InfoQueue> m_infoQueue;
		HANDLE m_fenceEvent = nullptr;
		HANDLE m_waitable = nullptr;
		uint64_t m_fenceLast = 0;
		uint64_t m_frameIndex = 0;
		UINT m_width = 0;
		UINT m_height = 0;
		bool m_occluded = false;
	};
}
