#include "adjust_ui.h"
#include "picture.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>

namespace iv
{
	namespace
	{
		constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
		const ImU32 kChannelColours[3] = { IM_COL32(235, 85, 85, 150), IM_COL32(85, 205, 85, 150), IM_COL32(95, 135, 245, 150) };

		// a value kept 0-1 (or -1..1) shown on the scale GIMP and Photoshop show (0-255, -100..100, degrees)
		bool ScaledSlider(const char* label, float* v, float scale, float lo, float hi, const char* format)
		{
			float s = *v * scale;
			if (!ImGui::SliderFloat(label, &s, lo, hi, format, ImGuiSliderFlags_AlwaysClamp))
				return false;
			*v = s / scale;
			return true;
		}

		bool ScaledDrag(const char* label, float* v, float scale, float lo, float hi, const char* format)
		{
			float s = *v * scale;
			if (!ImGui::DragFloat(label, &s, 0.5f, lo, hi, format, ImGuiSliderFlags_AlwaysClamp))
				return false;
			*v = s / scale;
			return true;
		}

		// one stage's bins, the three channels overlaid (channel 0) or one; scaled to the tallest bin away from the clipped ends
		void DrawHistogram(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const DisplayHistogram* h, int stage, int channel)
		{
			dl->AddRectFilled(p0, p1, ImGui::GetColorU32(ImGuiCol_FrameBg));
			if (!h || !h->samples)
				return;
			const int k0 = channel == 0 ? 0 : channel - 1, k1 = channel == 0 ? 3 : channel;
			uint32_t peak = 1;
			for (int k = k0; k < k1; ++k)
				for (int i = 1; i < 255; ++i)
					peak = std::max(peak, h->bins[stage][k][i]);
			const float w = (p1.x - p0.x) / 256.0f, height = p1.y - p0.y;
			for (int k = k0; k < k1; ++k)
				for (int i = 0; i < 256; ++i)
				{
					const float y = std::min(1.0f, (float)h->bins[stage][k][i] / (float)peak) * height;
					if (y > 0.0f)
						dl->AddRectFilled(ImVec2(p0.x + i * w, p1.y - y), ImVec2(p0.x + (i + 1) * w, p1.y), kChannelColours[k]);
				}
		}

		// Enabled box and Reset button of every window
		bool Header(bool& enabled, bool& reset)
		{
			const bool changed = ImGui::Checkbox("Enabled", &enabled);
			ImGui::SameLine();
			reset = ImGui::Button("Reset");
			return changed;
		}

		void Label(const char* text, float x)
		{
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(text);
			ImGui::SameLine(x);
		}
	}

	const char* AdjustToolName(AdjustTool t)
	{
		const char* names[] = { "Grade", "Levels", "Curves", "Color Balance", "Hue/Saturation" };
		return names[(int)t];
	}

	const char* AdjustToolShortcut(AdjustTool t)
	{
		const char* keys[] = { "G", "Ctrl+L", "Ctrl+M", "Ctrl+B", "Ctrl+U" };
		return keys[(int)t];
	}

	bool AdjustWindows::Draw(ColorAdjust& a, const DisplayHistogram* h)
	{
		bool changed = false;
		m_focused = -1;
		const ImGuiViewport* mv = ImGui::GetMainViewport();
		const float em = ImGui::GetFontSize();
		// first placement, in ems from the viewport's top right: Grade and Curves (the tall ones) in the outer column, the other three inside
		const ImVec2 place[(int)AdjustTool::Count] = { { 26.0f, 5.0f }, { 50.0f, 5.0f }, { 26.0f, 26.0f }, { 50.0f, 25.0f }, { 50.0f, 41.0f } };
		for (int t = 0; t < (int)AdjustTool::Count; ++t)
		{
			if (!m_open[t])
				continue;
			ImGui::SetNextWindowPos(ImVec2(mv->Pos.x + std::max(0.0f, mv->Size.x - em * place[t].x), mv->Pos.y + em * place[t].y), ImGuiCond_FirstUseEver);
			if (ImGui::Begin(AdjustToolName((AdjustTool)t), &m_open[t], kWindowFlags))
			{
				switch ((AdjustTool)t)
				{
				case AdjustTool::Grade: changed |= GradeWindow(a.grade); break;
				case AdjustTool::Levels: changed |= LevelsWindow(a.levels, h); break;
				case AdjustTool::Curves: changed |= CurvesWindow(a.curves, h); break;
				case AdjustTool::Balance: changed |= BalanceWindow(a.balance); break;
				case AdjustTool::HueSat: changed |= HueSatWindow(a.hueSat); break;
				default: break;
				}
				if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
					m_focused = t;
			}
			ImGui::End();
		}
		return changed;
	}

