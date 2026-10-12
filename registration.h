#pragma once

namespace iv
{
	// --register / --unregister: a ProgID per format under HKCU with its icon, listed in Open with; the default app stays the user's choice
	int RegisterFileTypes(bool install);
}
