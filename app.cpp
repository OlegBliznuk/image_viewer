#include "app.h"
#include "image_formats.h"
#include "image_save.h"
#include "sequence.h"

#include "imgui_impl_dx12.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"
#include <nfd.h>

#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <numeric>

#pragma comment(lib, "imgui.lib")
#pragma comment(lib, "nfd.lib")

namespace iv
{
	namespace
	{
		constexpr float kMinZoom = 1.0f / 64.0f;
		constexpr float kMaxZoom = 256.0f;
		constexpr UINT_PTR kSizeMoveTimer = 1;
		constexpr ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;

		double Now()
		{
			return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}

		// "[||||||||............] 42% loading..."
		std::wstring ProgressText(float progress)
		{
			constexpr int kCells = 20;
			std::wstring bar(kCells, L'.');
			std::fill_n(bar.begin(), std::clamp((int)(progress * kCells + 0.5f), 0, kCells), L'|');
			return L"[" + bar + L"] " + std::to_wstring((int)(progress * 100.0f)) + L"% loading...";
		}

		// "16:9" when the reduced terms are small, else the nearest common ratio within 0.5% ("~2.39:1"), else the decimal ratio
		std::string AspectRatio(int w, int h)
		{
			if (w <= 0 || h <= 0)
				return {};
			const int g = std::gcd(w, h), a = w / g, b = h / g;
			if ((a == 8 && b == 5) || (a == 5 && b == 8))
				return a > b ? "16:10" : "10:16";
			if (a <= 32 && b <= 32)
				return std::to_string(a) + ":" + std::to_string(b);
			const bool portrait = h > w;
			const double r = portrait ? (double)h / w : (double)w / h;
			struct Ratio { double value; const char* name; const char* portraitName; };
			const Ratio common[] = { { 1.0, "1:1", "1:1" }, { 5.0 / 4.0, "5:4", "4:5" }, { 4.0 / 3.0, "4:3", "3:4" }, { 1.43, "1.43:1", "1:1.43" }, { 3.0 / 2.0, "3:2", "2:3" }, { 16.0 / 10.0, "16:10", "10:16" }, { 5.0 / 3.0, "5:3", "3:5" }, { 16.0 / 9.0, "16:9", "9:16" }, { 1.85, "1.85:1", "1:1.85" }, { 1.9, "1.90:1", "1:1.90" }, { 2.0, "2:1", "1:2" }, { 2.39, "2.39:1", "1:2.39" } };
			for (const Ratio& c : common)
				if (std::abs(r / c.value - 1.0) < 0.005)
					return std::string("~") + (portrait ? c.portraitName : c.name);
			char buf[32];
			snprintf(buf, sizeof(buf), portrait ? "1:%.2f" : "%.2f:1", r);
			return buf;
		}
	}

	App::App() = default;
	App::~App() = default;

	Parallel App::Par(int threads)
	{
		return { m_pool.get(), &m_poolMutex, threads };
	}

	void App::StartGpuInit(bool debugLayer)
	{
#if defined(IV_STARTUP_TIMING)
		m_gpu.timing = &m_timing;
		m_streamer.timing = &m_timing;
#endif
		m_deviceReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		m_queueReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		m_imguiReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		m_gpuThread = std::thread([this, debugLayer] { GpuInitThread(debugLayer); });
		MEMORYSTATUSEX mem = { sizeof(mem) };
		GlobalMemoryStatusEx(&mem);
		m_cacheBudget = mem.ullTotalPhys / 4;
	}

