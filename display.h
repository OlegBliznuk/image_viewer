#pragma once

#include "disposer.h"
#include "gpu.h"
#include "image_io.h"

#include <string>
#include <vector>

namespace iv
{
	// view transform from scene-linear to the screen
	enum class ViewTransform { Linear, Srgb, Aces };

	struct DrawParams
	{
		float vpMin[2];
		float vpMax[2];
		float imgOrigin[2];
		float zoom = 1.0f;
		float exposure = 0.0f;
		float mask[4] = { 0, 0, 0, 0 };
		bool gray = false;
		bool srgbIn = false;
		ViewTransform view = ViewTransform::Srgb;
		bool colorMatrix = false;   // matrix takes linear RGB to Rec.709 primaries (row-major)
		float matrix[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
	};

	// the current frame from a ring of up to 3 textures created on demand (uploads skip textures in flight), Mitchell mips built on the GPU as far as the zoom needs, the old image held across a layout change
	class Display
	{
	public:
		bool Create(Gpu& gpu, std::string& err, Disposer* disposer);
		void Destroy(Gpu& gpu);
		void SetLayout(Gpu& gpu, const Layout& layout);
		bool Upload(Gpu& gpu, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, uint64_t fence, uint64_t completed);
		// the image the wheel opens next into a spare texture with its whole mip chain (false while the spare is in flight); AdoptAhead makes it current when that image opens with this layout
		bool UploadAhead(Gpu& gpu, ID3D12GraphicsCommandList* cmd, ID3D12Resource* src, const Layout& layout, bool srgb, uint64_t fence, uint64_t completed);
		bool AdoptAhead(Gpu& gpu, const Layout& layout);
		void Draw(ID3D12GraphicsCommandList* cmd, const DrawParams& p, uint64_t fence, UINT rtWidth, UINT rtHeight);
		// frees the held image once it is replaced and the GPU is done with it
		void Retire(Gpu& gpu, uint64_t completed);
		void Clear() { m_current = -1, m_held.visible = false; }
		// an RGBA8 raster drawn over the image (SVG at screen resolution) covering image space from (x0, y0) at zoom raster pixels per image pixel
		bool OverlaySlotFree(uint64_t completed) const { return FreeOverlay(completed) >= 0; }
		void SetOverlay(Gpu& gpu, ID3D12GraphicsCommandList* cmd, const uint8_t* rgba, int width, int height, float x0, float y0, float zoom, uint64_t fence, uint64_t completed);
		void ClearOverlay() { m_overlay = -1; }
	private:
		struct View
		{
			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
		};

		struct Tex
		{
			ComPtr<ID3D12Heap> heap;   // ring textures are placed in pooled heaps
			ComPtr<ID3D12Resource> res;
			View srv;
			std::vector<View> mipSrv;   // mip n - 1, read to build mip n
			std::vector<View> mipUav;   // mip n
			View uav0;   // mip 0, for frames expanded on the GPU (packed RGBE / RGB)
			int levels = 1;
			int built = 1;
			uint64_t lastUse = 0;
		};

		struct Held
		{
			Tex tex;
			Layout layout;
			DrawParams params;
			bool visible = false;
		};

		struct Overlay
		{
			Tex tex;
			ComPtr<ID3D12Resource> upload;
			uint8_t* mapped = nullptr;
			Layout layout;
			float x0 = 0.0f;
			float y0 = 0.0f;
			float zoom = 1.0f;
		};

		int FreeOverlay(uint64_t completed) const;
		int LevelsFor(Gpu& gpu, const Layout& layout) const;
		bool CreateRingTexture(Gpu& gpu, Tex& t, const Layout& layout, int levels);
		bool Fill(Gpu& gpu, ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, ID3D12Resource* src, uint64_t fence);
		void FreeTex(Gpu& gpu, Tex& t);
		void ReleaseTextures(Gpu& gpu);
		void BuildMips(ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, int last, bool srgb);
		void DrawTex(ID3D12GraphicsCommandList* cmd, Tex& t, const Layout& layout, const DrawParams& p, uint64_t fence, UINT rtWidth, UINT rtHeight);

		ComPtr<ID3D12RootSignature> m_rootSig;
		ComPtr<ID3D12PipelineState> m_pso;
		ComPtr<ID3D12RootSignature> m_mipRootSig;
		ComPtr<ID3D12PipelineState> m_mipPso;
		ComPtr<ID3D12RootSignature> m_unpackRootSig;
		ComPtr<ID3D12PipelineState> m_unpackPso;
		// packed frames are copied here before the unpack reads them: shader reads of the write-back cache buffers miss the GPU's L2 and cross the bus once per read
		ComPtr<ID3D12Resource> m_staging;
		uint64_t m_stagingFence = 0;   // the upload that last left it in the shader-read state within its command list
		Tex m_ring[3];
		Tex m_ahead;   // the next browse item, or a spare (stale contents) left by AdoptAhead
		Layout m_aheadLayout;
		int m_current = -1;
		Layout m_layout;
		int m_levels = 1;
		DrawParams m_lastParams;
		Held m_held;
		Overlay m_overlays[2];
		int m_overlay = -1;
		std::vector<ComPtr<ID3D12Heap>> m_heaps;   // free heaps, the largest four kept
		Disposer* m_disposer = nullptr;
	};
}
