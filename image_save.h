#pragma once

#include "display.h"
#include "image_io.h"

#include <string>

namespace iv
{
	// lower-case extensions Save As writes, space separated
	const char* SaveExtensions();
	bool CanSave(const std::wstring& path);
	// exr, hdr and float tif get the scene-linear values (channel view and colour matrix applied), the other formats the picture on screen (also exposure and view transform)
	std::string SaveImage(const std::wstring& path, const Layout& layout, const uint8_t* data, const DrawParams& p, const Parallel& par);
}
