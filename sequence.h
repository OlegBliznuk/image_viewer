#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace iv
{
	// file name order of Explorer (StrCmpLogicalW)
	bool NameLess(const std::wstring& a, const std::wstring& b);

	// one parse of a folder for the sequence detection of every open and the wheel's item list, kept while the folder's last-write time stays
	class FolderScan
	{
	public:
		// parses again when the folder or its last-write time changed
		void Refresh(const std::filesystem::path& folder);
		// files of the sequence the path belongs to, sorted by frame number; the path alone when it is no sequence
		void Detect(const std::wstring& path, std::vector<std::wstring>& files, std::vector<int>& frameNumbers, int& index);
		// one file name per image or sequence (its first frame), every file on its own without sequences, in NameLess order
		const std::vector<std::wstring>& Items(bool sequences);
	private:
		struct Run
		{
			std::vector<std::wstring> files;
			std::vector<int> frames;
		};

		std::wstring m_folder;
		std::filesystem::file_time_type m_stamp{};
		std::vector<Run> m_runs;   // sequences, two or more frames
		std::vector<std::wstring> m_singles;   // file names
		std::unordered_map<std::wstring, std::pair<int, int>> m_where;   // lower-case file name of a sequence frame: run, frame index
		std::vector<std::wstring> m_items[2];
		bool m_itemsBuilt[2] = {};
	};
}
