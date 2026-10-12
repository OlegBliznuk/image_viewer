#pragma once

#include "display.h"
#include "gpu.h"
#include "image_io.h"
#include "metadata.h"
#include "prefetch.h"
#include "sequence.h"
#include "svg_view.h"
#include "streamer.h"

#include "imgui.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iv
{
	// startup: StartGpuInit, Open, Init, PlaceWindow, show the window (GDI preview), InitDeferred, FinishGpuInit, Run
	class App
	{
	public:
		App();
		~App();
		void StartGpuInit(bool debugLayer);
		void Init(HWND hwnd);
		void InitDeferred();
		bool FinishGpuInit();
		void Shutdown();
		bool Open(const std::wstring& path, bool browsing = false);
		void PlaceWindow(bool fitImage);
		int Run();
		bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result);
		bool ImGuiInputEnabled() const { return m_gpuReady; }
#if defined(IV_STARTUP_TIMING)
		void Mark(const char* label) { m_timing.Mark(label); }
		void SetTimingQuit() { m_timingQuit = true; }
#else
		void Mark(const char*) {}
		void SetTimingQuit() {}
#endif
	private:
		void GpuInitThread(bool debugLayer);
		void OnDeviceReady();
		void ActivateFiles(const uint8_t* seed, CacheBuffer* adopt = nullptr);
		void JoinFirstFrame();
		void UpdateTitle();
		void RefreshTitle();
		float EstimatedProgress(double elapsed, uintmax_t bytes, FileKind kind) const;
		bool LoadingVisible() const;
		void TrackLoading();
		void BuildPreview();
		void PaintPreview();
		void Frame();
		void MenuBar();
		void Toolbar();
		void Viewport();
		void Timeline();
		void StatusBar();
		void MetadataWindow();
		void Hotkeys();
		void ApplyView(int view);
		void SetMip(int level);
		void Advance(double now);
		void Step(int delta);
		int BrowseTarget(int delta, std::filesystem::path& folder);
		void Browse(int delta);
		void PrefetchNext();
		void UploadAhead(ID3D12GraphicsCommandList* cmd, uint64_t fence, uint64_t completed);
		void UpdateSvgView(ID3D12GraphicsCommandList* cmd, uint64_t fence, uint64_t completed);
		int WheelNotches();
		void SetFrame(int index);
		void TogglePlay();
		void ToggleFullscreen();
		double FramePeriod(int index) const;
		void OpenDialog();
		void SaveDialog();
		void FitView();
		void ActualSize();
		void ApplyPendingView();
		void ZoomAround(ImVec2 screen, float zoom, ImVec2 anchorOrigin, float anchorZoom);
		bool BeginBar(const char* id, float y, float height);
		float RowHeight() const;
		float MenuHeight() const;
		bool IsSequence() const { return m_files.size() > 1; }
		int NextIndex() const { return (m_cur + 1) % (int)m_files.size(); }
		DrawParams MakeDrawParams() const;
		Parallel Par(int threads);

		// one parallel loop at a time on the pool (it takes a single launcher); declared first so it outlives every thread that uses it
		std::unique_ptr<ThreadPool> m_pool;
		std::mutex m_poolMutex;
		// big releases off the UI thread; outlives the streamer and the display that post to it
		Disposer m_disposer;
		HWND m_hwnd = nullptr;
		Gpu m_gpu;
		Display m_display;
		Streamer m_streamer;
		uint64_t m_cacheBudget = 0;

		std::thread m_gpuThread;
		HANDLE m_deviceReady = nullptr;
		HANDLE m_queueReady = nullptr;
		HANDLE m_imguiReady = nullptr;
		bool m_gpuOk = false;
		bool m_dx12Backend = false;
		std::string m_gpuError;
		bool m_gpuReady = false;
		bool m_deviceWorkDone = false;
		bool m_workersStarted = false;
		bool m_inFrame = false;
		bool m_loading = false;
		double m_loadStart = 0.0;
		// last open: from Open to the GPU finishing the frame that shows it (probe, allocation, decode, upload and the rest)
		double m_openStart = 0.0;
		bool m_openPending = false;
		uint64_t m_openFence = 0;
		double m_lastOpenMs = -1.0;
		int m_loadFrame = -1;
		uintmax_t m_loadBytes = 0;
		std::wstring m_baseTitle;
		std::wstring m_titleShown;
		// typical decode speed per FileKind (bytes of file per second) for the estimated progress, refined by every load that takes long enough
		double m_bytesPerSecond[(int)FileKind::Count] = { 150e6, 60e6, 60e6, 400e6, 400e6, 60e6, 150e6, 20e6, 30e6, 5e6, 50e6, 40e6, 200e6 };
		SvgView m_svgView;
		SvgView::Request m_svgRequested;
		std::wstring m_overlayPath;
		bool m_svgPending = false;
		bool m_svgFonts = false;

		std::thread m_decodeThread;
		bool m_imageIoReady = false;
		PageMemory m_firstFrame;
		std::unique_ptr<uint32_t[]> m_preview;
		RECT m_previewRect = {};
		SIZE m_previewClient = {};
		float m_estMenuH = 0.0f;
		float m_estRowH = 0.0f;
		COLORREF m_colMenu = 0;
		COLORREF m_colBar = 0;
		COLORREF m_colViewport = 0;

		ImageInfo m_info;
		std::vector<std::wstring> m_files;
		std::vector<int> m_frameNumbers;
		int m_view = 0;
		Layout m_layout;
		int m_cur = 0;
		int m_shown = -1;

		bool m_playing = false;
		bool m_realtime = false;
		int m_fps = 24;
		bool m_fileTiming = false;
		double m_nextDue = 0.0;
		std::deque<double> m_advances;
#if defined(GC_RELEASENOOPTS_BUILD) || defined(_DEBUG)
		double m_lastPrint = 0.0;
#endif

		float m_zoom = 1.0f;
		ImVec2 m_origin = ImVec2(0, 0);
		ImVec2 m_vpMin = ImVec2(0, 0);
		ImVec2 m_vpSize = ImVec2(0, 0);
		bool m_viewPending = false;
		ImVec2 m_rmbAnchor = ImVec2(0, 0);
		float m_wheelAcc = 0.0f;
		FolderScan m_scan;
		bool m_showMeta = false;
		std::wstring m_metaPath;   // the file m_meta was read from
		std::vector<MetaEntry> m_meta;
		int m_browseDir = 1;
		bool m_sequences = true;
		Prefetcher m_prefetch;
		std::wstring m_aheadPath;   // the prefetched item Display holds a texture of
		std::wstring m_browsePending;   // a browse item skipped while it was loading: the next wheel step counts from it
		bool m_openAbandoned = false;
		ImVec2 m_rmbOrigin = ImVec2(0, 0);
		float m_rmbZoom = 1.0f;
		double m_mmbDownTime = 0.0;
		bool m_fullscreen = false;
		bool m_fullscreenToggle = false;
		bool m_fitPending = false;
		bool m_restoreView = false;
		float m_savedZoom = 1.0f;
		ImVec2 m_savedOrigin = ImVec2(0, 0);
		WINDOWPLACEMENT m_savedPlacement = { sizeof(WINDOWPLACEMENT) };

		float m_exposure = 0.0f;
		ViewTransform m_viewTransform = ViewTransform::Srgb;
		std::string m_error;
		std::string m_notice;   // the last save, until the next open
		std::vector<uint8_t> m_cacheStates;
		int m_quietFrames = 0;
#if defined(IV_STARTUP_TIMING)
		StartupTiming m_timing;
		int m_timedFrames = 0;
		bool m_timingQuit = false;
		bool m_timingDone = false;
#endif
	};
}