	void App::GpuInitThread(bool debugLayer)
	{
		m_gpuOk = m_gpu.CreateDevice(debugLayer, m_deviceReady);
		if (!m_gpuOk)
			m_gpuError = "Direct3D 12 initialisation failed.";
		else
		{
			SetEvent(m_queueReady);
			m_gpuOk = m_display.Create(m_gpu, m_gpuError, &m_disposer);
		}
		WaitForSingleObject(m_imguiReady, INFINITE);
		if (!m_gpuOk)
			return;
		ImGui_ImplDX12_InitInfo info;
		info.Device = m_gpu.device.Get();
		info.CommandQueue = m_gpu.queue.Get();
		info.NumFramesInFlight = kFramesInFlight;
		info.RTVFormat = kBackBufferFormat;
		info.DSVFormat = DXGI_FORMAT_UNKNOWN;
		info.UserData = &m_gpu.srvAlloc;
		info.SrvDescriptorHeap = m_gpu.srvHeap.Get();
		info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu) { ((SrvAllocator*)i->UserData)->Alloc(cpu, gpu); };
		info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo* i, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu) { ((SrvAllocator*)i->UserData)->Free(cpu, gpu); };
		ImGui_ImplDX12_Init(&info);
		m_dx12Backend = true;
		ImGui_ImplDX12_CreateDeviceObjects();
		IV_MARK(m_gpu.timing, "imgui dx12 backend + device objects (D3DCompile)");
	}

	void App::Init(HWND hwnd)
	{
		m_hwnd = hwnd;
		const float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		ImGui::StyleColorsDark();
		ImGuiStyle& style = ImGui::GetStyle();
		style.WindowRounding = 0.0f;
		style.WindowBorderSize = 0.0f;
		style.ScaleAllSizes(scale);
		style.FontScaleDpi = scale;

		// the bitmap font is 13 px; matches GetFrameHeight() once frames run
		m_estMenuH = 13.0f * style.FontScaleDpi + 2.0f * style.FramePadding.y;
		m_estRowH = m_estMenuH + 2.0f * style.WindowPadding.y;
		const float clear = 0.09f;
		const ImVec4 mb = style.Colors[ImGuiCol_MenuBarBg], wb = style.Colors[ImGuiCol_WindowBg];
		auto rgb = [](float r, float g, float b) { return RGB(std::lround(r * 255.0f), std::lround(g * 255.0f), std::lround(b * 255.0f)); };
		m_colMenu = rgb(mb.x, mb.y, mb.z);
		m_colBar = rgb(wb.x * wb.w + clear * (1.0f - wb.w), wb.y * wb.w + clear * (1.0f - wb.w), wb.z * wb.w + clear * (1.0f - wb.w));
		m_colViewport = rgb(clear, clear, clear);
		UpdateTitle();
		Mark("imgui context + style");
	}

	void App::InitDeferred()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->AddFontDefaultBitmap();
		ImFontConfig merge;
		merge.MergeMode = true;
		if (GetFileAttributesW(L"C:\\Windows\\Fonts\\segoeui.ttf") != INVALID_FILE_ATTRIBUTES)
			io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 0.0f, &merge);
		ImGui_ImplWin32_Init(m_hwnd);
		SetEvent(m_imguiReady);
		Mark("imgui fonts + win32 backend");
		NFD_Init();
		Mark("NFD_Init (COM apartment)");
	}

	void App::OnDeviceReady()
	{
		m_deviceWorkDone = true;
		m_streamer.Init(m_gpu.device.Get(), &m_disposer);
		if (!m_files.empty())
		{
			JoinFirstFrame();
			// the decoded pages become the cache buffer (pinned beside the driver's queue creation) instead of a fresh heap plus a copy: 8K image on screen 21-48 ms sooner
			CacheBuffer imported;
			if (m_firstFrame && m_streamer.Import(imported, m_firstFrame, m_layout.size).empty())
				ActivateFiles(nullptr, &imported);
			else
				ActivateFiles(m_firstFrame.get());
			Mark("device ready: display textures + first frame handed to the cache");
		}
	}

	bool App::FinishGpuInit()
	{
		const HANDLE thread = (HANDLE)m_gpuThread.native_handle();
		bool quit = false, swapDone = false, swapOk = false;
		while (true)
		{
			HANDLE handles[3];
			int kinds[3];
			DWORD count = 0;
			handles[count] = thread, kinds[count++] = 0;
			if (!m_deviceWorkDone)
				handles[count] = m_deviceReady, kinds[count++] = 1;
			if (!swapDone)
				handles[count] = m_queueReady, kinds[count++] = 2;
			const DWORD r = MsgWaitForMultipleObjects(count, handles, FALSE, INFINITE, QS_ALLINPUT);
			if (r < WAIT_OBJECT_0 + count)
			{
				const int kind = kinds[r - WAIT_OBJECT_0];
				if (kind == 0)
					break;
				if (kind == 1)
					OnDeviceReady();
				else
					swapDone = true, swapOk = m_gpu.CreateSwapChain(m_hwnd);
				continue;
			}
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
				{
					quit = true;
					continue;
				}
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}
		m_gpuThread.join();
		Mark("GPU init thread joined");
		if (!m_deviceWorkDone && m_gpu.device)
			OnDeviceReady();
		if (!swapDone && m_gpuOk)
			swapOk = m_gpu.CreateSwapChain(m_hwnd);
		if (quit)
			return false;
		if (!m_gpuOk || !swapOk)
		{
			MessageBoxA(m_hwnd, m_gpuError.empty() ? "Swap chain creation failed." : m_gpuError.c_str(), "image_viewer", MB_ICONERROR);
			return false;
		}
		m_gpuReady = true;
		m_disposer.Post([frame = std::move(m_firstFrame)]() mutable { frame.reset(); });
		m_preview.reset();
		return true;
	}

	void App::Shutdown()
	{
		JoinFirstFrame();
		if (m_gpuThread.joinable())
		{
			SetEvent(m_imguiReady);
			m_gpuThread.join();
		}
		m_gpu.WaitIdle();
		m_streamer.Stop();
		if (m_dx12Backend)
			ImGui_ImplDX12_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		m_display.Destroy(m_gpu);
		m_disposer.Drain();
		m_gpu.Destroy();
		NFD_Quit();
		CloseHandle(m_deviceReady);
		CloseHandle(m_queueReady);
		CloseHandle(m_imguiReady);
	}

	// browsing: opened by the wheel, so the view stays when the size matches, the Sequences choice stays within one format and the prefetched decode is used
	bool App::Open(const std::wstring& path, bool browsing)
	{
		JoinFirstFrame();
		if (!m_imageIoReady)
			InitImageIo(), m_imageIoReady = true;
		const double openStart = Now();
		Prefetcher::Result pre;
		const double waitStart = Now();
		FileKind waitKind = FileKind::Exr;
		KindFromExtension(path, waitKind);
		std::error_code sizeError;
		const uintmax_t waitBytes = std::filesystem::file_size(path, sizeError);
		// another wheel notch while the prefetch of this item is still decoding: it is skipped, the next step goes on from it
		MSG pending;
		const Prefetcher::Taken taken = !browsing ? Prefetcher::Taken::No : m_prefetch.Take(path, pre, [&]
		{
			if (Now() - waitStart > 0.15 && m_hwnd)
				SetWindowTextW(m_hwnd, (L"image_viewer - " + std::filesystem::path(path).filename().wstring() + L"   " + ProgressText(EstimatedProgress(Now() - waitStart, waitBytes, waitKind))).c_str()), m_titleShown.clear();
			return !(m_hwnd && PeekMessageW(&pending, m_hwnd, WM_MOUSEWHEEL, WM_MOUSEWHEEL, PM_NOREMOVE));
		});
		m_openAbandoned = taken == Prefetcher::Taken::Abandoned;
		if (m_openAbandoned)
		{
			m_browsePending = path;
			m_titleShown.clear();
			return false;
		}
		const bool prefetched = taken == Prefetcher::Taken::Yes;
		ImageInfo info;
		const std::string err = prefetched ? std::string() : ProbeImage(path, info);
		Mark("open: probe header");
		if (!err.empty())
		{
			m_error = ToUtf8(std::filesystem::path(path).filename().wstring()) + ": " + err;
			return false;
		}
		if (prefetched)
			info = std::move(pre.info);
		if (info.kind == FileKind::Svg && !m_svgFonts)
			fmt::RegisterSvgFonts(), m_svgFonts = true;
		if (!browsing || info.kind != m_info.kind)
			m_sequences = SequencesByDefault(info.kind);
		std::vector<std::wstring> files = { path };
		std::vector<int> numbers = { 0 };
		int index = 0;
		if (info.frames > 1)
		{
			files.assign(info.frames, path);
			numbers.resize(info.frames);
			std::iota(numbers.begin(), numbers.end(), 1);
		}
		else if (m_sequences)
			m_scan.Detect(path, files, numbers, index);
		Mark("open: sequence detection (folder scan + parse)");

		if (m_gpuReady)
			m_gpu.WaitIdle();
		const bool sameSize = info.width == m_info.width && info.height == m_info.height;
		m_info = info;
		m_fileTiming = !m_info.delays.empty();
		m_files = files;
		m_frameNumbers = numbers;
		m_cur = index;
		m_shown = -1;
		m_playing = false;
		m_error.clear();
		m_notice.clear();
		m_view = m_info.defaultView;
		m_layout = MakeLayout(m_info, m_info.views[m_view]);
		if (m_deviceWorkDone)
		{
			if (!m_pool)
				m_pool = std::make_unique<ThreadPool>();
			const bool adopt = prefetched && pre.layout == m_layout;
			ActivateFiles(nullptr, adopt ? &pre.pixels : nullptr);
			m_streamer.GiveBuffer(pre.pixels);
			// the texture uploaded ahead shows it without another upload
			if (adopt && pre.path == m_aheadPath && m_display.AdoptAhead(m_gpu, m_layout))
				m_shown = m_cur;
			m_aheadPath.clear();
		}
		else
		{
			// startup: decode the first frame on all cores for the GDI preview while the GPU initialises
			m_firstFrame.reset();
			m_decodeThread = std::thread([this]
			{
				if (!m_pool)
					m_pool = std::make_unique<ThreadPool>();
				m_firstFrame = AllocatePages(m_layout.size + 64);
				if (m_firstFrame && !DecodeImage(m_files[m_cur], m_info.kind, m_info.views[m_view], m_layout, m_firstFrame.get(), Par((int)std::max(1u, std::thread::hardware_concurrency() / 2))).empty())
					m_firstFrame.reset();
				Mark("preview frame decoded");
			});
			m_previewClient = {};
			if (m_hwnd)
				InvalidateRect(m_hwnd, nullptr, FALSE);
		}
		m_browsePending.clear();
		m_viewPending = !(browsing && sameSize);
		ApplyPendingView();
		m_loadStart = 0.0;
		m_openStart = openStart, m_openPending = true, m_openFence = 0, m_lastOpenMs = -1.0;
		if (m_fileTiming)
		{
			m_playing = true;
			m_nextDue = Now() + FramePeriod(m_cur);
			m_advances.clear();
		}
		UpdateTitle();
		return true;
	}

	void App::JoinFirstFrame()
	{
		if (m_decodeThread.joinable())
			m_decodeThread.join();
	}

	void App::UpdateTitle()
	{
		if (!m_hwnd || m_files.empty())
			return;
		std::wstring title = L"image_viewer - " + std::filesystem::path(m_files[m_cur]).filename().wstring();
		if (IsSequence())
			title += L"  [" + std::to_wstring(m_frameNumbers.front()) + L"-" + std::to_wstring(m_frameNumbers.back()) + L", " + std::to_wstring(m_files.size()) + L" frames]";
		m_baseTitle = title;
		RefreshTitle();
	}

	// the title shows the estimated progress once a load has taken 150 ms
	void App::RefreshTitle()
	{
		if (!m_hwnd || m_baseTitle.empty())
			return;
		const std::wstring title = LoadingVisible() ? m_baseTitle + L"   " + ProgressText(EstimatedProgress(Now() - m_loadStart, m_loadBytes, m_info.kind)) : m_baseTitle;
		if (title != m_titleShown)
			SetWindowTextW(m_hwnd, title.c_str()), m_titleShown = title;
	}

	// a rough estimate, not measured: the decoders stay untouched; climbs to 90% at the expected time and never reaches 100% on its own
	float App::EstimatedProgress(double elapsed, uintmax_t bytes, FileKind kind) const
	{
		const double expected = std::max((double)bytes / m_bytesPerSecond[(int)kind], 0.05);
		return (float)std::min(0.99, 1.0 - std::exp(-2.3 * elapsed / expected));
	}

	bool App::LoadingVisible() const
	{
		return m_loadStart > 0.0 && Now() - m_loadStart > 0.15;
	}

	void App::ActivateFiles(const uint8_t* seed, CacheBuffer* adopt)
	{
		m_display.SetLayout(m_gpu, m_layout);
		m_streamer.Reset(m_files, m_info.kind, m_info.views[m_view], m_layout, m_cacheBudget, m_cur, m_info.frames > 1, Par(0), seed, adopt);
	}

	void App::ApplyView(int view)
	{
		if (view == m_view || view < 0 || view >= (int)m_info.views.size())
			return;
		m_view = view;
		if (m_info.kind != FileKind::Exr)
			return;
		m_gpu.WaitIdle();
		m_layout = MakeLayout(m_info, m_info.views[m_view]);
		ActivateFiles(nullptr);
		m_shown = -1;
	}

	// another level of a mip-mapped file, decoded on its own; the picture keeps its size on screen around the viewport centre
	void App::SetMip(int level)
	{
		const int current = m_info.views.empty() ? 0 : m_info.views[m_view].mip;
		if (level == current || level < 0 || level >= (int)m_info.mips.size())
			return;
		const ImVec2 centre = m_vpSize * 0.5f;
		const ImVec2 q = (centre - m_origin) / m_zoom;
		const float scale = (float)m_info.mips[level].first / (float)m_info.mips[current].first;
		m_zoom = std::clamp(m_zoom / scale, kMinZoom, kMaxZoom);
		m_origin = centre - q * scale * m_zoom;
		for (ViewDesc& v : m_info.views)
			v.mip = level;
		m_info.width = m_info.mips[level].first;
		m_info.height = m_info.mips[level].second;
		m_gpu.WaitIdle();
		m_layout = MakeLayout(m_info, m_info.views[m_view]);
		ActivateFiles(nullptr);
		m_shown = -1;
		m_loadStart = 0.0;
		m_openStart = Now(), m_openPending = true, m_openFence = 0, m_lastOpenMs = -1.0;
	}

	void App::PlaceWindow(bool fitImage)
	{
		const float chrome = m_estMenuH + m_estRowH * (IsSequence() ? 3.0f : 2.0f);
		POINT cursor = {};
		GetCursorPos(&cursor);
		const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
		MONITORINFO mi = { sizeof(mi) };
		GetMonitorInfoW(monitor, &mi);
		const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(monitor);
		RECT frame = { 0, 0, 0, 0 };
		AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, (UINT)std::lround(96.0f * scale));
		const int frameW = frame.right - frame.left, frameH = frame.bottom - frame.top;
		const int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
		const int maxW = workW - frameW, maxH = workH - frameH;
		int cw = (int)(1280 * scale), ch = (int)(800 * scale);
		if (fitImage && m_info.width > 0)
		{
			const float z = std::min({ 1.0f, (float)maxW / m_info.width, (maxH - chrome) / m_info.height });
			cw = std::max((int)std::ceil(m_info.width * z), (int)(640 * scale));
			ch = std::max((int)std::ceil(m_info.height * z + chrome), (int)(360 * scale));
		}
		const int w = std::min(cw, maxW) + frameW, h = std::min(ch, maxH) + frameH;
		SetWindowPos(m_hwnd, nullptr, mi.rcWork.left + (workW - w) / 2, mi.rcWork.top + (workH - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
		Mark("window placement");
	}

	void App::BuildPreview()
	{
		JoinFirstFrame();
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		m_previewClient = { rc.right, rc.bottom };
		// once the frame went to the cache the last preview stays
		if (!m_firstFrame)
			return;
		m_preview.reset();
		if (rc.right <= 0 || rc.bottom <= 0)
			return;
		const float top = m_estMenuH + m_estRowH, bottom = m_estRowH * (IsSequence() ? 2.0f : 1.0f);
		const float vw = (float)rc.right, vh = std::max(rc.bottom - top - bottom, 1.0f);
		const float iw = (float)m_layout.width, ih = (float)m_layout.height;
		const float zoom = iw <= vw && ih <= vh ? 1.0f : std::clamp(std::min(vw / iw, vh / ih), kMinZoom, kMaxZoom);
		const float ox = std::floor((vw - iw * zoom) * 0.5f), oy = top + std::floor((vh - ih * zoom) * 0.5f);
		const int x0 = std::max(0, (int)ox), y0 = std::max((int)top, (int)oy);
		const int x1 = std::min((int)rc.right, (int)std::ceil(ox + iw * zoom)), y1 = std::min((int)(top + vh), (int)std::ceil(oy + ih * zoom));
		if (x1 <= x0 || y1 <= y0)
			return;
		m_previewRect = { x0, y0, x1, y1 };
		m_preview = std::make_unique_for_overwrite<uint32_t[]>((size_t)(x1 - x0) * (y1 - y0));
		RenderPreview(m_layout, m_firstFrame.get(), zoom, ox, oy, x0, y0, x1, y1, m_preview.get(), Par((int)std::max(1u, std::thread::hardware_concurrency() / 2)));
		Mark("GDI preview rendered");
	}

	void App::PaintPreview()
	{
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(m_hwnd, &ps);
		RECT rc;
		GetClientRect(m_hwnd, &rc);
		if (m_previewClient.cx != rc.right || m_previewClient.cy != rc.bottom)
			BuildPreview();
		const int menuH = (int)m_estMenuH, top = (int)(m_estMenuH + m_estRowH), bottom = rc.bottom - (int)(m_estRowH * (IsSequence() ? 2.0f : 1.0f));
		const struct { RECT r; COLORREF c; } fills[] = { { { 0, 0, rc.right, menuH }, m_colMenu }, { { 0, menuH, rc.right, top }, m_colBar }, { { 0, top, rc.right, bottom }, m_colViewport }, { { 0, bottom, rc.right, rc.bottom }, m_colBar } };
		for (const auto& f : fills)
		{
			HBRUSH brush = CreateSolidBrush(f.c);
			FillRect(dc, &f.r, brush);
			DeleteObject(brush);
		}
		if (m_preview)
		{
			const int w = m_previewRect.right - m_previewRect.left, h = m_previewRect.bottom - m_previewRect.top;
			BITMAPINFO bmi = {};
			bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
			bmi.bmiHeader.biWidth = w;
			bmi.bmiHeader.biHeight = -h;
			bmi.bmiHeader.biPlanes = 1;
			bmi.bmiHeader.biBitCount = 32;
			bmi.bmiHeader.biCompression = BI_RGB;
			SetDIBitsToDevice(dc, m_previewRect.left, m_previewRect.top, w, h, 0, 0, 0, h, m_preview.get(), &bmi, DIB_RGB_COLORS);
		}
		EndPaint(m_hwnd, &ps);
	}

	int App::Run()
	{
		const HANDLE ready = m_streamer.ReadyEvent();
		while (true)
		{
			DWORD timeout = 0;
			// the time is known once the GPU is done; one more frame shows it even when the app would sleep until the next input
			if (m_openFence && m_gpu.CompletedFence() >= m_openFence)
				m_lastOpenMs = (Now() - m_openStart) * 1000.0, m_openFence = 0, m_quietFrames = 0;
			if (m_openFence)
				timeout = 1;
			else if (m_svgPending && !m_playing)
				timeout = 10;
			else if (m_loading && !m_playing)
				timeout = 40;
			else if (!m_playing && m_quietFrames >= 3 && GImGui->InputEventsQueue.Size == 0)
				timeout = INFINITE;
			else if (m_playing && m_realtime && IsSequence() && m_streamer.State(NextIndex()) != Streamer::kReady)
				timeout = 16;
			if (timeout && MsgWaitForMultipleObjects(1, &ready, FALSE, timeout, QS_ALLINPUT) == WAIT_OBJECT_0)
				m_quietFrames = 0;
			MSG msg;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
					return (int)msg.wParam;
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
				m_quietFrames = 0;
			}
			if (m_gpu.IsOccluded() || IsIconic(m_hwnd))
			{
				Sleep(10);
				continue;
			}
			Frame();
			m_quietFrames++;
			if (m_fullscreenToggle)
				m_fullscreenToggle = false, ToggleFullscreen();
			// cache workers start after the first frame so their buffer allocations do not delay it
			if (!m_workersStarted)
				m_streamer.StartWorkers(), m_workersStarted = true;
		}
	}

	void App::Frame()
	{
		m_inFrame = true;
#if defined(IV_STARTUP_TIMING)
		const bool timed = !m_timingDone && m_timedFrames < 2;
		auto mark = [&](const char* label) { if (timed) m_timing.Mark(label); };
#else
		auto mark = [](const char*) {};
#endif
		Advance(Now());
		ImGui_ImplDX12_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		if (!m_files.empty())
			m_streamer.States(m_cacheStates);
		TrackLoading();
		RefreshTitle();
		Hotkeys();
		if (!m_fullscreen)
			MenuBar(), Toolbar();
		Viewport();
		if (!m_fullscreen && IsSequence())
			Timeline();
		if (!m_fullscreen)
			StatusBar(), MetadataWindow();
		ImGui::Render();
		mark("frame: UI built");

		const float clear[4] = { 0.09f, 0.09f, 0.09f, 1.0f };
		ID3D12GraphicsCommandList* cmd = m_gpu.BeginFrame(clear);
		mark("frame: BeginFrame (swap chain + fence wait)");
		const uint64_t fence = m_gpu.RecordingFence();
		const uint64_t completed = m_gpu.CompletedFence();
		m_streamer.SetCompletedFence(completed);
		[[maybe_unused]] bool uploaded = false;
		if (m_shown != m_cur && !m_files.empty())
		{
			if (ID3D12Resource* src = m_streamer.AcquireForGpu(m_cur, fence))
			{
				if (m_display.Upload(m_gpu, cmd, src, fence, completed))
				{
					m_shown = m_cur, uploaded = true;
					if (m_openPending)
						m_openPending = false, m_openFence = fence;
					if (!IsSequence())
						PrefetchNext();
				}
			}
			else if (m_shown < 0 && m_streamer.State(m_cur) == Streamer::kFailed)
				m_display.Clear();
		}
		else if (m_openPending && !m_files.empty())
		{
			// opened onto the texture uploaded ahead
			m_openPending = false, m_openFence = fence;
			if (!IsSequence())
				PrefetchNext();
		}
		else if (!m_files.empty() && !IsSequence())
			UploadAhead(cmd, fence, completed);
		m_display.Retire(m_gpu, completed);
		if (m_info.kind == FileKind::Svg && !m_files.empty() && m_shown == m_cur)
			UpdateSvgView(cmd, fence, completed);
		if (m_files.empty() || m_overlayPath != m_files[m_cur] || m_shown != m_cur)
			m_display.ClearOverlay();
		m_display.Draw(cmd, MakeDrawParams(), fence, m_gpu.Width(), m_gpu.Height());
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd);
		mark("frame: ImGui RenderDrawData (font texture upload on the first frame)");
		m_gpu.EndFrame(!(m_playing && m_realtime));
		mark("frame: execute + Present");
		m_gpu.FlushDebugMessages();
