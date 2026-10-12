#pragma once

#include "image_io.h"

namespace iv
{
	// OpenEXR: views probed through the C++ API; the decode runs on the core (C) API, chunks spread by par, each worker reusing one decode pipeline
	std::string ProbeExr(const std::string& path, ImageInfo& out);
	std::string DecodeExr(const std::wstring& path, const ViewDesc& view, const Layout& layout, uint8_t* dst, const Parallel& par);
}
