#pragma once

// startup timeline, compiled in only with -p:IvStartupTiming=1 (IV_STARTUP_TIMING)
#if defined(IV_STARTUP_TIMING)

#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace iv
{
	class StartupTiming
	{
	public:
		StartupTiming()
		{
			char buf[32] = {};
			if (GetEnvironmentVariableA("IV_LAUNCH_TIME", buf, sizeof(buf)) > 0)
				m_origin = _strtoui64(buf, nullptr, 10), m_fromLauncher = true;
			else
			{
				FILETIME c, e, k, u;
				GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
				m_origin = ((uint64_t)c.dwHighDateTime << 32) | c.dwLowDateTime;
			}
		}

		void Mark(const char* label)
		{
			FILETIME f;
			GetSystemTimePreciseAsFileTime(&f);
			const double ms = ((((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) - m_origin) / 10000.0;
			std::lock_guard lock(m_mutex);
			m_marks.push_back({ label, ms, GetCurrentThreadId() == m_mainThread });
		}

		void Print()
		{
			std::lock_guard lock(m_mutex);
			std::stable_sort(m_marks.begin(), m_marks.end(), [](const Entry& a, const Entry& b) { return a.ms < b.ms; });
			printf("[startup] origin: %s\n", m_fromLauncher ? "launcher timestamp" : "process creation time");
			double prev = 0.0;
			for (const Entry& m : m_marks)
			{
				printf("[startup] %9.2f ms  %+8.2f  %s%s\n", m.ms, m.main ? m.ms - prev : 0.0, m.main ? "" : "    [worker] ", m.label);
				if (m.main)
					prev = m.ms;
			}
			fflush(stdout);
		}

	private:
		struct Entry { const char* label; double ms; bool main; };
		std::mutex m_mutex;
		std::vector<Entry> m_marks;
		uint64_t m_origin = 0;
		DWORD m_mainThread = GetCurrentThreadId();
		bool m_fromLauncher = false;
	};
}

#define IV_MARK(timing, label) do { if (timing) (timing)->Mark(label); } while (0)
#else
#define IV_MARK(timing, label) do { } while (0)
#endif
