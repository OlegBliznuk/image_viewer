#include "display.h"

#include <algorithm>
#include <cmath>

#include "display_vs.h"
#include "display_ps.h"
#include "mips_cs.h"
#include "unpack_cs.h"

namespace iv
{
	namespace
	{
		struct Constants
		{
			float ndcMin[2], ndcMax[2], imgOrigin[2], imgSize[2], texSize[2];
			float exposure;
			uint32_t flags;
			float mask[4];
			uint32_t maxLevel;
			float pad[3];   // the matrix rows start on a 16-byte boundary, as in the cbuffer
			float m[3][4];
		};
		static_assert(sizeof(Constants) == 32 * 4);

		struct MipConstants
		{
			uint32_t srcSize[2], dstSize[2];
			uint32_t srgb;
		};

		constexpr D3D12_RESOURCE_STATES kShaderRead = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

		D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource* res, UINT subresource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = res;
			b.Transition.Subresource = subresource;
			b.Transition.StateBefore = before;
			b.Transition.StateAfter = after;
			return b;
		}

		bool CreateRootSignature(Gpu& gpu, const D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>& out)
		{
			ComPtr<ID3DBlob> blob, msg;
			return SUCCEEDED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &msg)) && SUCCEEDED(gpu.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&out)));
		}

		UINT MipSize(int size, int level) { return (UINT)std::max(size >> level, 1); }

		D3D12_RESOURCE_DESC TextureDesc(const Layout& layout, int levels, bool uav)
		{
			D3D12_RESOURCE_DESC rd = {};
			rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			rd.Width = (UINT64)layout.width;
			rd.Height = (UINT)layout.height;
			rd.DepthOrArraySize = 1;
			rd.MipLevels = (UINT16)levels;
			rd.Format = layout.format;
			rd.SampleDesc.Count = 1;
			rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
			return rd;
		}

		bool CreateTexture(Gpu& gpu, const Layout& layout, ComPtr<ID3D12Resource>& out)
		{
			const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
			const D3D12_RESOURCE_DESC rd = TextureDesc(layout, 1, false);
			return SUCCEEDED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, kShaderRead, nullptr, IID_PPV_ARGS(&out)));
		}

		// a buffer pitched as layout into mip 0
		void CopyToTexture(ID3D12GraphicsCommandList* cmd, ID3D12Resource* tex, ID3D12Resource* src, const Layout& layout)
		{
			D3D12_TEXTURE_COPY_LOCATION dst = {};
			dst.pResource = tex;
			dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION from = {};
			from.pResource = src;
			from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			from.PlacedFootprint.Footprint = { layout.format, (UINT)layout.width, (UINT)layout.height, 1, layout.rowPitch };
			D3D12_RESOURCE_BARRIER b = Barrier(tex, 0, kShaderRead, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->ResourceBarrier(1, &b);
			cmd->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
			b = Barrier(tex, 0, D3D12_RESOURCE_STATE_COPY_DEST, kShaderRead);
			cmd->ResourceBarrier(1, &b);
		}
	}

	bool Display::Create(Gpu& gpu, std::string& err, Disposer* disposer)
	{
		m_disposer = disposer;
		D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER params[2] = {};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[1].DescriptorTable.NumDescriptorRanges = 1;
		params[1].DescriptorTable.pDescriptorRanges = &range;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		if (!CreateRootSignature(gpu, { 2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE }, m_rootSig))
		{
			err = "root signature creation failed";
			return false;
		}

		D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
		pso.pRootSignature = m_rootSig.Get();
		pso.VS = { g_display_vs, sizeof(g_display_vs) };
		pso.PS = { g_display_ps, sizeof(g_display_ps) };
		pso.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		pso.SampleMask = UINT_MAX;
		pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		pso.RasterizerState.DepthClipEnable = TRUE;
		pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		pso.NumRenderTargets = 1;
		pso.RTVFormats[0] = kBackBufferFormat;
		pso.SampleDesc.Count = 1;
		if (FAILED(gpu.device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&m_pso))))
		{
			err = "pipeline creation failed";
			return false;
		}

		D3D12_DESCRIPTOR_RANGE srcRange = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 };
		D3D12_DESCRIPTOR_RANGE dstRange = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0 };
		D3D12_ROOT_PARAMETER mipParams[3] = {};
		mipParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		mipParams[0].Constants.Num32BitValues = sizeof(MipConstants) / 4;
		mipParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		mipParams[1].DescriptorTable = { 1, &srcRange };
		mipParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		mipParams[2].DescriptorTable = { 1, &dstRange };
		D3D12_COMPUTE_PIPELINE_STATE_DESC mipPso = {};
		if (!CreateRootSignature(gpu, { 3, mipParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE }, m_mipRootSig))
		{
			err = "mip root signature creation failed";
			return false;
		}
		mipPso.pRootSignature = m_mipRootSig.Get();
		mipPso.CS = { g_mips_cs, sizeof(g_mips_cs) };
		if (FAILED(gpu.device->CreateComputePipelineState(&mipPso, IID_PPV_ARGS(&m_mipPso))))
		{
			err = "mip pipeline creation failed";
			return false;
		}

		// RGBE expansion: constants, the frame cache buffer as a raw root SRV, mip 0 as UAV
		D3D12_ROOT_PARAMETER unpackParams[3] = {};
		unpackParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		unpackParams[0].Constants.Num32BitValues = 4;
		unpackParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		unpackParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		unpackParams[2].DescriptorTable = { 1, &dstRange };
		D3D12_COMPUTE_PIPELINE_STATE_DESC unpackPso = {};
		if (!CreateRootSignature(gpu, { 3, unpackParams, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE }, m_unpackRootSig))
		{
			err = "unpack root signature creation failed";
			return false;
		}
		unpackPso.pRootSignature = m_unpackRootSig.Get();
		unpackPso.CS = { g_unpack_cs, sizeof(g_unpack_cs) };
		if (FAILED(gpu.device->CreateComputePipelineState(&unpackPso, IID_PPV_ARGS(&m_unpackPso))))
		{
			err = "unpack pipeline creation failed";
			return false;
		}
		IV_MARK(gpu.timing, "display root signatures + PSOs");
		return true;
	}

	void Display::FreeTex(Gpu& gpu, Tex& t)
	{
		if (t.res)
		{
			gpu.srvAlloc.Free(t.srv.cpu, t.srv.gpu);
			for (View& v : t.mipSrv)
				gpu.srvAlloc.Free(v.cpu, v.gpu);
			for (View& v : t.mipUav)
				gpu.srvAlloc.Free(v.cpu, v.gpu);
			if (t.uav0.cpu.ptr)
				gpu.srvAlloc.Free(t.uav0.cpu, t.uav0.gpu);
		}
		if (t.heap)
		{
			m_heaps.push_back(std::move(t.heap));
			std::sort(m_heaps.begin(), m_heaps.end(), [](const ComPtr<ID3D12Heap>& a, const ComPtr<ID3D12Heap>& b) { return a->GetDesc().SizeInBytes > b->GetDesc().SizeInBytes; });
			// the rest go on the Disposer thread: freeing a heap of video memory takes milliseconds
			while (m_heaps.size() > 4)
			{
				if (m_disposer)
					m_disposer->Post([dead = std::move(m_heaps.back())]() mutable { dead.Reset(); });
				m_heaps.pop_back();
			}
		}
		t = Tex();
	}

	void Display::ReleaseTextures(Gpu& gpu)
	{
		for (Tex& t : m_ring)
			FreeTex(gpu, t);
		m_current = -1;
	}

	void Display::Retire(Gpu& gpu, uint64_t completed)
	{
		if (m_held.tex.res && !m_held.visible && m_held.tex.lastUse <= completed)
			FreeTex(gpu, m_held.tex);
	}

	void Display::Destroy(Gpu& gpu)
	{
		ReleaseTextures(gpu);
		FreeTex(gpu, m_held.tex);
		FreeTex(gpu, m_ahead);
		m_held.visible = false;
		for (Overlay& o : m_overlays)
			FreeTex(gpu, o.tex), o = Overlay();
		m_overlay = -1;
		m_heaps.clear();
		m_pso.Reset();
		m_rootSig.Reset();
		m_mipPso.Reset();
		m_mipRootSig.Reset();
		m_unpackPso.Reset();
		m_unpackRootSig.Reset();
		m_staging.Reset();
	}

	void Display::SetLayout(Gpu& gpu, const Layout& layout)
	{
		// same layout: the previous image stays on screen until the next upload replaces it
		if (layout == m_layout && m_ring[0].res)
			return;
		// callers wait for the GPU to go idle first, so the textures can go at once
		if (m_current >= 0)
		{
			FreeTex(gpu, m_held.tex);
			m_held.tex = std::move(m_ring[m_current]);
			m_held.layout = m_layout;
			m_held.params = m_lastParams;
			m_held.visible = true;
			m_ring[m_current] = Tex();
		}
		ReleaseTextures(gpu);
		m_layout = layout;
		m_levels = LevelsFor(gpu, layout);
	}

	// a full mip chain where the format takes typed UAV stores (the mips are built on the GPU), mip 0 alone otherwise
	int Display::LevelsFor(Gpu& gpu, const Layout& layout) const
	{
		if (layout.width <= 0 || layout.height <= 0)
			return 1;
		D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { layout.format };
		const bool uavStore = SUCCEEDED(gpu.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))) && (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) && (fs.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
		return uavStore ? 1 + (int)std::floor(std::log2((double)std::max(layout.width, layout.height))) : 1;
	}

	bool Display::CreateRingTexture(Gpu& gpu, Tex& t, const Layout& layout, int levels)
	{
		if (layout.width <= 0 || layout.height <= 0)
			return false;
		// placed in the smallest pooled heap that fits, so a new size costs no allocation once a large enough heap exists
		const D3D12_RESOURCE_DESC rd = TextureDesc(layout, levels, levels > 1 || layout.packing != Packing::None);
		const D3D12_RESOURCE_ALLOCATION_INFO ai = gpu.device->GetResourceAllocationInfo(0, 1, &rd);
		auto fit = m_heaps.end();
		for (auto it = m_heaps.begin(); it != m_heaps.end(); ++it)
			if ((*it)->GetDesc().SizeInBytes >= ai.SizeInBytes && (fit == m_heaps.end() || (*it)->GetDesc().SizeInBytes < (*fit)->GetDesc().SizeInBytes))
				fit = it;
		if (fit != m_heaps.end())
			t.heap = std::move(*fit), m_heaps.erase(fit);
		else
		{
			// every texel is written before it is read, so the zero fill is skipped where the runtime knows the flag
			D3D12_HEAP_DESC hd = { ai.SizeInBytes, { D3D12_HEAP_TYPE_DEFAULT }, ai.Alignment, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES | D3D12_HEAP_FLAG_CREATE_NOT_ZEROED };
			if (FAILED(gpu.device->CreateHeap(&hd, IID_PPV_ARGS(&t.heap))))
			{
				hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES;
				if (FAILED(gpu.device->CreateHeap(&hd, IID_PPV_ARGS(&t.heap))))
					return false;
			}
		}
		if (FAILED(gpu.device->CreatePlacedResource(t.heap.Get(), 0, &rd, kShaderRead, nullptr, IID_PPV_ARGS(&t.res))))
			return FreeTex(gpu, t), false;
		t.levels = levels;
		gpu.srvAlloc.Alloc(&t.srv.cpu, &t.srv.gpu);
		gpu.device->CreateShaderResourceView(t.res.Get(), nullptr, t.srv.cpu);
		t.mipSrv.resize(levels - 1);
		t.mipUav.resize(levels - 1);
		for (int n = 1; n < levels; ++n)
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
			sd.Format = layout.format;
			sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			sd.Texture2D.MostDetailedMip = n - 1;
			sd.Texture2D.MipLevels = 1;
			View& s = t.mipSrv[n - 1];
			gpu.srvAlloc.Alloc(&s.cpu, &s.gpu);
			gpu.device->CreateShaderResourceView(t.res.Get(), &sd, s.cpu);
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = layout.format;
			ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			ud.Texture2D.MipSlice = n;
			View& u = t.mipUav[n - 1];
			gpu.srvAlloc.Alloc(&u.cpu, &u.gpu);
			gpu.device->CreateUnorderedAccessView(t.res.Get(), nullptr, &ud, u.cpu);
		}
		if (layout.packing != Packing::None)
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
			ud.Format = layout.format;
			ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			gpu.srvAlloc.Alloc(&t.uav0.cpu, &t.uav0.gpu);
			gpu.device->CreateUnorderedAccessView(t.res.Get(), nullptr, &ud, t.uav0.cpu);
		}
		return true;
	}

	// mip 0 of t from a frame-cache buffer laid out as layout
	bool Display::Fill(Gpu& gpu, ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, ID3D12Resource* src, uint64_t fence)
	{
		if (layout.packing != Packing::None)
		{
			// only the packed bytes cross the bus, in one copy to video memory; the shader expands them there into the texels
			// slack as the cache buffers have: the unpack reads whole words past the last pixel
			if (!m_staging || m_staging->GetDesc().Width < layout.size + 64)
			{
				m_staging.Reset();
				const D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT };
				D3D12_RESOURCE_DESC bd = {};
				bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				bd.Width = layout.size + 64;
				bd.Height = 1, bd.DepthOrArraySize = 1, bd.MipLevels = 1, bd.SampleDesc.Count = 1, bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_staging))) && FAILED(gpu.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_staging))))
					return false;
				m_stagingFence = 0;
			}
			// buffers decay to COMMON when their command list completes and promote from it to the copy; a second upload in the same list turns it back first
			D3D12_RESOURCE_BARRIER b = Barrier(m_staging.Get(), 0, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			if (m_stagingFence == fence)
				cmd->ResourceBarrier(1, &b);
			cmd->CopyBufferRegion(m_staging.Get(), 0, src, 0, layout.size);
			const D3D12_RESOURCE_BARRIER bs[2] = { Barrier(m_staging.Get(), 0, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), Barrier(t.res.Get(), 0, kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
			cmd->ResourceBarrier(2, bs);
			m_stagingFence = fence;
			const bool yuv = layout.packing == Packing::Yuv;
			const uint32_t mode = yuv ? 5 : layout.packing == Packing::Rgbe ? 0 : layout.format == DXGI_FORMAT_R8G8B8A8_UNORM ? 1 : layout.format == DXGI_FORMAT_R16G16B16A16_UNORM ? 2 : layout.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 3 : 4;
			// YCbCr planes: threads cover the stored (unoriented) planes, loads stay within the frame (sizes are whole words)
			const bool turned = yuv && layout.yuv.orientation >= 5;
			const uint32_t w = (uint32_t)(turned ? layout.height : layout.width), h = (uint32_t)(turned ? layout.width : layout.height);
			const uint32_t k[4] = { w, h, yuv ? (uint32_t)std::min<uint64_t>(layout.size - 1, UINT32_MAX) : layout.rowPitch, mode };
			cmd->SetComputeRootSignature(m_unpackRootSig.Get());
			cmd->SetPipelineState(m_unpackPso.Get());
			cmd->SetComputeRoot32BitConstants(0, 4, k, 0);
			cmd->SetComputeRootShaderResourceView(1, m_staging->GetGPUVirtualAddress());
			cmd->SetComputeRootDescriptorTable(2, t.uav0.gpu);
			cmd->Dispatch((w + 63) / 64, h, 1);
			b = Barrier(t.res.Get(), 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
			cmd->ResourceBarrier(1, &b);
		}
		else
			CopyToTexture(cmd, t.res.Get(), src, layout);
		t.built = 1;
		t.lastUse = fence;
		return true;
	}

	bool Display::Upload(Gpu& gpu, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, uint64_t fence, uint64_t completed)
	{
		int pick = -1;
		for (int i = 0; i < 3 && pick < 0; ++i)
			if (i != m_current && m_ring[i].res && m_ring[i].lastUse <= completed)
				pick = i;
		// a still image needs one texture, playback grows the ring to three
		for (int i = 0; i < 3 && pick < 0; ++i)
			if (!m_ring[i].res && CreateRingTexture(gpu, m_ring[i], m_layout, m_levels))
				pick = i;
		if (pick < 0 || !Fill(gpu, cmd, m_ring[pick], m_layout, src, fence))
			return false;
		m_current = pick;
		m_held.visible = false;
		return true;
	}

	bool Display::UploadAhead(Gpu& gpu, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, const Layout& layout, bool srgb, uint64_t fence, uint64_t completed)
	{
		if (m_ahead.res && m_ahead.lastUse > completed)
			return false;
		const int levels = LevelsFor(gpu, layout);
		if (m_ahead.res && (!(m_aheadLayout == layout) || m_ahead.levels != levels))
			FreeTex(gpu, m_ahead);
		if (!m_ahead.res && !CreateRingTexture(gpu, m_ahead, layout, levels))
			return false;
		m_aheadLayout = layout;
		if (!Fill(gpu, cmd, m_ahead, layout, src, fence))
			return false;
		BuildMips(cmd, m_ahead, layout, levels - 1, srgb);
		return true;
	}

	bool Display::AdoptAhead(Gpu& gpu, const Layout& layout)
	{
		if (!m_ahead.res || !(m_aheadLayout == layout) || !(m_layout == layout) || m_ahead.levels != m_levels)
			return false;
		// callers wait for the GPU to go idle first; the texture it replaces becomes the next spare
		int slot = m_current >= 0 ? m_current : 0;
		std::swap(m_ring[slot], m_ahead);
		if (!m_ahead.res)
			m_aheadLayout = Layout();
		m_current = slot;
		m_held.visible = false;
		return true;
	}
	void Display::BuildMips(ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, int last, bool srgb)
	{
		cmd->SetComputeRootSignature(m_mipRootSig.Get());
		cmd->SetPipelineState(m_mipPso.Get());
		for (int n = t.built; n <= last; ++n)
		{
			D3D12_RESOURCE_BARRIER b = Barrier(t.res.Get(), n, kShaderRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			cmd->ResourceBarrier(1, &b);
			const MipConstants k = { { MipSize(layout.width, n - 1), MipSize(layout.height, n - 1) }, { MipSize(layout.width, n), MipSize(layout.height, n) }, srgb ? 1u : 0u };
			cmd->SetComputeRoot32BitConstants(0, sizeof(k) / 4, &k, 0);
			cmd->SetComputeRootDescriptorTable(1, t.mipSrv[n - 1].gpu);
			cmd->SetComputeRootDescriptorTable(2, t.mipUav[n - 1].gpu);
			cmd->Dispatch((k.dstSize[0] + 7) / 8, (k.dstSize[1] + 7) / 8, 1);
			b = Barrier(t.res.Get(), n, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kShaderRead);
			cmd->ResourceBarrier(1, &b);
		}
		t.built = last + 1;
	}

	int Display::FreeOverlay(uint64_t completed) const
	{
		for (int i = 0; i < 2; ++i)
			if (i != m_overlay && m_overlays[i].tex.lastUse <= completed)
				return i;
		return -1;
	}

	void Display::SetOverlay(Gpu& gpu, ID3D12GraphicsCommandList* cmd, const uint8_t* rgba, int width, int height, float x0, float y0, float zoom, uint64_t fence, uint64_t completed)
	{
		const int pick = FreeOverlay(completed);
		if (pick < 0)
			return;
		Overlay& o = m_overlays[pick];
		const Layout l = PitchedLayout(width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 4);
		if (!o.tex.res || !(l == o.layout))
		{
			FreeTex(gpu, o.tex);
			o.upload.Reset();
			o.mapped = nullptr;
			D3D12_HEAP_PROPERTIES up = { D3D12_HEAP_TYPE_UPLOAD };
			D3D12_RESOURCE_DESC bd = {};
			bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bd.Width = l.size;
			bd.Height = 1;
			bd.DepthOrArraySize = 1;
			bd.MipLevels = 1;
			bd.SampleDesc.Count = 1;
			bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (!CreateTexture(gpu, l, o.tex.res) || FAILED(gpu.device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&o.upload))) || FAILED(o.upload->Map(0, nullptr, (void**)&o.mapped)))
			{
				o.tex.res.Reset();
				o.upload.Reset();
				o.mapped = nullptr;
				return;
			}
			gpu.srvAlloc.Alloc(&o.tex.srv.cpu, &o.tex.srv.gpu);
			gpu.device->CreateShaderResourceView(o.tex.res.Get(), nullptr, o.tex.srv.cpu);
			o.layout = l;
		}
		for (int y = 0; y < height; ++y)
			memcpy(o.mapped + (size_t)y * l.rowPitch, rgba + (size_t)y * width * 4, (size_t)width * 4);
		CopyToTexture(cmd, o.tex.res.Get(), o.upload.Get(), l);
		o.tex.lastUse = fence;
		o.x0 = x0, o.y0 = y0, o.zoom = zoom;
		m_overlay = pick;
	}

	void Display::Draw(ID3D12GraphicsCommandList* cmd, const DrawParams& p, uint64_t fence, UINT rtWidth, UINT rtHeight)
	{
		if (m_current >= 0)
		{
			m_lastParams = p;
			DrawTex(cmd, m_ring[m_current], m_layout, p, fence, rtWidth, rtHeight);
			if (m_overlay >= 0)
			{
				// the overlay keeps its placement in image space, so a stale one follows pan and zoom until the next render arrives
				const Overlay& o = m_overlays[m_overlay];
				DrawParams q = p;
				q.zoom = p.zoom / o.zoom;
				q.imgOrigin[0] = p.imgOrigin[0] + o.x0 * p.zoom;
				q.imgOrigin[1] = p.imgOrigin[1] + o.y0 * p.zoom;
				DrawTex(cmd, m_overlays[m_overlay].tex, o.layout, q, fence, rtWidth, rtHeight);
			}
		}
		else if (m_held.visible)
			DrawTex(cmd, m_held.tex, m_held.layout, m_held.params, fence, rtWidth, rtHeight);
	}

	void Display::DrawTex(ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, const DrawParams& p, uint64_t fence, UINT rtWidth, UINT rtHeight)
	{
		if (rtWidth == 0 || rtHeight == 0)
			return;
		const float imgW = layout.width * p.zoom, imgH = layout.height * p.zoom;
		const float x0 = std::max(p.vpMin[0], p.imgOrigin[0]), x1 = std::min(p.vpMax[0], p.imgOrigin[0] + imgW);
		const float y0 = std::max(p.vpMin[1], p.imgOrigin[1]), y1 = std::min(p.vpMax[1], p.imgOrigin[1] + imgH);
		if (x1 <= x0 || y1 <= y0)
			return;
		const int level = p.zoom < 1.0f ? std::min((int)std::log2(layout.width / imgW), t.levels - 1) : 0;
		if (level >= t.built)
			BuildMips(cmd, t, layout, level, p.srgbIn);
		Constants k = {};
		k.ndcMin[0] = x0 / rtWidth * 2.0f - 1.0f;
		k.ndcMin[1] = 1.0f - y0 / rtHeight * 2.0f;
		k.ndcMax[0] = x1 / rtWidth * 2.0f - 1.0f;
		k.ndcMax[1] = 1.0f - y1 / rtHeight * 2.0f;
		k.imgOrigin[0] = p.imgOrigin[0];
		k.imgOrigin[1] = p.imgOrigin[1];
		k.imgSize[0] = imgW;
		k.imgSize[1] = imgH;
		k.texSize[0] = (float)layout.width;
		k.texSize[1] = (float)layout.height;
		k.exposure = std::exp2(p.exposure);
		k.flags = (p.srgbIn ? 1u : 0u) | (p.view == ViewTransform::Aces ? 2u : 0u) | (p.gray ? 8u : 0u) | (p.view == ViewTransform::Linear ? 16u : 0u) | (p.colorMatrix ? 32u : 0u);
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				k.m[r][c] = p.matrix[r * 3 + c];
		std::copy(p.mask, p.mask + 4, k.mask);
		k.maxLevel = (uint32_t)(t.built - 1);

		const D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)rtWidth, (float)rtHeight, 0.0f, 1.0f };
		const D3D12_RECT scissor = { (LONG)std::floor(x0), (LONG)std::floor(y0), (LONG)std::ceil(x1), (LONG)std::ceil(y1) };
		cmd->RSSetViewports(1, &vp);
		cmd->RSSetScissorRects(1, &scissor);
		cmd->SetGraphicsRootSignature(m_rootSig.Get());
		cmd->SetPipelineState(m_pso.Get());
		cmd->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &k, 0);
		cmd->SetGraphicsRootDescriptorTable(1, t.srv.gpu);
		cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		cmd->DrawInstanced(4, 1, 0, 0);
		t.lastUse = fence;
	}
}
