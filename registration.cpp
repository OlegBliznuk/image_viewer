#include "registration.h"
#include "image_io.h"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <cstdio>
#include <string>
#include <vector>

namespace iv
{
	namespace
	{
		const std::wstring kClasses = L"Software\\Classes\\";

		bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& value)
		{
			return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
		}

		std::wstring GetString(const std::wstring& key, const wchar_t* name = nullptr)
		{
			wchar_t buf[1024] = {};
			DWORD size = sizeof(buf);
			return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
		}

		std::wstring UserChoice(const std::wstring& ext)
		{
			return GetString(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\." + ext + L"\\UserChoice", L"ProgId");
		}

		std::vector<std::wstring> ValueNames(const std::wstring& key)
		{
			std::vector<std::wstring> out;
			HKEY h = nullptr;
			if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS)
				return out;
			for (DWORD i = 0;; ++i)
			{
				wchar_t name[512];
				DWORD n = 512;
				if (RegEnumValueW(h, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
					break;
				out.push_back(name);
			}
			RegCloseKey(h);
			return out;
		}

		// a per-user entry whose open command runs an image_viewer.exe from any folder: this program, maybe an older build somewhere else
		bool RunsImageViewer(const std::wstring& progId)
		{
			return !progId.empty() && StrStrIW(GetString(kClasses + progId + L"\\shell\\open\\command").c_str(), L"\\image_viewer.exe") != nullptr;
		}

		bool Same(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
	}

	// install: our ProgID per extension; entries older builds left behind are removed, and when the user's Always choice is one of them (the choice
	// itself is hash-protected) it is pointed at this exe with our icon and hidden from the Open with list until the user picks the current entry
	int RegisterFileTypes(bool install)
	{
		// a Windows-subsystem app: write to the console it was started from, unless stdout is redirected
		if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_UNKNOWN && AttachConsole(ATTACH_PARENT_PROCESS))
			freopen("CONOUT$", "w", stdout);
		wchar_t exe[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		const std::wstring command = L"\"" + std::wstring(exe) + L"\" \"%1\"";
		bool ok = true;
		// the entry Windows makes when the exe is picked by browsing; one for all extensions, so it gets the command but no per-type icon
		const std::wstring applications = L"Applications\\image_viewer.exe";
		if (install && RunsImageViewer(applications))
			ok &= SetString(kClasses + applications + L"\\shell\\open\\command", nullptr, command);
		for (const FormatDesc& f : Formats())
		{
			const std::wstring ext = FromUtf8(f.ext);
			const std::wstring progId = L"image_viewer." + ext;
			const std::wstring openWith = kClasses + L"." + ext + L"\\OpenWithProgids";
			const std::wstring icon = f.icon ? std::wstring(exe) + L",-" + std::to_wstring(f.icon) : L"%1";
			const std::wstring choice = UserChoice(ext);
			const bool ours = Same(choice, progId);
			const bool earlier = !ours && RunsImageViewer(choice);
			const bool shared = Same(choice, applications);
			for (const std::wstring& old : ValueNames(openWith))
				if (!Same(old, progId) && !Same(old, choice) && RunsImageViewer(old))
				{
					RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + old).c_str());
					RegDeleteKeyValueW(HKEY_CURRENT_USER, openWith.c_str(), old.c_str());
				}
			if (install)
			{
				ok &= SetString(kClasses + progId, nullptr, FromUtf8(f.name));
				ok &= SetString(kClasses + progId + L"\\DefaultIcon", nullptr, icon);
				ok &= SetString(kClasses + progId + L"\\shell\\open\\command", nullptr, command);
				ok &= SetString(kClasses + progId + L"\\Application", L"ApplicationName", L"image_viewer");
				ok &= RegSetKeyValueW(HKEY_CURRENT_USER, openWith.c_str(), progId.c_str(), REG_NONE, nullptr, 0) == ERROR_SUCCESS;
				if (earlier && !shared)
				{
					ok &= SetString(kClasses + choice + L"\\shell\\open\\command", nullptr, command);
					ok &= SetString(kClasses + choice + L"\\DefaultIcon", nullptr, icon);
					RegDeleteKeyValueW(HKEY_CURRENT_USER, openWith.c_str(), choice.c_str());
				}
			}
			else
			{
				const LSTATUS tree = RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + progId).c_str());
				ok &= tree == ERROR_SUCCESS || tree == ERROR_FILE_NOT_FOUND;
				RegDeleteKeyValueW(HKEY_CURRENT_USER, openWith.c_str(), progId.c_str());
				if (earlier && !shared && Same(GetString(kClasses + choice + L"\\DefaultIcon"), icon))
					RegDeleteTreeW(HKEY_CURRENT_USER, (kClasses + choice + L"\\DefaultIcon").c_str());
			}
			const bool autoEntry = Same(choice, ext + L"_auto_file");
			const char* state = !install ? (ours ? "unregistered; its Always choice now points nowhere, pick another app" : "unregistered")
				: ours ? "icon in use"
				: earlier && autoEntry ? "the auto entry of the current choice opens this exe, with our icon"
				: earlier && shared ? "the current choice opens this exe; for the icon pick image_viewer under Open with > Always"
				: earlier ? "the current choice (an earlier entry) opens this exe with our icon; pick image_viewer under Open with > Always to move to the current entry"
				: "registered; for the icon pick image_viewer under Open with > Always";
			wprintf(L"  .%-5s %S   (current default: %s)\n", ext.c_str(), state, choice.empty() ? L"none" : choice.c_str());
		}
		SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
		wprintf(L"%s %s for %s\n", ok ? L"done:" : L"failed:", install ? L"registered" : L"unregistered", exe);
		fflush(stdout);
		return ok ? 0 : 1;
	}
}
