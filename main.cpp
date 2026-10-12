#include "app.h"
#include "registration.h"

#include "imgui_impl_win32.h"

#include <shellapi.h>
#include <cstdlib>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		auto* app = (iv::App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
		if ((!app || app->ImGuiInputEnabled()) && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return true;
		LRESULT result = 0;
		if (app && app->HandleMessage(msg, wParam, lParam, result))
			return result;
		if (msg == WM_SYSCOMMAND && (wParam & 0xfff0) == SC_KEYMENU)
			return 0;
		if (msg == WM_DESTROY)
		{
			PostQuitMessage(0);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}
}

// usage: image_viewer [image] [--no-focus] [--d3d-debug] [--timing-quit] | --register | --unregister
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
	iv::App app;
	app.Mark("wWinMain entered (loader, CRT init, static ctors done)");
	std::wstring path;
	bool noFocus = false, debugLayer = false;
	for (int i = 1; i < __argc; ++i)
	{
		const std::wstring arg = __wargv[i];
		if (arg == L"--no-focus")
			noFocus = true;
		else if (arg == L"--d3d-debug")
			debugLayer = true;
		else if (arg == L"--timing-quit")
			app.SetTimingQuit();
		else if (arg == L"--register" || arg == L"--unregister")
			return iv::RegisterFileTypes(arg == L"--register");
		else if (path.empty() && !arg.starts_with(L"--"))
			path = arg;
	}
	app.StartGpuInit(debugLayer);
	app.Mark("GPU init thread started");
	if (!path.empty())
		app.Open(path);

	ImGui_ImplWin32_EnableDpiAwareness();
	HICON icon = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	HICON iconSmall = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, instance, icon, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"image_viewer", iconSmall };
	RegisterClassExW(&wc);
	HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"image_viewer", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance, nullptr);
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)&app);
	app.Mark("DPI awareness, window class, CreateWindow");
	app.Init(hwnd);
	DragAcceptFiles(hwnd, TRUE);
	app.PlaceWindow(!path.empty());
	ShowWindow(hwnd, noFocus ? SW_SHOWNOACTIVATE : SW_SHOWDEFAULT);
	if (noFocus)
		SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	UpdateWindow(hwnd);
	app.Mark("window shown (GDI preview painted)");
	app.InitDeferred();

	int code = 0;
	if (app.FinishGpuInit())
		code = app.Run();
	else if (IsWindow(hwnd))
		code = 1;
	SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
	app.Shutdown();
	if (IsWindow(hwnd))
		DestroyWindow(hwnd);
	UnregisterClassW(wc.lpszClassName, instance);
	return code;
}
