#include "image_formats.h"

#pragma warning(push)
#pragma warning(disable : 4251)
#include <lunasvg/lunasvg.h>
#pragma warning(pop)

#include <Windows.h>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "lunasvg.lib")

// SVG through lunasvg (vcpkg, lunasvg.dll delay-loaded), drawn over white the way a browser shows a standalone SVG
namespace iv::fmt
{
	void SvgImageSize(float docW, float docH, int& w, int& h)
	{
		if (docW <= 0.0f || docH <= 0.0f)
			docW = docH = 512.0f;
		// the image the drawing is scaled to: its own size, the long side capped at 4096
		const float s = std::min(1.0f, 4096.0f / std::max(docW, docH));
		w = std::max(1, (int)std::lround(docW * s));
		h = std::max(1, (int)std::lround(docH * s));
	}

	void RegisterSvgFonts()
	{
		// lunasvg itself maps sans-serif / serif / monospace / cursive / fantasy to Arial / Times New Roman / Courier New / Comic Sans MS / Impact and unknown families to Arial
		struct Face { const char* family; const wchar_t* file[4]; };   // regular, bold, italic, bold italic; a missing style takes the closest one
		const Face faces[] = {
			{ "Arial", { L"arial", L"arialbd", L"ariali", L"arialbi" } }, { "Times New Roman", { L"times", L"timesbd", L"timesi", L"timesbi" } },
			{ "Courier New", { L"cour", L"courbd", L"couri", L"courbi" } }, { "Segoe UI", { L"segoeui", L"segoeuib", L"segoeuii", L"segoeuiz" } },
			{ "Calibri", { L"calibri", L"calibrib", L"calibrii", L"calibriz" } }, { "Consolas", { L"consola", L"consolab", L"consolai", L"consolaz" } },
			{ "Verdana", { L"verdana", L"verdanab", L"verdanai", L"verdanaz" } }, { "Georgia", { L"georgia", L"georgiab", L"georgiai", L"georgiaz" } },
			{ "Tahoma", { L"tahoma", L"tahomabd" } }, { "Comic Sans MS", { L"comic", L"comicbd", L"comici", L"comicz" } }, { "Impact", { L"impact" } },
		};
		wchar_t windows[MAX_PATH] = {};
		GetWindowsDirectoryW(windows, MAX_PATH);
		// mapped, not read: the faces cost nothing until a glyph is drawn and share the system file cache; the cache keeps them for the process lifetime
		for (const Face& f : faces)
			for (int style = 0; style < 4; ++style)
			{
				if (!f.file[style])
					continue;
				const HANDLE file = CreateFileW((std::wstring(windows) + L"\\Fonts\\" + f.file[style] + L".ttf").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
				if (file == INVALID_HANDLE_VALUE)
					continue;
				LARGE_INTEGER size = {};
				const HANDLE mapping = GetFileSizeEx(file, &size) ? CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr) : nullptr;
				void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
				if (mapping)
					CloseHandle(mapping);
				CloseHandle(file);
				if (view)
					lunasvg_add_font_face_from_data(f.family, style & 1, style & 2, view, (size_t)size.QuadPart, [](void* v) { UnmapViewOfFile(v); }, view);   // a face that fails to load unmaps through the callback
			}
	}

	std::string ProbeSvg(const uint8_t* data, size_t size, Probe& out)
	{
		const auto doc = lunasvg::Document::loadFromData((const char*)data, size);
		if (!doc)
			return "svg: cannot parse";
		SvgImageSize(doc->width(), doc->height(), out.w, out.h);
		return {};
	}

	std::string DecodeSvg(const uint8_t* data, size_t size, uint8_t* dst, size_t pitch, int maxW, int maxH, const Parallel&)
	{
		const auto doc = lunasvg::Document::loadFromData((const char*)data, size);
		if (!doc)
			return "svg: cannot parse";
		int w = 0, h = 0;
		SvgImageSize(doc->width(), doc->height(), w, h);
		lunasvg::Bitmap bitmap(dst, std::min(w, maxW), std::min(h, maxH), (int)pitch);
		bitmap.clear(0xFFFFFFFF);
		doc->render(bitmap, lunasvg::Matrix(w / doc->width(), 0, 0, h / doc->height(), 0, 0));
		bitmap.convertToRGBA();
		return {};
	}
}
