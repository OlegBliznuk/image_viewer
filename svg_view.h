#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iv
{
	// the visible part of an SVG rendered at screen resolution on its own thread; a newer request replaces one not yet started
	class SvgView
	{
	public:
		struct Request
		{
			std::wstring path;
			int imageW = 0;
			int imageH = 0;
			float x0 = 0.0f;   // image point at the raster's top-left
			float y0 = 0.0f;
			float zoom = 1.0f;   // raster pixels per image pixel
			int width = 0;
			int height = 0;
			bool operator==(const Request&) const = default;
		};

		struct Result
		{
			Request request;
			std::vector<uint8_t> rgba;
		};

		~SvgView();
		void Render(const Request& request);
		bool Take(Result& out);
	private:
		void Loop();

		std::thread m_thread;
		std::mutex m_mutex;
		std::condition_variable m_cv;
		Request m_pending;
		bool m_hasPending = false;
		Result m_done;
		bool m_hasDone = false;
		bool m_stop = false;
	};
}
