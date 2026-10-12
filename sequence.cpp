#include "sequence.h"
#include "image_io.h"
#include "sequence_parser.h"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "shlwapi.lib")

namespace
{
	std::vector<iv::SequenceParser::String> ParserExtensions()
	{
		std::vector<iv::SequenceParser::String> exts;
		iv::SequenceParser::String ext;
		for (const char c : iv::SupportedExtensions() + " ")
			if (c != ' ')
				ext += (iv::SequenceParser::Char)c;
			else if (!ext.empty())
				exts.push_back(std::move(ext)), ext.clear();
		return exts;
	}

	std::wstring FileName(const std::wstring& path)
	{
		return path.substr(path.find_last_of(L"\\/") + 1);
	}

	std::wstring Lower(std::wstring s)
	{
		for (auto& ch : s)
			ch = (wchar_t)towlower(ch);
		return s;
	}
}

namespace iv
{
	bool NameLess(const std::wstring& a, const std::wstring& b)
	{
		return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
	}

	void FolderScan::Refresh(const std::filesystem::path& folder)
	{
		std::error_code ec;
		const auto stamp = std::filesystem::last_write_time(folder, ec);
		if (!ec && folder.native() == m_folder && stamp == m_stamp)
			return;
		m_folder = folder.native();
		m_stamp = ec ? std::filesystem::file_time_type{} : stamp;
		m_runs.clear();
		m_singles.clear();
		m_where.clear();
		m_itemsBuilt[0] = m_itemsBuilt[1] = false;
		for (SequenceParser::Sequence& sq : SequenceParser::Parse(folder, ParserExtensions(), true))
		{
			if (sq.files.size() < 2)
			{
				m_singles.push_back(FileName(sq.files[0].second));
				continue;
			}
			Run& run = m_runs.emplace_back();
			for (auto& [frame, path] : sq.files)
			{
				run.files.push_back(std::move(path));
				run.frames.push_back((int)frame);
				m_where.emplace(Lower(FileName(run.files.back())), std::make_pair((int)m_runs.size() - 1, (int)run.files.size() - 1));
			}
		}
	}

	void FolderScan::Detect(const std::wstring& path, std::vector<std::wstring>& files, std::vector<int>& frameNumbers, int& index)
	{
		files = { path };
		frameNumbers = { 0 };
		index = 0;
		std::error_code ec;
		const std::filesystem::path full = std::filesystem::absolute(path, ec);
		if (ec)
			return;
		Refresh(full.parent_path());
		const auto at = m_where.find(Lower(full.filename().native()));
		if (at == m_where.end())
			return;
		const Run& run = m_runs[at->second.first];
		files = run.files;
		frameNumbers = run.frames;
		index = at->second.second;
	}

	const std::vector<std::wstring>& FolderScan::Items(bool sequences)
	{
		std::vector<std::wstring>& items = m_items[sequences];
		if (m_itemsBuilt[sequences])
			return items;
		items = m_singles;
		for (const Run& run : m_runs)
			for (size_t i = 0; i < (sequences ? 1 : run.files.size()); ++i)
				items.push_back(FileName(run.files[i]));
		std::sort(items.begin(), items.end(), NameLess);
		m_itemsBuilt[sequences] = true;
		return items;
	}
}