	// Nuke's knob names; "rgb" splits a knob into its channels, "1" joins them again (to the red value)
	bool AdjustWindows::GradeWindow(GradeParams& g)
	{
		bool reset = false;
		bool changed = Header(g.enabled, reset);
		if (reset)
		{
			const bool enabled = g.enabled;
			g = GradeParams();
			g.enabled = enabled;
			changed = true;
		}
		const float em = ImGui::GetFontSize();
		struct Knob { const char* name; float* v; };
		const Knob knobs[7] = { { "blackpoint", g.blackpoint }, { "whitepoint", g.whitepoint }, { "lift", g.lift }, { "gain", g.gain }, { "multiply", g.multiply }, { "offset", g.offset }, { "gamma", g.gamma } };
		for (int i = 0; i < 7; ++i)
		{
			ImGui::PushID(i);
			Label(knobs[i].name, em * 7.0f);
			ImGui::SetNextItemWidth(em * 15.0f);
			float* v = knobs[i].v;
			if (m_gradeSplit[i])
				changed |= ImGui::DragFloat3("##v", v, 0.005f, 0.0f, 0.0f, "%.3f");
			else if (ImGui::DragFloat("##v", &v[0], 0.005f, 0.0f, 0.0f, "%.3f"))
				v[1] = v[2] = v[0], changed = true;
			ImGui::SameLine();
			if (ImGui::SmallButton(m_gradeSplit[i] ? "1" : "rgb"))
			{
				m_gradeSplit[i] = !m_gradeSplit[i];
				if (!m_gradeSplit[i] && (v[1] != v[0] || v[2] != v[0]))
					v[1] = v[2] = v[0], changed = true;
			}
			ImGui::PopID();
		}
		changed |= ImGui::Checkbox("reverse", &g.reverse);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("black clamp", &g.blackClamp);
		ImGui::SameLine();
		changed |= ImGui::Checkbox("white clamp", &g.whiteClamp);
		Label("mix", em * 7.0f);
		ImGui::SetNextItemWidth(em * 15.0f);
		changed |= ImGui::SliderFloat("##mix", &g.mix, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		return changed;
	}

	// GIMP's levels on the displayed values; the histogram is what Levels gets, the triangles under it drag input black and white
	bool AdjustWindows::LevelsWindow(LevelsParams& l, const DisplayHistogram* h)
	{
		bool reset = false;
		bool changed = Header(l.enabled, reset);
		if (reset)
		{
			const bool enabled = l.enabled;
			l = LevelsParams();
			l.enabled = enabled;
			changed = true;
		}
		const float em = ImGui::GetFontSize(), w = em * 18.0f, step = 1.0f / 255.0f;
		const char* channels[] = { "RGB", "Red", "Green", "Blue" };
		ImGui::SetNextItemWidth(em * 7.0f);
		ImGui::Combo("Channel", &m_levelsChannel, channels, 4);
		LevelsChannel& c = l.ch[m_levelsChannel];
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p0 = ImGui::GetCursorScreenPos();
		DrawHistogram(dl, p0, ImVec2(p0.x + w, p0.y + em * 6.0f), h, 0, m_levelsChannel);
		ImGui::Dummy(ImVec2(w, em * 6.0f));
		const ImVec2 m0 = ImGui::GetCursorScreenPos();
		ImGui::InvisibleButton("##markers", ImVec2(w, em * 0.9f));
		const float mx = std::clamp((ImGui::GetIO().MousePos.x - m0.x) / w, 0.0f, 1.0f);
		if (ImGui::IsItemActivated())
			m_levelsDrag = std::abs(mx - c.inLow) <= std::abs(mx - c.inHigh) ? 0 : 1;
		if (ImGui::IsItemActive() && m_levelsDrag >= 0)
		{
			float& v = m_levelsDrag == 0 ? c.inLow : c.inHigh;
			const float nv = m_levelsDrag == 0 ? std::min(mx, c.inHigh - step) : std::max(mx, c.inLow + step);
			if (nv != v)
				v = nv, changed = true;
		}
		else
			m_levelsDrag = -1;
		auto marker = [&](float v, ImU32 fill)
		{
			const float x = m0.x + v * w;
			const ImVec2 a(x, m0.y), b(x - em * 0.4f, m0.y + em * 0.8f), d(x + em * 0.4f, m0.y + em * 0.8f);
			dl->AddTriangleFilled(a, b, d, fill);
			dl->AddTriangle(a, b, d, IM_COL32(128, 128, 128, 255));
		};
		marker(c.inLow, IM_COL32(20, 20, 20, 255));
		marker(c.inHigh, IM_COL32(240, 240, 240, 255));
		Label("Input", em * 4.5f);
		ImGui::SetNextItemWidth(em * 4.0f);
		changed |= ScaledDrag("##inLow", &c.inLow, 255.0f, 0.0f, c.inHigh * 255.0f - 1.0f, "%.0f");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(em * 4.0f);
		changed |= ImGui::DragFloat("##gamma", &c.gamma, 0.01f, 0.1f, 10.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		ImGui::SameLine();
		ImGui::SetNextItemWidth(em * 4.0f);
		changed |= ScaledDrag("##inHigh", &c.inHigh, 255.0f, c.inLow * 255.0f + 1.0f, 255.0f, "%.0f");
		Label("Output", em * 4.5f);
		ImGui::SetNextItemWidth(em * 4.0f);
		changed |= ScaledDrag("##outLow", &c.outLow, 255.0f, 0.0f, 255.0f, "%.0f");
		ImGui::SameLine(0.0f, em * 4.0f + ImGui::GetStyle().ItemSpacing.x * 2.0f);
		ImGui::SetNextItemWidth(em * 4.0f);
		changed |= ScaledDrag("##outHigh", &c.outHigh, 255.0f, 0.0f, 255.0f, "%.0f");
		if (ImGui::Button("Reset channel"))
			c = LevelsChannel(), changed = true;
		return changed;
	}

	// GIMP's curves; click adds a point (or picks the one near), drag moves it between its neighbours, right click deletes it
	bool AdjustWindows::CurvesWindow(CurvesParams& cv, const DisplayHistogram* h)
	{
		bool reset = false;
		bool changed = Header(cv.enabled, reset);
		if (reset)
		{
			const bool enabled = cv.enabled;
			cv = CurvesParams();
			cv.enabled = enabled;
			m_curveSelected = -1;
			changed = true;
		}
		const float em = ImGui::GetFontSize(), size = em * 16.0f, step = 1.0f / 255.0f;
		const char* channels[] = { "RGB", "Red", "Green", "Blue" };
		ImGui::SetNextItemWidth(em * 7.0f);
		if (ImGui::Combo("Channel", &m_curvesChannel, channels, 4))
			m_curveSelected = -1;
		std::vector<CurvePoint>& pts = cv.points[m_curvesChannel];
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + size, p0.y + size);
		ImGui::InvisibleButton("##curve", ImVec2(size, size), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		auto toCurve = [&](ImVec2 m) { return ImVec2(std::clamp((m.x - p0.x) / size, 0.0f, 1.0f), std::clamp(1.0f - (m.y - p0.y) / size, 0.0f, 1.0f)); };
		auto toScreen = [&](float x, float y) { return ImVec2(p0.x + x * size, p0.y + (1.0f - y) * size); };
		auto nearest = [&](ImVec2 m)
		{
			int best = -1;
			float bestD = em * em * 0.36f;
			for (int i = 0; i < (int)pts.size(); ++i)
			{
				const ImVec2 s = toScreen(pts[i].x, pts[i].y);
				const float d = (s.x - m.x) * (s.x - m.x) + (s.y - m.y) * (s.y - m.y);
				if (d < bestD)
					best = i, bestD = d;
			}
			return best;
		};
		const ImVec2 mouse = ImGui::GetIO().MousePos;
		if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
		{
			int i = nearest(mouse);
			if (i < 0)
			{
				const ImVec2 q = toCurve(mouse);
				const auto at = std::lower_bound(pts.begin(), pts.end(), q.x, [](const CurvePoint& p, float x) { return p.x < x; });
				// x stays unique: a click on a point's column takes that point
				if (at != pts.end() && at->x - q.x < 0.5f * step)
					i = (int)(at - pts.begin());
				else if (at != pts.begin() && q.x - (at - 1)->x < 0.5f * step)
					i = (int)(at - pts.begin()) - 1;
				else
				{
					i = (int)(at - pts.begin());
					pts.insert(at, CurvePoint{ q.x, q.y });
					changed = true;
				}
			}
			m_curveSelected = i;
			m_curveDragging = true;
		}
		if (m_curveDragging && ImGui::IsItemActive() && m_curveSelected >= 0 && m_curveSelected < (int)pts.size())
		{
			const ImVec2 q = toCurve(mouse);
			const int i = m_curveSelected;
			const float lo = i > 0 ? pts[i - 1].x + step : 0.0f, hi = i + 1 < (int)pts.size() ? pts[i + 1].x - step : 1.0f;
			const float nx = std::clamp(q.x, lo, std::max(lo, hi));
			if (nx != pts[i].x || q.y != pts[i].y)
				pts[i].x = nx, pts[i].y = q.y, changed = true;
		}
		if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
			m_curveDragging = false;
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && pts.size() > 2)
		{
			const int i = nearest(mouse);
			if (i >= 0)
				pts.erase(pts.begin() + i), m_curveSelected = -1, changed = true;
		}

		DrawHistogram(dl, p0, p1, h, 1, m_curvesChannel);
		const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Text, 0.15f);
		for (int q = 1; q < 4; ++q)
		{
			dl->AddLine(toScreen(q * 0.25f, 0.0f), toScreen(q * 0.25f, 1.0f), grid);
			dl->AddLine(toScreen(0.0f, q * 0.25f), toScreen(1.0f, q * 0.25f), grid);
		}
		dl->AddLine(toScreen(0.0f, 0.0f), toScreen(1.0f, 1.0f), grid);
		float samples[256];
		CurveSamples(pts, samples);
		ImVec2 line[256];
		for (int i = 0; i < 256; ++i)
			line[i] = toScreen(i / 255.0f, samples[i]);
		const ImU32 curveColour = m_curvesChannel == 0 ? IM_COL32(230, 230, 230, 255) : (kChannelColours[m_curvesChannel - 1] | IM_COL32(0, 0, 0, 255));
		dl->AddPolyline(line, 256, curveColour, ImDrawFlags_None, std::max(1.0f, em * 0.1f));
		const float r = em * 0.25f;
		for (int i = 0; i < (int)pts.size(); ++i)
		{
			const ImVec2 s = toScreen(pts[i].x, pts[i].y);
			if (i == m_curveSelected)
				dl->AddRectFilled(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), curveColour);
			else
				dl->AddRect(ImVec2(s.x - r, s.y - r), ImVec2(s.x + r, s.y + r), curveColour);
		}
		dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border));

		if (m_curveSelected >= 0 && m_curveSelected < (int)pts.size())
		{
			CurvePoint& p = pts[m_curveSelected];
			const int i = m_curveSelected;
			const float lo = i > 0 ? pts[i - 1].x + step : 0.0f, hi = i + 1 < (int)pts.size() ? pts[i + 1].x - step : 1.0f;
			Label("Input", em * 4.5f);
			ImGui::SetNextItemWidth(em * 4.0f);
			changed |= ScaledDrag("##px", &p.x, 255.0f, lo * 255.0f, std::max(lo, hi) * 255.0f, "%.1f");
			ImGui::SameLine();
			ImGui::TextUnformatted("Output");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(em * 4.0f);
			changed |= ScaledDrag("##py", &p.y, 255.0f, 0.0f, 255.0f, "%.1f");
			ImGui::SameLine();
			changed |= ImGui::Checkbox("Corner", &p.corner);
		}
		else
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size);
			ImGui::TextDisabled("click adds a point, drag moves it, right click deletes it");
			ImGui::PopTextWrapPos();
		}
		if (ImGui::Button("Reset channel"))
			pts = { { 0.0f, 0.0f }, { 1.0f, 1.0f } }, m_curveSelected = -1, changed = true;
		return changed;
	}

	// GIMP's colour balance: the three colour axes per tonal range
	bool AdjustWindows::BalanceWindow(BalanceParams& b)
	{
		bool reset = false;
		bool changed = Header(b.enabled, reset);
		if (reset)
		{
			const bool enabled = b.enabled;
			b = BalanceParams();
			b.enabled = enabled;
			changed = true;
		}
		const float em = ImGui::GetFontSize();
		ImGui::RadioButton("Shadows", &m_balanceRange, 0);
		ImGui::SameLine();
		ImGui::RadioButton("Midtones", &m_balanceRange, 1);
		ImGui::SameLine();
		ImGui::RadioButton("Highlights", &m_balanceRange, 2);
		const int r = m_balanceRange;
		const char* left[3] = { "Cyan", "Magenta", "Yellow" };
		const char* right[3] = { "Red", "Green", "Blue" };
		float* v[3] = { &b.cyanRed[r], &b.magentaGreen[r], &b.yellowBlue[r] };
		for (int k = 0; k < 3; ++k)
		{
			ImGui::PushID(k);
			Label(left[k], em * 4.5f);
			ImGui::SetNextItemWidth(em * 13.0f);
			changed |= ScaledSlider("##v", v[k], 100.0f, -100.0f, 100.0f, "%.0f");
			ImGui::SameLine();
			ImGui::TextUnformatted(right[k]);
			ImGui::PopID();
		}
		changed |= ImGui::Checkbox("Preserve luminosity", &b.preserveLuminosity);
		if (ImGui::Button("Reset range"))
			*v[0] = *v[1] = *v[2] = 0.0f, changed = true;
		return changed;
	}

	// GIMP's hue-saturation: master and six hue ranges, hue in degrees
	bool AdjustWindows::HueSatWindow(HueSatParams& h)
	{
		bool reset = false;
		bool changed = Header(h.enabled, reset);
		if (reset)
		{
			const bool enabled = h.enabled;
			h = HueSatParams();
			h.enabled = enabled;
			changed = true;
		}
		const float em = ImGui::GetFontSize();
		const char* ranges[7] = { "Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas" };
		ImGui::SetNextItemWidth(em * 8.0f);
		ImGui::Combo("Range", &m_hueRange, ranges, 7);
		const int r = m_hueRange;
		Label("Hue", em * 6.0f);
		ImGui::SetNextItemWidth(em * 13.0f);
		changed |= ScaledSlider("##hue", &h.hue[r], 180.0f, -180.0f, 180.0f, "%.0f");
		Label("Saturation", em * 6.0f);
		ImGui::SetNextItemWidth(em * 13.0f);
		changed |= ScaledSlider("##sat", &h.saturation[r], 100.0f, -100.0f, 100.0f, "%.0f");
		Label("Lightness", em * 6.0f);
		ImGui::SetNextItemWidth(em * 13.0f);
		changed |= ScaledSlider("##light", &h.lightness[r], 100.0f, -100.0f, 100.0f, "%.0f");
		Label("Overlap", em * 6.0f);
		ImGui::SetNextItemWidth(em * 13.0f);
		changed |= ScaledSlider("##overlap", &h.overlap, 100.0f, 0.0f, 100.0f, "%.0f");
		if (ImGui::Button("Reset range"))
			h.hue[r] = h.saturation[r] = h.lightness[r] = 0.0f, changed = true;
		return changed;
	}
}
