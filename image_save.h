#pragma once

#include "adjust.h"
#include "display.h"
#include "image_io.h"

#include <string>

namespace iv
{
	// lower-case extensions Save As writes, space separated
	const char* SaveExtensions();
	bool CanSave(const std::wstring& path);
	// exr, hdr and float tif get the scene-linear values (channel view, colour matrix and grade applied), the other formats the picture on screen (also exposure,
	// view transform and the display adjustments)
	std::string SaveImage(const std::wstring& path, const Layout& layout, const uint8_t* data, const DrawParams& p, const AdjustState& adjust, const Parallel& par);
}
