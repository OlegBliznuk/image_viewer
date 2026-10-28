#pragma once

#include "display.h"
#include "image_io.h"
#include "video_encoder.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace iv
{
	class Streamer;

	struct ExportJob
	{
		std::wstring path;
		std::vector<std::wstring> files;   // per frame, in order (a multi-frame file repeats its path)
		std::vector<int> indices;   // per frame: its entry in the streamer's list, which is also the frame number of a multi-frame file
		std::vector<double> durations;   // per frame, seconds
		FileKind kind = FileKind::Exr;
		ViewDesc view;
		Layout layout;
		bool multiFrame = false;
		DrawParams params;
		AdjustState adjust;
		int scale = 1;
		VideoSettings video;
	};

	// writes a video on its own thread: each frame copied from the streamer's cache while that still holds the generation the job was made from, else decoded,
	// then converted as on screen (picture.h) straight into the encoder's sample
	class VideoExport
	{
	public:
		struct Status
		{
			bool running = false;
			int done = 0;
			int total = 0;
			double seconds = 0.0;
			bool finished = false;   // a result waits for TakeResult
			bool cancelled = false;
			std::string error;
		};

		~VideoExport() { Stop(); }
		void Start(ExportJob job, Streamer* streamer, uint64_t generation, const Parallel& par);
		void Cancel() { m_cancel.Cancel(); }
		// cancels and waits for the thread
		void Stop();
		Status Get() const;
		bool Running() const;
		// running, or a result not taken yet
		bool Busy() const;
		// the result of a finished export, once
		bool TakeResult(Status& out);
	private:
		void Run(ExportJob job, Streamer* streamer, uint64_t generation, Parallel par);

		std::thread m_thread;
		mutable std::mutex m_mutex;
		Status m_status;
		CancelToken m_cancel;
	};
}
