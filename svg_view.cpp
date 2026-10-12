#include "svg_view.h"
#include "image_formats.h"

#pragma warning(push)
#pragma warning(disable : 4251)
#include <lunasvg/lunasvg.h>
#pragma warning(pop)

#include <memory>

namespace iv
{
	SvgView::~SvgView()
	{
		{
			std::lock_guard lock(m_mutex);
			m_stop = true;
		}
		m_cv.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void SvgView::Render(const Request& request)
	{
		std::lock_guard lock(m_mutex);
		m_pending = request;
		m_hasPending = true;
		if (!m_thread.joinable())
			m_thread = std::thread(&SvgView::Loop, this);
		m_cv.notify_all();
	}

	bool SvgView::Take(Result& out)
	{
		std::lock_guard lock(m_mutex);
		if (!m_hasDone)
			return false;
		out = std::move(m_done);
		m_hasDone = false;
		return true;
	}

	void SvgView::Loop()
	{
		std::unique_ptr<lunasvg::Document> doc;
		std::wstring docPath;
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_cv.wait(lock, [&] { return m_stop || m_hasPending; });
			if (m_stop)
				return;
			const Request request = m_pending;
			m_hasPending = false;
			lock.unlock();
			if (request.path != docPath)
			{
				fmt::Bytes file;
				doc = fmt::ReadFile(request.path, file).empty() ? lunasvg::Document::loadFromData((const char*)file.data(), file.size()) : nullptr;
				docPath = request.path;
			}
			Result result;
			result.request = request;
			if (doc && doc->width() > 0.0f && doc->height() > 0.0f)
			{
				result.rgba.resize((size_t)request.width * request.height * 4);
				lunasvg::Bitmap bitmap(result.rgba.data(), request.width, request.height, request.width * 4);
				bitmap.clear(0xFFFFFFFF);
				const float sx = request.zoom * request.imageW / doc->width(), sy = request.zoom * request.imageH / doc->height();
				doc->render(bitmap, lunasvg::Matrix(sx, 0, 0, sy, -request.x0 * request.zoom, -request.y0 * request.zoom));
				bitmap.convertToRGBA();
			}
			lock.lock();
			m_done = std::move(result);
			m_hasDone = true;
		}
	}
}
