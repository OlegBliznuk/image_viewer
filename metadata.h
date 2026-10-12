#pragma once

#include "image_io.h"

#include <string>
#include <vector>

namespace iv
{
	// one row of the metadata window; a section row names the group that follows
	struct MetaEntry
	{
		std::string key;
		std::string value;
		bool section = false;
	};

	// the file's own facts, then what its format records: EXR attributes, EXIF and GPS tags (JPEG, TIFF, HEIC, WebP, PNG, CR3), PNG text, ICC profile names, HDR / GIF / BMP / TGA / ICO / SVG headers
	std::vector<MetaEntry> ReadMetadata(const std::wstring& path, const ImageInfo& info);
}