#if defined(IV_STARTUP_TIMING)
		if (!m_timingDone)
		{
			if (m_timedFrames++ == 0)
				m_timing.Mark("first frame presented");
			if (uploaded)
			{
				m_timing.Mark("first frame with the image presented");
				m_timingDone = true;
				m_timing.Print();
				if (m_timingQuit)
					PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
			}
		}
#endif
		m_inFrame = false;
	}

	// times the load of the current frame for the progress estimate; a load that took long enough refines the speed of its format
	void App::TrackLoading()
	{
		const bool loading = !m_files.empty() && m_shown != m_cur && m_streamer.State(m_cur) != Streamer::kFailed;
		if (loading && (m_loadStart == 0.0 || m_loadFrame != m_cur))
		{
			std::error_code ec;
			m_loadStart = Now(), m_loadFrame = m_cur, m_loadBytes = std::filesystem::file_size(m_files[m_cur], ec);
		}
		else if (!loading && m_loadStart > 0.0)
		{
			const double took = Now() - m_loadStart;
			if (m_shown == m_cur && took > 0.1 && m_loadBytes > 0)
				m_bytesPerSecond[(int)m_info.kind] = 0.5 * m_bytesPerSecond[(int)m_info.kind] + 0.5 * m_loadBytes / took;
			m_loadStart = 0.0;
		}
		m_loading = loading;
	}

	void App::Advance(double now)
	{
		if (!m_playing || !IsSequence() || (m_shown != m_cur && m_streamer.State(m_cur) != Streamer::kFailed))
			return;
		if (!m_realtime && now < m_nextDue)
			return;
		const int next = NextIndex();
		const uint8_t state = m_streamer.State(next);
		if (state != Streamer::kReady && state != Streamer::kFailed)
			return;
		const double period = FramePeriod(next);
		m_nextDue = now - m_nextDue > period ? now + period : m_nextDue + period;
		m_cur = next;
		m_streamer.SetPlayhead(m_cur);
		m_advances.push_back(now);
		while (m_advances.size() > 2 && now - m_advances.front() > 1.0)
			m_advances.pop_front();
#if defined(GC_RELEASENOOPTS_BUILD) || defined(_DEBUG)
		if (now - m_lastPrint > 2.0 && m_advances.size() > 1)
		{
			m_lastPrint = now;
			printf("[play] %.1f fps (%s)\n", (m_advances.size() - 1) / (m_advances.back() - m_advances.front()), m_realtime ? "real-time" : "paced");
			fflush(stdout);
		}
#endif
	}

	void App::SetFrame(int index)
	{
		if (m_files.empty())
			return;
		m_cur = std::clamp(index, 0, (int)m_files.size() - 1);
		m_streamer.SetPlayhead(m_cur);
	}

	void App::Step(int delta)
	{
		if (!IsSequence())
			return;
		m_playing = false;
		const int n = (int)m_files.size();
		SetFrame(((m_cur + delta) % n + n) % n);
	}

	// index in m_scan.Items of the item delta steps from the current one (-1 at the ends), listed by the current Sequences choice
	int App::BrowseTarget(int delta, std::filesystem::path& folder)
	{
		if (m_files.empty() || delta == 0)
			return -1;
		std::error_code ec;
		const std::filesystem::path current = std::filesystem::absolute(m_browsePending.empty() ? m_files.front() : m_browsePending, ec);
		folder = current.parent_path();
		m_scan.Refresh(folder);
		const std::vector<std::wstring>& items = m_scan.Items(m_sequences);
		if (items.empty())
			return -1;
		const std::wstring name = current.filename().wstring();
		const auto at = std::lower_bound(items.begin(), items.end(), name, NameLess);
		const int pos = (int)(at - items.begin());
		const bool listed = at != items.end() && !NameLess(name, *at);
		const int target = std::clamp(listed || delta < 0 ? pos + delta : pos + delta - 1, 0, (int)items.size() - 1);
		return listed && target == pos ? -1 : target;
	}

	// next or previous image or sequence in the current folder, in Explorer's name order; skips unreadable files, stops at both ends
	void App::Browse(int delta)
	{
		std::filesystem::path folder;
		const int target = BrowseTarget(delta, folder);
		if (target < 0)
			return;
		m_browseDir = delta > 0 ? 1 : -1;
		// back onto the image on screen after a skipped one: nothing to open
		std::error_code ec;
		if (!m_browsePending.empty() && std::filesystem::equivalent(folder / m_scan.Items(m_sequences)[target], m_files.front(), ec))
		{
			m_browsePending.clear();
			return;
		}
		// Open may parse the folder again: the list is fetched per step
		for (int i = target; i >= 0 && i < (int)m_scan.Items(m_sequences).size(); i += m_browseDir)
			if (Open((folder / m_scan.Items(m_sequences)[i]).wstring(), true) || m_openAbandoned)
				return;
	}

	// SVG: the visible part is rendered again at screen resolution whenever the view changes; the cached raster shows until it arrives
	void App::UpdateSvgView(ID3D12GraphicsCommandList* cmd, uint64_t fence, uint64_t completed)
	{
		const float ox = std::round(m_vpMin.x + m_origin.x), oy = std::round(m_vpMin.y + m_origin.y);
		const float x0 = std::floor(std::max(m_vpMin.x, ox)), y0 = std::floor(std::max(m_vpMin.y, oy));
		const float x1 = std::ceil(std::min(m_vpMin.x + m_vpSize.x, ox + m_info.width * m_zoom)), y1 = std::ceil(std::min(m_vpMin.y + m_vpSize.y, oy + m_info.height * m_zoom));
		if (x1 > x0 && y1 > y0)
		{
			const SvgView::Request request = { m_files[m_cur], m_info.width, m_info.height, (x0 - ox) / m_zoom, (y0 - oy) / m_zoom, m_zoom, (int)(x1 - x0), (int)(y1 - y0) };
			if (!(request == m_svgRequested))
				m_svgView.Render(request), m_svgRequested = request, m_svgPending = true;
		}
		SvgView::Result result;
		if (m_display.OverlaySlotFree(completed) && m_svgView.Take(result))
		{
			m_svgPending = !(result.request == m_svgRequested);
			if (result.request.path == m_files[m_cur] && !result.rgba.empty())
			{
				m_display.SetOverlay(m_gpu, cmd, result.rgba.data(), result.request.width, result.request.height, result.request.x0, result.request.y0, result.request.zoom, fence, completed);
				m_overlayPath = result.request.path;
			}
		}
	}

	// the prefetched next item goes to the GPU while this one is on screen, so the wheel step only swaps textures
	void App::UploadAhead(ID3D12GraphicsCommandList* cmd, uint64_t fence, uint64_t completed)
	{
		std::wstring path;
		Layout layout;
		ID3D12Resource* pixels = nullptr;
		bool srgb = false;
		if (m_prefetch.Ready(path, layout, pixels, srgb) && path != m_aheadPath && m_display.UploadAhead(m_gpu, cmd, pixels, layout, srgb, fence, completed))
			m_aheadPath = path;
	}

	// once a single image is on screen, decode the item the wheel would open next
	void App::PrefetchNext()
	{
		std::filesystem::path folder;
		const int target = BrowseTarget(m_browseDir, folder);
		if (target < 0)
			return;
		const std::wstring& item = m_scan.Items(m_sequences)[target];
		FileKind kind = FileKind::Exr;
		if (KindFromExtension(item, kind) && kind == FileKind::Svg && !m_svgFonts)
			fmt::RegisterSvgFonts(), m_svgFonts = true;
		m_prefetch.Request((folder / item).wstring(), m_streamer, Par((int)std::max(1u, std::thread::hardware_concurrency() / 2)));
	}

	// whole wheel notches since the last call (touchpads deliver fractions); positive = wheel up
	int App::WheelNotches()
	{
		m_wheelAcc += ImGui::GetIO().MouseWheel;
		const int notches = (int)m_wheelAcc;
		m_wheelAcc -= (float)notches;
		return notches;
	}

	// full screen: a borderless popup over the window's monitor without UI, the image fitted; toggling back restores window placement and view
	void App::ToggleFullscreen()
	{
		// the WM_SIZE messages of the transition only resize the swap chain; one frame is drawn at the end
		m_inFrame = true;
		if (!m_fullscreen)
		{
			GetWindowPlacement(m_hwnd, &m_savedPlacement);
			m_savedZoom = m_zoom, m_savedOrigin = m_origin;
			MONITORINFO mi = { sizeof(mi) };
			GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
			SetWindowLongPtrW(m_hwnd, GWL_STYLE, (GetWindowLongPtrW(m_hwnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
			SetWindowPos(m_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			m_fullscreen = true;
			m_fitPending = true;
		}
		else
		{
			SetWindowLongPtrW(m_hwnd, GWL_STYLE, (GetWindowLongPtrW(m_hwnd, GWL_STYLE) & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
			SetWindowPlacement(m_hwnd, &m_savedPlacement);
			SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
			m_fullscreen = false;
			m_restoreView = true;
		}
		m_inFrame = false;
		Frame();
	}

	// animations keep their own frame times until the fps field is edited
	double App::FramePeriod(int index) const
	{
		if (m_fileTiming && index >= 0 && index < (int)m_info.delays.size())
			return m_info.delays[index];
		return 1.0 / std::clamp(m_fps, 1, 1000);
	}

	void App::TogglePlay()
	{
		if (!IsSequence())
			return;
		m_playing = !m_playing;
		m_nextDue = Now() + FramePeriod(m_cur);
		m_advances.clear();
	}

	void App::OpenDialog()
	{
		// "Images" with every extension, then one filter per file kind
		const auto formats = Formats();
		std::vector<std::pair<std::string, std::string>> groups = { { "Images", "" } };
		for (size_t i = 0; i < formats.size(); ++i)
		{
			groups[0].second += (i ? "," : "") + std::string(formats[i].ext);
			if (i == 0 || formats[i - 1].kind != formats[i].kind)
				groups.push_back({ formats[i].name, formats[i].ext });
			else
				groups.back().second += "," + std::string(formats[i].ext);
		}
		std::vector<nfdu8filteritem_t> filters;
		for (const auto& [name, exts] : groups)
			filters.push_back({ name.c_str(), exts.c_str() });
		const std::string folder = m_files.empty() ? std::string() : ToUtf8(std::filesystem::path(m_files[m_cur]).parent_path().wstring());
		nfdopendialogu8args_t args = {};
		args.filterList = filters.data();
		args.filterCount = (nfdfiltersize_t)filters.size();
		args.defaultPath = folder.empty() ? nullptr : folder.c_str();
		args.parentWindow = { NFD_WINDOW_HANDLE_TYPE_WINDOWS, m_hwnd };
		nfdu8char_t* out = nullptr;
		if (NFD_OpenDialogU8_With(&out, &args) == NFD_OKAY && out)
		{
			const std::wstring path = FromUtf8(out);
			NFD_FreePathU8(out);
			Open(path);
		}
	}

	// the frame on screen (the current timeline frame of an animation) in a format picked by extension; see SaveImage for what each format receives
	void App::SaveDialog()
	{
		std::vector<uint8_t> pixels;
		if (m_shown < 0 || !m_streamer.CopyFrame(m_shown, pixels))
			return;
		std::vector<std::pair<std::string, std::string>> groups = { { "Images", "" } };
		for (const FormatDesc& f : Formats())
		{
			if (!CanSave(FromUtf8(std::string("x.") + f.ext)))
				continue;
			groups[0].second += (groups[0].second.empty() ? "" : ",") + std::string(f.ext);
			if (groups.size() > 1 && groups.back().first == f.name)
				groups.back().second += "," + std::string(f.ext);
			else
				groups.push_back({ f.name, f.ext });
		}
		std::vector<nfdu8filteritem_t> filters;
		for (const auto& [name, exts] : groups)
			filters.push_back({ name.c_str(), exts.c_str() });
		const std::filesystem::path current(m_files[m_shown]);
		const std::string folder = ToUtf8(current.parent_path().wstring());
		const std::string name = ToUtf8(current.stem().wstring()) + (CanSave(current.wstring()) ? ToUtf8(current.extension().wstring()) : ".png");
		nfdsavedialogu8args_t args = {};
		args.filterList = filters.data();
		args.filterCount = (nfdfiltersize_t)filters.size();
		args.defaultPath = folder.empty() ? nullptr : folder.c_str();
		args.defaultName = name.c_str();
		args.parentWindow = { NFD_WINDOW_HANDLE_TYPE_WINDOWS, m_hwnd };
		nfdu8char_t* out = nullptr;
		if (NFD_SaveDialogU8_With(&out, &args) != NFD_OKAY || !out)
			return;
		const std::wstring path = FromUtf8(out);
		NFD_FreePathU8(out);
		if (!CanSave(path))
		{
			m_error = "save: unsupported file type (" + std::string(SaveExtensions()) + ")";
			return;
		}
		SetCursor(LoadCursorW(nullptr, IDC_WAIT));
		const std::string err = SaveImage(path, m_layout, pixels.data(), MakeDrawParams(), Par((int)std::thread::hardware_concurrency()));
		const std::string file = ToUtf8(std::filesystem::path(path).filename().wstring());
		m_notice = err.empty() ? "saved " + file : std::string();
		m_error = err.empty() ? std::string() : "save " + file + ": " + err;
	}

	void App::FitView()
	{
		if (m_info.width <= 0 || m_vpSize.x <= 0)
			return;
		m_zoom = std::clamp(std::min(m_vpSize.x / m_info.width, m_vpSize.y / m_info.height), kMinZoom, kMaxZoom);
		m_origin = ImVec2(std::floor((m_vpSize.x - m_info.width * m_zoom) * 0.5f), std::floor((m_vpSize.y - m_info.height * m_zoom) * 0.5f));
	}

	void App::ActualSize()
	{
		if (m_info.width <= 0)
			return;
		m_zoom = 1.0f;
		m_origin = ImVec2(std::floor((m_vpSize.x - m_info.width) * 0.5f), std::floor((m_vpSize.y - m_info.height) * 0.5f));
	}

	// a new image of another size is fitted (or shown 1:1 when it fits) before its first frame is drawn
	void App::ApplyPendingView()
	{
		if (!m_viewPending || m_info.width <= 0 || m_vpSize.x <= 0)
			return;
		m_viewPending = false;
		if (!m_fullscreen && m_info.width <= m_vpSize.x && m_info.height <= m_vpSize.y)
			ActualSize();
		else
			FitView();
	}

	void App::ZoomAround(ImVec2 screen, float zoom, ImVec2 anchorOrigin, float anchorZoom)
	{
		const ImVec2 q = (screen - m_vpMin - anchorOrigin) / anchorZoom;
		m_zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
		m_origin = screen - m_vpMin - q * m_zoom;
	}

	float App::MenuHeight() const
	{
		return ImGui::GetFrameHeight();
	}

	float App::RowHeight() const
	{
		return ImGui::GetFrameHeight() + 2.0f * ImGui::GetStyle().WindowPadding.y;
	}

	bool App::BeginBar(const char* id, float y, float height)
	{
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(mv->Pos.x, mv->Pos.y + y));
		ImGui::SetNextWindowSize(ImVec2(mv->Size.x, height));
		return ImGui::Begin(id, nullptr, kBarFlags);
	}

	void App::Hotkeys()
	{
		const ImGuiIO& io = ImGui::GetIO();
		if (io.WantTextInput)
			return;
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O))
			OpenDialog();
		if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S) && m_shown >= 0)
			SaveDialog();
		if (io.KeyMods != ImGuiMod_None)
			return;
		// Esc closes an open menu or combo first
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		{
			if (m_fullscreen)
				m_fullscreenToggle = true;
			else
				PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
		}
		if (ImGui::IsKeyPressed(ImGuiKey_F11, false))
			m_fullscreenToggle = true;
		if (ImGui::IsKeyPressed(ImGuiKey_I, false) && !m_files.empty())
			m_showMeta = !m_showMeta;
		if (ImGui::IsKeyPressed(ImGuiKey_F, false))
			FitView();
		if (ImGui::IsKeyPressed(ImGuiKey_H, false))
			ActualSize();
		if (ImGui::IsKeyPressed(ImGuiKey_Space, false))
			TogglePlay();
		if (ImGui::IsKeyPressed(ImGuiKey_R, false))
			m_realtime = !m_realtime;
		if (ImGui::IsKeyPressed(ImGuiKey_A, false))
			m_viewTransform = m_viewTransform == ViewTransform::Aces ? ViewTransform::Srgb : ViewTransform::Aces;
		if (ImGui::IsKeyPressed(ImGuiKey_PageUp) && m_view > 0)
			ApplyView(m_view - 1);
		if (ImGui::IsKeyPressed(ImGuiKey_PageDown) && m_view + 1 < (int)m_info.views.size())
			ApplyView(m_view + 1);
		if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
			Step(-1);
		if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
			Step(1);
		if (ImGui::IsKeyPressed(ImGuiKey_Home, false) && IsSequence())
			m_playing = false, SetFrame(0);
		if (ImGui::IsKeyPressed(ImGuiKey_End, false) && IsSequence())
			m_playing = false, SetFrame((int)m_files.size() - 1);
	}

	void App::MenuBar()
	{
		if (!ImGui::BeginMainMenuBar())
			return;
		if (ImGui::BeginMenu("File"))
		{
			if (ImGui::MenuItem("Open...", "Ctrl+O"))
				OpenDialog();
			if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, m_shown >= 0))
				SaveDialog();
			if (ImGui::MenuItem("Metadata", "I", m_showMeta, !m_files.empty()))
				m_showMeta = !m_showMeta;
			ImGui::Separator();
			if (ImGui::MenuItem("Exit", "Alt+F4"))
				PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("View"))
		{
			if (ImGui::MenuItem("Fit", "F", false, m_info.width > 0))
				FitView();
			if (ImGui::MenuItem("Actual size", "H", false, m_info.width > 0))
				ActualSize();
			ImGui::Separator();
			if (ImGui::MenuItem("Full Screen", "F11", m_fullscreen))
				m_fullscreenToggle = true;
			ImGui::EndMenu();
		}
		ImGui::EndMainMenuBar();
	}

	// the shown file's metadata (File > Metadata, I), read again when another file shows but not while playing, so playback never waits on header reads
	void App::MetadataWindow()
	{
		if (!m_showMeta || m_files.empty())
			return;
		const std::wstring& path = m_files[m_cur];
		if (path != m_metaPath && !m_playing)
			m_meta = ReadMetadata(path, m_info), m_metaPath = path;
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		const float em = ImGui::GetFontSize();
		ImGui::SetNextWindowSize(ImVec2(em * 32.0f, std::min(mv->Size.y * 0.7f, em * 48.0f)), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowPos(ImVec2(mv->Pos.x + std::max(0.0f, mv->Size.x - em * 33.0f), mv->Pos.y + MenuHeight() + RowHeight() + em * 0.5f), ImGuiCond_FirstUseEver);
		const std::string title = "Metadata   " + ToUtf8(std::filesystem::path(m_metaPath).filename().wstring()) + "###metadata";
		if (ImGui::Begin(title.c_str(), &m_showMeta, ImGuiWindowFlags_NoCollapse))
		{
			if (ImGui::Button("Copy all"))
			{
				std::string text;
				for (const MetaEntry& e : m_meta)
					text += e.section ? (text.empty() ? "" : "\n") + e.key + "\n" : e.key + ": " + e.value + "\n";
				ImGui::SetClipboardText(text.c_str());
			}
			if (ImGui::BeginTable("##meta", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY))
			{
				ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthFixed, em * 11.0f);
				ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
				for (const MetaEntry& e : m_meta)
				{
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					if (e.section)
					{
						ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
						ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", e.key.c_str());
						continue;
					}
					ImGui::TextDisabled("%s", e.key.c_str());
					ImGui::TableNextColumn();
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(e.value.c_str());
					ImGui::PopTextWrapPos();
				}
				ImGui::EndTable();
			}
		}
		ImGui::End();
	}

	void App::Toolbar()
	{
		BeginBar("##toolbar", MenuHeight(), RowHeight());
		const float em = ImGui::GetFontSize();
		ImGui::AlignTextToFramePadding();
		ImGui::SetNextItemWidth(em * 18.0f);
		ImGui::BeginDisabled(m_info.views.empty());
		const std::string preview = m_info.views.empty() ? std::string() : m_info.views[m_view].label + "   " + m_info.views[m_view].type;
		if (ImGui::BeginCombo("##channel", preview.c_str(), ImGuiComboFlags_HeightLarge))
		{
			float labelW = 0.0f;
			for (const ViewDesc& v : m_info.views)
				labelW = std::max(labelW, ImGui::CalcTextSize(v.label.c_str()).x);
			const float typeX = ImGui::GetCursorPosX() + labelW + em * 1.5f;
			for (int i = 0; i < (int)m_info.views.size(); ++i)
			{
				if (i == m_info.firstSingle && i > 0)
					ImGui::Separator();
				ImGui::PushID(i);
				if (ImGui::Selectable(m_info.views[i].label.c_str(), i == m_view))
					ApplyView(i);
				ImGui::SameLine(typeX);
				ImGui::TextDisabled("%s", m_info.views[i].type.c_str());
				if (i == m_view)
					ImGui::SetItemDefaultFocus();
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		else if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
			ApplyView(std::clamp(m_view - WheelNotches(), 0, (int)m_info.views.size() - 1));
		ImGui::EndDisabled();
		ImGui::SameLine(0.0f, em);
		ImGui::SetNextItemWidth(em * 7.0f);
		ImGui::DragFloat("##exposure", &m_exposure, 0.02f, -20.0f, 20.0f, "%+.2f EV");
		ImGui::SameLine(0.0f, em);
		ImGui::TextUnformatted("View");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(em * 5.5f);
		const char* transforms[] = { "Linear", "sRGB", "ACES" };
		int transform = (int)m_viewTransform;
		if (ImGui::BeginCombo("##view", transforms[transform]))
		{
			for (int i = 0; i < 3; ++i)
				if (ImGui::Selectable(transforms[i], i == transform))
					m_viewTransform = (ViewTransform)i;
			ImGui::EndCombo();
		}
		else if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
			m_viewTransform = (ViewTransform)std::clamp(transform - WheelNotches(), 0, 2);
		ImGui::SameLine(0.0f, em);
		ImGui::BeginDisabled(m_files.empty() || m_info.frames > 1);
		bool sequences = m_sequences;
		if (ImGui::Checkbox("Sequences", &sequences))
		{
			m_sequences = sequences;
			Open(std::wstring(m_files[m_cur]), true);
		}
		ImGui::EndDisabled();
		ImGui::SameLine(0.0f, em);
		ImGui::Text("%.1f%%", m_zoom * 100.0f);
		ImGui::End();
	}

	void App::Viewport()
	{
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		const float top = m_fullscreen ? 0.0f : MenuHeight() + RowHeight();
		const float bottom = m_fullscreen ? 0.0f : RowHeight() * (IsSequence() ? 2.0f : 1.0f);
		const ImVec2 pos(mv->Pos.x, mv->Pos.y + top);
		const ImVec2 size(mv->Size.x, std::max(mv->Size.y - top - bottom, 1.0f));
		ImGui::SetNextWindowPos(pos);
		ImGui::SetNextWindowSize(size);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
		ImGui::Begin("##viewport", nullptr, kBarFlags | ImGuiWindowFlags_NoBackground);
		ImGui::PopStyleVar();
		if (m_vpSize.x > 0 && (size.x != m_vpSize.x || size.y != m_vpSize.y))
			m_origin += (size - m_vpSize) * 0.5f;
		m_vpMin = pos;
		m_vpSize = size;
		ApplyPendingView();
		if (m_fitPending)
			m_fitPending = false, FitView();
		if (m_restoreView)
			m_restoreView = false, m_zoom = m_savedZoom, m_origin = m_savedOrigin;

		ImGui::InvisibleButton("##canvas", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const ImGuiIO& io = ImGui::GetIO();
		if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
			Browse(-WheelNotches());
		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Middle))
			m_origin += io.MouseDelta;
		// a quick middle click (released within 250 ms, no drag) toggles the full-screen view; wall time, the app sleeps between input events
		if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
			m_mmbDownTime = Now();
		if (m_mmbDownTime > 0.0 && ImGui::IsMouseReleased(ImGuiMouseButton_Middle))
		{
			if (Now() - m_mmbDownTime < 0.25 && io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Middle] < io.MouseDragThreshold * io.MouseDragThreshold)
				m_fullscreenToggle = true;
			m_mmbDownTime = 0.0;
		}
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			m_rmbAnchor = io.MousePos, m_rmbOrigin = m_origin, m_rmbZoom = m_zoom;
		if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
		{
			const ImVec2 d = io.MousePos - m_rmbAnchor;
			ZoomAround(m_rmbAnchor, m_rmbZoom * std::exp2((d.x - d.y) / (ImGui::GetFontSize() * 12.0f)), m_rmbOrigin, m_rmbZoom);
		}
		if (m_fullscreen && LoadingVisible())
		{
			const std::string text = ToUtf8(ProgressText(EstimatedProgress(Now() - m_loadStart, m_loadBytes, m_info.kind)));
			const ImVec2 ts = ImGui::CalcTextSize(text.c_str()), at(pos.x + ImGui::GetFontSize(), pos.y + size.y - ts.y - ImGui::GetFontSize());
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			dl->AddRectFilled(at - ImVec2(6, 4), at + ts + ImVec2(6, 4), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f), 4.0f);
			dl->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), text.c_str());
		}
		if (m_files.empty())
		{
			const std::string hint = "File > Open, or drop an image here (" + SupportedExtensions() + ")";
			const ImVec2 ts = ImGui::CalcTextSize(hint.c_str());
			ImGui::GetWindowDrawList()->AddText(pos + (size - ts) * 0.5f, ImGui::GetColorU32(ImGuiCol_TextDisabled), hint.c_str());
		}
		ImGui::End();
	}

	void App::Timeline()
	{
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		BeginBar("##timeline", mv->Size.y - RowHeight() * 2.0f, RowHeight());
		const float em = ImGui::GetFontSize();
		const int n = (int)m_files.size();
		if (ImGui::Button("|<"))
			m_playing = false, SetFrame(0);
		ImGui::SameLine();
		if (ImGui::ArrowButton("##back", ImGuiDir_Left))
			Step(-1);
		ImGui::SameLine();
		if (ImGui::Button(m_playing ? "Pause" : "Play", ImVec2(em * 3.5f, 0.0f)))
			TogglePlay();
		ImGui::SameLine();
		if (ImGui::ArrowButton("##fwd", ImGuiDir_Right))
			Step(1);
		ImGui::SameLine();
		if (ImGui::Button(">|"))
			m_playing = false, SetFrame(n - 1);
		ImGui::SameLine();

		// frame bar: click or drag to scrub
		const float barW = std::max(ImGui::GetContentRegionAvail().x - em * 21.0f, em * 6.0f);
		const ImVec2 r0 = ImGui::GetCursorScreenPos(), r1(r0.x + barW, r0.y + ImGui::GetFrameHeight());
		ImGui::InvisibleButton("##frames", r1 - r0);
		// the playhead travels edge to edge: first frame flush left, last frame flush right
		const float hw = std::clamp(barW / n * 0.5f, 1.0f, em * 0.5f), step = (barW - 2.0f * hw) / (n - 1);
		auto frameX = [&](int i) { return r0.x + hw + i * step; };
		const int hoverIdx = std::clamp((int)std::lround((ImGui::GetIO().MousePos.x - r0.x - hw) / step), 0, n - 1);
		if (ImGui::IsItemActivated())
			m_playing = false;
		if (ImGui::IsItemActive())
			SetFrame(hoverIdx);
		else if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%d", m_frameNumbers[hoverIdx]);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(r0, r1, ImGui::GetColorU32(ImGui::IsItemHovered() || ImGui::IsItemActive() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
		dl->PushClipRect(r0, r1, true);

		// ruler: minor ticks at half height, major ticks at full height, both on round frame numbers for the current pixels per frame
		const int steps[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000, 100000 };
		int minor = steps[std::size(steps) - 1], major = minor;
		for (int s : steps)
			if (s * step >= em * 0.4f)
			{
				minor = s;
				break;
			}
		for (int s : steps)
			if (s % minor == 0 && s * step >= em * 4.0f)
			{
				major = s;
				break;
			}
		const ImU32 minorCol = ImGui::GetColorU32(ImGuiCol_Text, 0.25f), majorCol = ImGui::GetColorU32(ImGuiCol_Text, 0.55f);
		for (int i = 0; i < n; ++i)
		{
			const int f = m_frameNumbers[i];
			const bool isMajor = f % major == 0;
			if (!isMajor && f % minor != 0)
				continue;
			const float x = std::floor(frameX(i)) + 0.5f;
			dl->AddLine(ImVec2(x, isMajor ? r0.y : r0.y + (r1.y - r0.y) * 0.5f), ImVec2(x, r1.y), isMajor ? majorCol : minorCol);
		}

		const float barH = std::max(2.0f, em * 0.2f);
		for (int i = 0; i < n && (int)m_cacheStates.size() == n;)
		{
			int j = i;
			while (j < n && m_cacheStates[j] == m_cacheStates[i])
				++j;
			const uint8_t st = m_cacheStates[i];
			if (st != Streamer::kNone)
			{
				const ImU32 col = st == Streamer::kReady ? IM_COL32(70, 170, 90, 255) : st == Streamer::kDecoding ? IM_COL32(210, 170, 60, 255) : IM_COL32(210, 60, 60, 255);
				dl->AddRectFilled(ImVec2(i == 0 ? r0.x : frameX(i) - step * 0.5f, r1.y - barH), ImVec2(j == n ? r1.x : frameX(j) - step * 0.5f, r1.y), col);
			}
			i = j;
		}

		// playhead and its frame number
		const float cx = frameX(m_cur);
		dl->AddRectFilled(ImVec2(cx - hw, r0.y), ImVec2(cx + hw, r1.y), ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
		char label[32];
		snprintf(label, sizeof(label), "%d", m_frameNumbers[m_cur]);
		const ImVec2 ts = ImGui::CalcTextSize(label);
		const float pad = em * 0.25f;
		float tx = cx + hw + pad * 2.0f;
		if (tx + ts.x + pad > r1.x)
			tx = cx - hw - pad * 2.0f - ts.x;
		const float ty = r0.y + (r1.y - r0.y - ts.y) * 0.5f;
		dl->AddRectFilled(ImVec2(tx - pad, ty), ImVec2(tx + ts.x + pad, ty + ts.y), ImGui::GetColorU32(ImGuiCol_WindowBg, 0.85f), pad);
		dl->AddText(ImVec2(tx, ty), ImGui::GetColorU32(ImGuiCol_Text), label);
		dl->PopClipRect();

		ImGui::SameLine();
		ImGui::SetNextItemWidth(em * 4.0f);
		const double animTime = m_fileTiming ? std::accumulate(m_info.delays.begin(), m_info.delays.end(), 0.0) : 0.0;
		int fps = m_fileTiming && animTime > 0.0 ? std::clamp((int)std::lround(m_info.delays.size() / animTime), 1, 1000) : m_fps;
		if (ImGui::InputInt("fps", &fps, 0, 0))
			m_fps = std::clamp(fps, 1, 1000), m_fileTiming = false;
		if (m_fileTiming && ImGui::IsItemHovered())
			ImGui::SetTooltip("the file's own frame times (average shown); type a rate to override");
		ImGui::SameLine();
		ImGui::Checkbox("Real-time", &m_realtime);
		ImGui::SameLine();
		if (m_playing && m_advances.size() > 1)
			ImGui::Text("%.1f fps", (m_advances.size() - 1) / (m_advances.back() - m_advances.front()));
		else
			ImGui::TextDisabled("%d/%d cached", (int)std::count(m_cacheStates.begin(), m_cacheStates.end(), (uint8_t)Streamer::kReady), n);
		ImGui::End();
	}

	void App::StatusBar()
	{
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		BeginBar("##status", mv->Size.y - RowHeight(), RowHeight());
		const float em = ImGui::GetFontSize();
		ImGui::AlignTextToFramePadding();
		if (m_files.empty())
			ImGui::TextDisabled("No image");
		else
		{
			const std::string name = ToUtf8(std::filesystem::path(m_files[m_cur]).filename().wstring());
			ImGui::Text("%s   %d x %d (%s)", name.c_str(), m_info.width, m_info.height, AspectRatio(m_info.width, m_info.height).c_str());
			const std::string tag = m_info.kind == FileKind::Cr3 ? "[JPEG PREVIEW]" : m_cur < (int)m_info.notes.size() ? "[" + m_info.notes[m_cur] + "]" : std::string();
			if (!tag.empty())
			{
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", tag.c_str());
			}
			if (m_info.mips.size() > 1)
			{
				const int mip = m_info.views[m_view].mip;
				ImGui::SameLine();
				ImGui::SetNextItemWidth(em * 4.5f);
				if (ImGui::BeginCombo("##mip", ("mip " + std::to_string(mip)).c_str(), ImGuiComboFlags_HeightLarge))
				{
					for (int l = 0; l < (int)m_info.mips.size(); ++l)
					{
						const std::string item = "mip " + std::to_string(l) + "   " + std::to_string(m_info.mips[l].first) + " x " + std::to_string(m_info.mips[l].second);
						if (ImGui::Selectable(item.c_str(), l == mip))
							SetMip(l);
						if (l == mip)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
				else if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
					SetMip(std::clamp(mip - WheelNotches(), 0, (int)m_info.mips.size() - 1));
			}
			if (m_lastOpenMs >= 0.0)
			{
				ImGui::SameLine(0.0f, em);
				ImGui::TextDisabled("loaded in %.0f ms", m_lastOpenMs);
			}
			const ImGuiIO& io = ImGui::GetIO();
			const ImVec2 o(std::round(m_vpMin.x + m_origin.x), std::round(m_vpMin.y + m_origin.y));
			const bool inVp = io.MousePos.x >= m_vpMin.x && io.MousePos.y >= m_vpMin.y && io.MousePos.x < m_vpMin.x + m_vpSize.x && io.MousePos.y < m_vpMin.y + m_vpSize.y;
			const int x = (int)std::floor((io.MousePos.x - o.x) / m_zoom), y = (int)std::floor((io.MousePos.y - o.y) / m_zoom);
			float v[4] = {};
			const int nc = inVp && m_shown >= 0 ? m_streamer.ReadPixel(m_shown, x, y, v) : 0;
			if (nc)
			{
				const ViewDesc& view = m_info.views[m_view];
				const float codeMax = m_layout.format == DXGI_FORMAT_R8G8B8A8_UNORM ? 255.0f : 65535.0f;
				ImGui::SameLine(0.0f, em * 2.0f);
				if (view.comp >= 0 && IsDisplayReferred(m_info))
					ImGui::Text("x %d  y %d    %s %.4f (%d)", x, y, view.label.c_str(), v[view.comp], (int)std::lround(v[view.comp] * codeMax));
				else if (view.comp >= 0)
					ImGui::Text("x %d  y %d    %s %.6g", x, y, view.label.c_str(), v[view.comp]);
				else if (IsDisplayReferred(m_info))
					ImGui::Text("x %d  y %d    R %.4f  G %.4f  B %.4f  A %.4f  (%d %d %d %d)", x, y, v[0], v[1], v[2], v[3], (int)std::lround(v[0] * codeMax), (int)std::lround(v[1] * codeMax), (int)std::lround(v[2] * codeMax), (int)std::lround(v[3] * codeMax));
				else if (nc == 1)
					ImGui::Text("x %d  y %d    %s %.6g", x, y, view.chans[0].c_str(), v[0]);
				else
					ImGui::Text("x %d  y %d    R %.6g  G %.6g  B %.6g  A %.6g", x, y, v[0], v[1], v[2], v[3]);
			}
		}
		if (!m_notice.empty())
		{
			ImGui::SameLine(0.0f, em * 2.0f);
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1.0f), "%s", m_notice.c_str());
		}
		const std::string frameErr = m_files.empty() ? std::string() : m_streamer.Error(m_cur);
		const std::string& err = m_error.empty() ? frameErr : m_error;
		if (!err.empty())
		{
			ImGui::SameLine(0.0f, em * 2.0f);
			ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1.0f), "%s", err.c_str());
		}
		ImGui::End();
	}

	DrawParams App::MakeDrawParams() const
	{
		DrawParams p;
		p.vpMin[0] = m_vpMin.x;
		p.vpMin[1] = m_vpMin.y;
		p.vpMax[0] = m_vpMin.x + m_vpSize.x;
		p.vpMax[1] = m_vpMin.y + m_vpSize.y;
		p.imgOrigin[0] = std::round(m_vpMin.x + m_origin.x);
		p.imgOrigin[1] = std::round(m_vpMin.y + m_origin.y);
		p.zoom = m_zoom;
		p.exposure = m_exposure;
		p.view = m_viewTransform;
		if (!m_info.views.empty())
		{
			const ViewDesc& v = m_info.views[m_view];
			p.srgbIn = IsDisplayReferred(m_info);
			if (v.comp >= 0)
				p.gray = true, p.mask[v.comp] = 1.0f;
			else if (v.nbComps == 1)
				p.gray = true, p.mask[0] = 1.0f;
			if (m_info.colorMatrix && !p.gray)
				p.colorMatrix = true, std::copy(m_info.toRec709, m_info.toRec709 + 9, p.matrix);
		}
		return p;
	}

	bool App::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& result)
	{
		switch (msg)
		{
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED)
				m_gpu.Resize(LOWORD(lParam), HIWORD(lParam));
			if (!m_gpuReady)
				InvalidateRect(m_hwnd, nullptr, FALSE);
			else if (wParam != SIZE_MINIMIZED && !m_inFrame)
				Frame();
			result = 0;
			return true;
		// Windows runs its own message loop while the window is dragged or resized; a timer keeps frames coming from inside it
		case WM_ENTERSIZEMOVE:
			SetTimer(m_hwnd, kSizeMoveTimer, USER_TIMER_MINIMUM, nullptr);
			result = 0;
			return true;
		case WM_EXITSIZEMOVE:
			KillTimer(m_hwnd, kSizeMoveTimer);
			result = 0;
			return true;
		case WM_TIMER:
			if (wParam != kSizeMoveTimer)
				return false;
			if (m_gpuReady && !m_inFrame && !IsIconic(m_hwnd))
				Frame();
			result = 0;
			return true;
		case WM_PAINT:
			if (m_gpuReady)
				return false;
			PaintPreview();
			result = 0;
			return true;
		case WM_DROPFILES:
		{
			HDROP drop = (HDROP)wParam;
			const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
			std::wstring path(len, L'\0');
			DragQueryFileW(drop, 0, path.data(), len + 1);
			DragFinish(drop);
			if (!path.empty())
				Open(path);
			result = 0;
			return true;
		}
		case WM_DPICHANGED:
		{
			const RECT* r = (const RECT*)lParam;
			SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
			// fonts follow the new monitor's scale, as the docking branch's io.ConfigDpiScaleFonts did
			if (ImGui::GetCurrentContext())
				ImGui::GetStyle().FontScaleDpi = HIWORD(wParam) / 96.0f;
			result = 0;
			return true;
		}
		}
		return false;
	}
}
