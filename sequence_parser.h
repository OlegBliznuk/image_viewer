#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace iv
{
	// groups a folder's files by name mask (render_v#.#.exr); the frame is the number whose fixed neighbours leave the fewest groups
	class SequenceParser
	{
	public:
		using Char = std::filesystem::path::value_type;
		using String = std::filesystem::path::string_type;

		struct Range
		{
			int64_t first;
			int64_t last;
		};

		struct Sequence
		{
			String mask;   // render_v002.#.exr, the file name for a single file
			String ext;   // of the first file, the others may differ in case
			std::vector<Range> ranges;
			std::vector<std::pair<int64_t, String>> files;   // <frame, full path>, frames ascending
		};

		// files with an allowed extension (lower-case list, any case on disk); those in no sequence come back one per Sequence with frame 0 when singles is set
		static std::vector<Sequence> Parse(const std::filesystem::path& folder, const std::vector<String>& extensions, bool singles = true)
		{
			std::vector<Sequence> out;
			Listing ls;
			List(folder, extensions, ls);
			std::vector<size_t> order(ls.files.size());
			for (size_t i = 0; i < order.size(); ++i)
				order[i] = i;
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ls.Mask(a) < ls.Mask(b); });
			for (size_t st = 0; st < order.size();)
			{
				size_t en = st + 1;
				while (en < order.size() && ls.Mask(order[en]) == ls.Mask(order[st]))
					++en;
				AddGroup(out, ls, order.data() + st, en - st, singles);
				st = en;
			}
			return out;
		}

	private:
		using View = std::basic_string_view<Char>;

		struct Num
		{
			int64_t value;
			size_t at;   // offset in the file name, sign included
			size_t len;
		};

		// offsets into the Listing buffers
		struct File
		{
			size_t name, nameLen, extAt;   // the file name in Listing::names, its extension from extAt
			size_t mask, maskLen;   // name with '#' per number, '.', lower-case extension
			size_t num, nbNums;
		};

		struct Listing
		{
			String folder;   // with a trailing separator
			std::vector<Char> names;
			std::vector<Char> masks;
			std::vector<Num> nums;
			std::vector<File> files;

			View Name(size_t f) const { return { names.data() + files[f].name, files[f].nameLen }; }
			View Mask(size_t f) const { return { masks.data() + files[f].mask, files[f].maskLen }; }
			const Num& NumOf(size_t f, size_t q) const { return nums[files[f].num + q]; }
			String Path(size_t f) const
			{
				String p;
				p.reserve(folder.size() + files[f].nameLen);
				p += folder;
				p.append(names.data() + files[f].name, files[f].nameLen);
				return p;
			}
		};

		static bool IsDigit(Char c) { return c >= '0' && c <= '9'; }
		static bool IsSeparator(Char c) { return c == '/' || (std::filesystem::path::preferred_separator != '/' && c == std::filesystem::path::preferred_separator); }
		static Char Lower(Char c) { return c >= 'A' && c <= 'Z' ? Char(c + ('a' - 'A')) : c; }
		// non-ASCII code units (UTF-16 or UTF-8 bytes) count as letters
		static bool IsWordChar(Char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (uint32_t)(std::make_unsigned_t<Char>)c > 127; }

		static void List(const std::filesystem::path& folder, const std::vector<String>& extensions, Listing& ls)
		{
			ls.folder = folder.native();
			if (!ls.folder.empty() && !IsSeparator(ls.folder.back()))
				ls.folder.push_back(std::filesystem::path::preferred_separator);
			std::error_code ec;
			for (std::filesystem::directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
			{
				if (!it->is_regular_file(ec))
					continue;
				const String& full = it->path().native();
				size_t st = full.size();
				while (st > 0 && !IsSeparator(full[st - 1]))
					--st;
				File f = {};
				f.name = ls.names.size();
				f.nameLen = full.size() - st;
				ls.names.insert(ls.names.end(), full.begin() + (ptrdiff_t)st, full.end());
				const Char* s = ls.names.data() + f.name;
				ptrdiff_t dot = (ptrdiff_t)f.nameLen - 1;
				while (dot >= 0 && s[dot] != '.')
					--dot;
				f.extAt = (size_t)(dot + 1);
				bool allowed = false;
				for (size_t e = 0; e < extensions.size() && !allowed && dot > 0; ++e)
				{
					allowed = extensions[e].size() == f.nameLen - f.extAt;
					for (size_t i = 0; allowed && i < extensions[e].size(); ++i)
						allowed = Lower(s[f.extAt + i]) == extensions[e][i];
				}
				if (!allowed)
				{
					ls.names.resize(f.name);
					continue;
				}
				Tokenize(ls, f, (size_t)dot);
				ls.files.push_back(f);
			}
		}

		// '-' before digits is a sign at the name start or after a separator (render.-005, shot_-12), not after a word (shot-0001)
		static void Tokenize(Listing& ls, File& f, size_t stemLen)
		{
			const Char* s = ls.names.data() + f.name;
			f.mask = ls.masks.size();
			f.num = ls.nums.size();
			for (size_t i = 0; i < stemLen;)
			{
				const bool sign = s[i] == '-' && i + 1 < stemLen && IsDigit(s[i + 1]) && (i == 0 || !IsWordChar(s[i - 1]));
				const size_t dg = i + (sign ? 1 : 0);
				if (!IsDigit(s[dg]))
				{
					ls.masks.push_back(s[i++]);
					continue;
				}
				size_t en = dg;
				while (en < stemLen && IsDigit(s[en]))
					++en;
				size_t lead = dg;
				while (lead < en - 1 && s[lead] == '0')
					++lead;
				if (en - lead > 18)
				{
					for (; i < en; ++i)
						ls.masks.push_back(s[i]);   // too long for a frame number, kept as text
					continue;
				}
				int64_t v = 0;
				for (size_t k = lead; k < en; ++k)
					v = v * 10 + (int64_t)(s[k] - '0');
				ls.nums.push_back({ sign ? -v : v, i, en - i });
				ls.masks.push_back('#');
				i = en;
			}
			ls.masks.push_back('.');
			for (size_t i = f.extAt; i < f.nameLen; ++i)
				ls.masks.push_back(Lower(s[i]));
			f.maskLen = ls.masks.size() - f.mask;
			f.nbNums = ls.nums.size() - f.num;
		}

		static bool SameOthers(const Listing& ls, size_t a, size_t b, size_t frame)
		{
			for (size_t q = 0; q < ls.files[a].nbNums; ++q)
				if (q != frame && ls.NumOf(a, q).value != ls.NumOf(b, q).value)
					return false;
			return true;
		}

		// ordered by the other numbers, then by the frame
		static void SortBy(const Listing& ls, size_t* g, size_t n, size_t frame)
		{
			auto less = [&](size_t a, size_t b)
			{
				for (size_t q = 0; q < ls.files[a].nbNums; ++q)
					if (q != frame && ls.NumOf(a, q).value != ls.NumOf(b, q).value)
						return ls.NumOf(a, q).value < ls.NumOf(b, q).value;
				return ls.NumOf(a, frame).value < ls.NumOf(b, frame).value;
			};
			std::sort(g, g + n, less);
		}

		// one mask: the frame is the number leaving the fewest runs of equal other numbers (ties to the later one), each run is a sequence candidate
		static void AddGroup(std::vector<Sequence>& out, const Listing& ls, size_t* g, size_t n, bool singles)
		{
			const size_t k = ls.files[g[0]].nbNums;
			if (k == 0)
			{
				if (singles)
					for (size_t i = 0; i < n; ++i)
						AddSingle(out, ls, g[i]);
				return;
			}
			size_t frame = 0;
			size_t best = n + 1;
			for (size_t p = 0; p < k && k > 1; ++p)
			{
				SortBy(ls, g, n, p);
				size_t runs = 1;
				for (size_t i = 1; i < n; ++i)
					runs += SameOthers(ls, g[i - 1], g[i], p) ? 0 : 1;
				if (runs <= best)
					best = runs, frame = p;
			}
			SortBy(ls, g, n, frame);
			for (size_t st = 0; st < n;)
			{
				size_t en = st + 1;
				while (en < n && SameOthers(ls, g[st], g[en], frame))
					++en;
				AddRun(out, ls, g + st, en - st, frame, singles);
				st = en;
			}
		}

		// a sequence: two or more distinct frames and at most sqrt(files) missing ones
		static void AddRun(std::vector<Sequence>& out, const Listing& ls, const size_t* g, size_t n, size_t frame, bool singles)
		{
			auto value = [&](size_t i) { return ls.NumOf(g[i], frame).value; };
			bool distinct = n >= 2;
			for (size_t i = 1; i < n && distinct; ++i)
				distinct = value(i) != value(i - 1);
			const int64_t files = (int64_t)n;
			const int64_t missing = distinct ? value(n - 1) - value(0) + 1 - files : 0;
			if (!distinct || missing > files || missing * missing > files)
			{
				if (singles)
					for (size_t i = 0; i < n; ++i)
						AddSingle(out, ls, g[i]);
				return;
			}
			const View name = ls.Name(g[0]);
			const Num& fn = ls.NumOf(g[0], frame);
			Sequence& sq = out.emplace_back();
			sq.mask = String(name.substr(0, fn.at)) + Char('#') + String(name.substr(fn.at + fn.len));
			sq.ext = String(name.substr(ls.files[g[0]].extAt));
			sq.files.reserve(n);
			for (size_t i = 0; i < n; ++i)
			{
				const int64_t v = value(i);
				sq.files.emplace_back(v, ls.Path(g[i]));
				if (i > 0 && v == sq.ranges.back().last + 1)
					sq.ranges.back().last = v;
				else
					sq.ranges.push_back({ v, v });
			}
		}

		static void AddSingle(std::vector<Sequence>& out, const Listing& ls, size_t f)
		{
			const View name = ls.Name(f);
			Sequence& sq = out.emplace_back();
			sq.mask = String(name);
			sq.ext = String(name.substr(ls.files[f].extAt));
			sq.ranges.push_back({ 0, 0 });
			sq.files.emplace_back(0, ls.Path(f));
		}
	};
}
