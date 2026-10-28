#pragma once

#include "adjust.h"

namespace iv
{
	struct DisplayHistogram;

	enum class AdjustTool { Grade, Levels, Curves, Balance, HueSat, Count };
	const char* AdjustToolName(AdjustTool t);
	// Grade G (Nuke's key), Levels Ctrl+L, Curves Ctrl+M, Color Balance Ctrl+B, Hue/Saturation Ctrl+U (Photoshop's)
	const char* AdjustToolShortcut(AdjustTool t);

	// the floating adjustment windows; closing one keeps its adjustment, its Enabled box bypasses it
	class AdjustWindows
	{
	public:
		void Toggle(AdjustTool t) { m_open[(int)t] = !m_open[(int)t]; }
		bool IsOpen(AdjustTool t) const { return m_open[(int)t]; }
		void Close(AdjustTool t) { m_open[(int)t] = false; }
		// the window that had the keyboard focus last frame, or -1: Esc closes it instead of the app
		int Focused() const { return m_focused; }
		bool WantsHistogram() const { return IsOpen(AdjustTool::Levels) || IsOpen(AdjustTool::Curves); }
		// the open windows; true when a value changed
		bool Draw(ColorAdjust& a, const DisplayHistogram* histogram);
	private:
		bool GradeWindow(GradeParams& g);
		bool LevelsWindow(LevelsParams& l, const DisplayHistogram* h);
		bool CurvesWindow(CurvesParams& c, const DisplayHistogram* h);
		bool BalanceWindow(BalanceParams& b);
		bool HueSatWindow(HueSatParams& h);

		bool m_open[(int)AdjustTool::Count] = {};
		int m_focused = -1;
		bool m_gradeSplit[7] = {};   // a knob shown per channel
		int m_levelsChannel = 0;
		int m_levelsDrag = -1;   // 0 input black, 1 input white
		int m_curvesChannel = 0;
		int m_curveSelected = -1;
		bool m_curveDragging = false;
		int m_balanceRange = 1;
		int m_hueRange = 0;
	};
}
