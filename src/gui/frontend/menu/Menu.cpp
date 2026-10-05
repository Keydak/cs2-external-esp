#include "Menu.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/features/Movement.hpp"
#include "core/features/Subtick.hpp"
#include "core/features/View.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Skins.hpp"
#include "core/features/GameRadar.hpp"
#include "core/features/ClanTag.hpp"
#include "core/features/Visuals.hpp"
#include "assets/fonts/WeaponIcons.h"
#include "assets/images/Logo.h"
#include "assets/models/PlayerModels.h"
#include "assets/models/PlayerPreviews.h"
#include "gui/frontend/images/ImageCache.hpp"
#include "core/engine/classes/Weapon.hpp"
#include "gui/frontend/esp/Esp.hpp"
#include "gui/frontend/esp/GameCrosshair.hpp"
#include "gui/frontend/overlays/Overlays.hpp"
#include "gui/renderer/Renderer.hpp" // Circular dependency
#include "gui/renderer/window/Window.hpp" // Circular dependency

#include "config/Config.hpp"

#include <imgui_internal.h>
#include <shellapi.h>

#include <numbers>


namespace {
	// Palette
	namespace col {
		// Neutral greys, no tint, so the single accent carries all the color
		constexpr ImU32 window = IM_COL32(17, 17, 19, 255);
		constexpr ImU32 sidebar = IM_COL32(13, 13, 15, 255);
		constexpr ImU32 panel = IM_COL32(22, 22, 25, 255);
		constexpr ImU32 selected = IM_COL32(32, 32, 36, 255);
		constexpr ImU32 border = IM_COL32(37, 37, 41, 255);
		constexpr ImU32 border_hover = IM_COL32(60, 60, 66, 255);
		constexpr ImU32 hover = IM_COL32(255, 255, 255, 7);
		constexpr ImU32 online = IM_COL32(126, 204, 142, 255);
		constexpr ImU32 track = IM_COL32(44, 44, 49, 255);
		constexpr ImU32 track_hover = IM_COL32(56, 56, 62, 255);
		constexpr ImU32 text = IM_COL32(237, 237, 239, 255);
		constexpr ImU32 text_label = IM_COL32(200, 200, 205, 255);
		constexpr ImU32 text_dim = IM_COL32(140, 140, 148, 255);
		constexpr ImU32 text_faint = IM_COL32(90, 90, 98, 255);
		constexpr ImU32 on_accent = IM_COL32(18, 18, 20, 255); // Text on accent fills
		constexpr ImU32 knob = IM_COL32(255, 255, 255, 255);
		constexpr ImU32 shadow = IM_COL32(0, 0, 0, 255);

		// From the config, refreshed every frame
		inline ImU32 accent = IM_COL32(255, 112, 67, 255);
	}

	// Layout, at a scale of 1. SetScale() fills in the scaled values below every frame
	float ui_scale = 1.f;

	float S(float value) {
		return value * ui_scale;
	}

	ImVec2 S(ImVec2 value) {
		return value * ui_scale;
	}

	constexpr ImVec2 BASE_MENU_SIZE = ImVec2(820.f, 540.f);

	ImVec2 MENU_SIZE;
	float WINDOW_ROUNDING;
	float SIDEBAR_WIDTH;
	float HEADER_HEIGHT;
	float CONTENT_PADDING;
	float COLUMN_SPACING;
	float ROW_HEIGHT;
	float PANEL_ROUNDING;

	ImVec2 TOGGLE_SIZE;
	float SWATCH_SIZE;
	float SLIDER_WIDTH;
	float SLIDER_VALUE_WIDTH;
	float WIDGET_SPACING;

	float TAB_SLIDE; // Pixels the content slides in from

	// Animation
	constexpr float MENU_OPEN_SPEED = 6.f;    // 1 / seconds
	constexpr float MENU_CLOSE_SPEED = 9.f;
	constexpr float TAB_SWITCH_SPEED = 4.5f;
	constexpr float RIPPLE_DURATION = 0.45f;

	void SetScale(float scale) {
		ui_scale = scale;

		MENU_SIZE = S(BASE_MENU_SIZE);
		WINDOW_ROUNDING = S(14.f);
		SIDEBAR_WIDTH = S(200.f);
		HEADER_HEIGHT = S(68.f);
		CONTENT_PADDING = S(20.f);
		COLUMN_SPACING = S(16.f);
		ROW_HEIGHT = S(34.f);
		PANEL_ROUNDING = S(10.f);

		TOGGLE_SIZE = S(ImVec2(38.f, 20.f));
		SWATCH_SIZE = S(18.f);
		SLIDER_WIDTH = S(116.f);
		SLIDER_VALUE_WIDTH = S(54.f);
		WIDGET_SPACING = S(8.f);

		TAB_SLIDE = S(14.f);
	}

#ifdef _DEBUG
	constexpr auto BRAND_TITLE = "Cs2 External";
	constexpr auto BRAND_SUBTITLE = "Counter-Strike 2 [DEV]";
#else
	constexpr auto BRAND_TITLE = "Cs2 External";
	constexpr auto BRAND_SUBTITLE = "Counter-Strike 2";
#endif

	ImFont* font_regular = nullptr;
	ImFont* font_bold = nullptr;

	struct ColumnLayout {
		float x, y, w, h;
	} column_layout{};

	// Applies the current style alpha, so fades & BeginDisabled() also reach custom drawings
	ImU32 C(ImU32 color, float alpha = 1.f) {
		return ImGui::GetColorU32(color, alpha);
	}

	ImU32 LerpColor(ImU32 a, ImU32 b, float t) {
		auto va = ImGui::ColorConvertU32ToFloat4(a);
		auto vb = ImGui::ColorConvertU32ToFloat4(b);

		return ImGui::GetColorU32(ImVec4(
			va.x + (vb.x - va.x) * t,
			va.y + (vb.y - va.y) * t,
			va.z + (vb.z - va.z) * t,
			va.w + (vb.w - va.w) * t
		));
	}

	float EaseOutCubic(float t) {
		t = 1.f - std::clamp(t, 0.f, 1.f);
		return 1.f - t * t * t;
	}

	// Smoothly moves a stored value towards target, keyed by the item id
	float Animate(ImGuiID id, float target, float speed = 14.f) {
		auto storage = ImGui::GetStateStorage();

		float value = storage->GetFloat(id, target);
		value += (target - value) * std::min(1.f, ImGui::GetIO().DeltaTime * speed);
		storage->SetFloat(id, value);

		return value;
	}

	// Soft glow around a rect, layers of growing translucent rects
	void Glow(ImDrawList* d, ImVec2 min, ImVec2 max, ImU32 color, float rounding, float size, float strength) {
		constexpr int layers = 6;

		for (int i = layers; i >= 1; i--) {
			float f = static_cast<float>(i) / layers;
			float grow = size * f;
			d->AddRectFilled(min - ImVec2(grow, grow), max + ImVec2(grow, grow), C(color, strength * (1.f - f) * 0.5f), rounding + grow);
		}
	}

	// Columns & panels

	void BeginColumn(int index) {
		ImGui::SetCursorPos(ImVec2(
			column_layout.x + index * (column_layout.w + COLUMN_SPACING),
			column_layout.y
		));

		ImGui::BeginChild(
			index == 0 ? "##column_left" : "##column_right",
			ImVec2(column_layout.w, column_layout.h),
			ImGuiChildFlags_None,
			ImGuiWindowFlags_NoBackground
		);
	}

	void EndColumn() {
		ImGui::EndChild();
	}

	void BeginPanel(const char* title) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, col::panel);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, PANEL_ROUNDING);
		ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0.f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14.f), 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.f), 0.f));

		ImGui::BeginChild(
			title,
			ImVec2(0, 0),
			ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
		);

		// Title bar: accent mark, title and a faint divider
		auto d = ImGui::GetWindowDrawList();
		auto window_pos = ImGui::GetWindowPos();
		auto window_width = ImGui::GetWindowWidth();
		auto start = ImGui::GetCursorScreenPos();

		d->AddRectFilled(start + ImVec2(0.f, S(14.f)), start + ImVec2(S(2.f), S(26.f)), C(col::accent), 1.f);
		d->AddText(font_bold, S(12.f), start + ImVec2(S(11.f), S(13.f)), C(col::text_dim), title);
		d->AddLine(ImVec2(window_pos.x, start.y + S(39.f)), ImVec2(window_pos.x + window_width, start.y + S(39.f)), C(col::border, 0.7f));

		ImGui::Dummy(ImVec2(0, S(44.f)));
	}

	void EndPanel() {
		ImGui::Dummy(ImVec2(0, S(8.f)));
		ImGui::EndChild();

		ImGui::PopStyleVar(4);
		ImGui::PopStyleColor();

		// Border lights up while hovered
		auto min = ImGui::GetItemRectMin();
		auto max = ImGui::GetItemRectMax();
		bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseHoveringRect(min, max);
		float h = Animate(ImGui::GetItemID() + 1, hovered ? 1.f : 0.f, 10.f);

		ImGui::GetWindowDrawList()->AddRect(min, max, LerpColor(col::border, col::border_hover, h), PANEL_ROUNDING, 0, 1.f);

		ImGui::Dummy(ImVec2(0, S(14.f)));
	}

	// Rows, label on the left and widgets aligned to the right

	struct Row {
		ImVec2 start;
		float width;
		float right; // Where the next right-aligned widget ends
	};

	Row BeginRow(const char* label, const char* tooltip = nullptr) {
		Row row{};
		row.start = ImGui::GetCursorScreenPos();
		row.width = ImGui::GetContentRegionAvail().x;
		row.right = row.start.x + row.width;

		auto d = ImGui::GetWindowDrawList();

		// Hover highlight
		auto highlight_min = row.start + ImVec2(-S(8.f), 1.f);
		auto highlight_max = row.start + ImVec2(row.width + S(8.f), ROW_HEIGHT - 1.f);
		bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(highlight_min, highlight_max);
		float h = Animate(ImGui::GetID("##row_hover"), hovered ? 1.f : 0.f, 16.f);

		if (h > 0.01f)
			d->AddRectFilled(highlight_min, highlight_max, C(col::hover, h), S(7.f));

		// What follows ## is only the id, like everywhere in ImGui
		auto label_end = ImGui::FindRenderedTextEnd(label);
		auto text_size = ImGui::CalcTextSize(label, label_end);
		auto text_pos = ImVec2(row.start.x + h * 2.f, row.start.y + (ROW_HEIGHT - text_size.y) * 0.5f);

		d->AddText(text_pos, LerpColor(col::text_label, col::text, h), label, label_end);

		if (tooltip) {
			// Small hint so users know there is more info
			auto hint_center = ImVec2(text_pos.x + text_size.x + S(10.f), row.start.y + ROW_HEIGHT * 0.5f);
			auto hint_max = ImVec2(hint_center.x + S(7.f), row.start.y + ROW_HEIGHT);
			bool hint_hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(row.start, hint_max);
			float hh = Animate(ImGui::GetID("##hint_hover"), hint_hovered ? 1.f : 0.f, 16.f);

			d->AddCircleFilled(hint_center, S(6.f), LerpColor(col::track, col::accent, hh), 16);
			auto mark_size = font_bold->CalcTextSizeA(S(10.f), FLT_MAX, 0.f, "?");
			d->AddText(font_bold, S(10.f), hint_center - mark_size * 0.5f, LerpColor(col::text_dim, col::text, hh), "?");

			if (hint_hovered)
				ImGui::SetTooltip("%s", tooltip);
		}

		return row;
	}

	// Reserves a widget slot at the right edge of the row, returns its top-left corner
	ImVec2 RowSlot(Row& row, ImVec2 size) {
		row.right -= size.x;

		auto pos = ImVec2(row.right, row.start.y + (ROW_HEIGHT - size.y) * 0.5f);
		row.right -= WIDGET_SPACING;

		return pos;
	}

	void EndRow(const Row& row) {
		ImGui::SetCursorScreenPos(row.start);
		ImGui::Dummy(ImVec2(row.width, ROW_HEIGHT));
	}

	// Widgets

	bool ToggleSwitch(const char* id, bool* value, ImVec2 pos) {
		ImGui::SetCursorScreenPos(pos);

		bool pressed = ImGui::InvisibleButton(id, TOGGLE_SIZE);
		auto item = ImGui::GetItemID();
		auto storage = ImGui::GetStateStorage();

		if (pressed) {
			*value = !*value;
			storage->SetFloat(item + 2, static_cast<float>(ImGui::GetTime())); // Ripple start
		}

		bool hovered = ImGui::IsItemHovered();
		bool held = ImGui::IsItemActive();

		float t = EaseOutCubic(Animate(item, *value ? 1.f : 0.f, 10.f));
		float hover = Animate(item + 1, hovered ? 1.f : 0.f, 16.f);
		float squish = Animate(item + 3, held ? 1.f : 0.f, 20.f);

		auto d = ImGui::GetWindowDrawList();
		auto max = pos + TOGGLE_SIZE;
		float radius = TOGGLE_SIZE.y * 0.5f;

		// Track, fills with the accent when on
		d->AddRectFilled(pos, max, LerpColor(LerpColor(col::track, col::track_hover, hover), col::accent, t), radius);

		// Ripple when switched on
		float since = static_cast<float>(ImGui::GetTime()) - storage->GetFloat(item + 2, -100.f);
		if (*value && since < RIPPLE_DURATION) {
			float p = EaseOutCubic(since / RIPPLE_DURATION);
			float grow = p * 7.f;
			d->AddRect(pos - ImVec2(grow, grow), max + ImVec2(grow, grow), C(col::accent, (1.f - p) * 0.7f), radius + grow, 0, 1.5f);
		}

		// Knob, stretches while pressed
		float knob_radius = radius - S(3.f);
		float stretch = squish * 5.f;
		float travel = TOGGLE_SIZE.x - TOGGLE_SIZE.y - stretch;
		float x = pos.x + radius + t * travel;
		float y = pos.y + radius;

		d->AddRectFilled(ImVec2(x - knob_radius, y - knob_radius + 1.5f), ImVec2(x + knob_radius + stretch, y + knob_radius + 1.5f), C(col::shadow, 0.35f), knob_radius);
		d->AddRectFilled(ImVec2(x - knob_radius, y - knob_radius), ImVec2(x + knob_radius + stretch, y + knob_radius), C(col::knob), knob_radius);

		return pressed;
	}

	void ColorSwatch(const char* id, color_t* color, ImVec2 pos, const char* tooltip) {
		ImGui::SetCursorScreenPos(pos);

		if (ImGui::InvisibleButton(id, ImVec2(SWATCH_SIZE, SWATCH_SIZE)))
			ImGui::OpenPopup(id);

		bool hovered = ImGui::IsItemHovered();
		float h = Animate(ImGui::GetItemID(), hovered || ImGui::IsPopupOpen(id) ? 1.f : 0.f, 16.f);

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);

		auto d = ImGui::GetWindowDrawList();
		auto grow = ImVec2(h * 1.5f, h * 1.5f);
		auto min = pos - grow;
		auto max = pos + ImVec2(SWATCH_SIZE, SWATCH_SIZE) + grow;

		ImVec4 value = *color;
		value.w *= ImGui::GetStyle().Alpha;

		ImGui::RenderColorRectWithAlphaCheckerboard(d, min, max, ImGui::ColorConvertFloat4ToU32(value), 5.f, ImVec2(0, 0), 6.f);
		d->AddRect(min, max, LerpColor(col::border_hover, col::text, h * 0.6f), S(6.f), 0, 1.f);

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.f), S(10.f)));
		if (ImGui::BeginPopup(id)) {
			ImGui::SetNextItemWidth(S(200.f));
			ImGui::ColorPicker4(
				"##picker",
				color->data(),
				ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview |
				ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoLabel
			);
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
	}

	void ColorRow(const char* label, color_t* color, const char* tooltip = nullptr) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		ColorSwatch("##color", color, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), nullptr);
		EndRow(row);

		ImGui::PopID();
	}

	// Toggle row with optional team/enemy color swatches
	bool Toggle(const char* label, bool* value, const char* tooltip = nullptr, color_t* team = nullptr, color_t* enemy = nullptr) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		bool changed = ToggleSwitch("##toggle", value, RowSlot(row, TOGGLE_SIZE));

		ImGui::BeginDisabled(!*value);
		{
			// Placed right to left, so enemy goes first to end up as [team][enemy][toggle]
			if (enemy)
				ColorSwatch("##enemy", enemy, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), team ? "Enemy color" : "Color");
			if (team)
				ColorSwatch("##team", team, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), "Team color");
		}
		ImGui::EndDisabled();

		EndRow(row);
		ImGui::PopID();

		return changed;
	}

	// Label & value on one line, the slider over the whole width under them
	bool SliderRow(const char* label, float* value, float min, float max, const char* value_text, const char* tooltip = nullptr) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto d = ImGui::GetWindowDrawList();

		// Value box
		const float box_height = S(22.f);
		auto box_pos = RowSlot(row, ImVec2(SLIDER_VALUE_WIDTH, box_height));
		EndRow(row);

		// Track
		const float slider_height = S(20.f);
		const float knob_room = S(8.f); // The knob sticks out of the ends
		float slider_width = row.width - knob_room * 2.f;
		auto slider_pos = ImVec2(row.start.x + knob_room, ImGui::GetCursorScreenPos().y - S(4.f));

		ImGui::SetCursorScreenPos(slider_pos);
		ImGui::InvisibleButton("##slider", ImVec2(slider_width, slider_height));

		auto item = ImGui::GetItemID();
		bool active = ImGui::IsItemActive();
		bool changed = false;

		if (active) {
			float t = std::clamp((ImGui::GetIO().MousePos.x - slider_pos.x) / slider_width, 0.f, 1.f);
			float new_value = min + t * (max - min);

			if (new_value != *value) {
				*value = new_value;
				changed = true;
			}
		}

		float hover = Animate(item + 1, ImGui::IsItemHovered() || active ? 1.f : 0.f, 16.f);
		float drag = Animate(item + 2, active ? 1.f : 0.f, 14.f);
		float t = Animate(item + 3, std::clamp((*value - min) / (max - min), 0.f, 1.f), 22.f);

		float y = slider_pos.y + slider_height * 0.5f;
		float fill_x = slider_pos.x + t * slider_width;

		d->AddRectFilled(ImVec2(slider_pos.x, y - S(2.f)), ImVec2(slider_pos.x + slider_width, y + S(2.f)), C(col::track), S(2.f));

		if (fill_x > slider_pos.x + 1.f) {
			d->AddRectFilled(ImVec2(slider_pos.x, y - S(2.f)), ImVec2(fill_x, y + S(2.f)), C(col::accent), S(2.f));
		}

		// Knob, grows while hovered
		float radius = S(6.f) + hover * 1.5f;
		d->AddCircleFilled(ImVec2(fill_x, y + 1.5f), radius, C(col::shadow, 0.35f), 24);
		d->AddCircleFilled(ImVec2(fill_x, y), radius, C(col::knob), 24);
		d->AddCircleFilled(ImVec2(fill_x, y), S(2.5f) * hover, C(col::accent), 16);

		d->AddRectFilled(box_pos, box_pos + ImVec2(SLIDER_VALUE_WIDTH, box_height), LerpColor(col::track, col::track_hover, hover), S(6.f));

		auto text_size = ImGui::CalcTextSize(value_text);
		d->AddText(
			box_pos + ImVec2((SLIDER_VALUE_WIDTH - text_size.x) * 0.5f, (box_height - text_size.y) * 0.5f),
			C(col::text),
			value_text
		);

		// Value bubble above the knob while dragging
		if (drag > 0.01f) {
			auto fd = ImGui::GetForegroundDrawList();
			auto bubble_size = ImVec2(text_size.x + S(14.f), text_size.y + S(6.f));
			auto bubble_min = ImVec2(fill_x - bubble_size.x * 0.5f, y - S(16.f) - bubble_size.y - drag * 4.f);

			fd->AddRectFilled(bubble_min, bubble_min + bubble_size, C(col::accent, drag), S(6.f));
			fd->AddText(bubble_min + ImVec2(S(7.f), S(3.f)), C(col::on_accent, drag), value_text);
		}

		ImGui::SetCursorScreenPos(ImVec2(row.start.x, slider_pos.y));
		ImGui::Dummy(ImVec2(row.width, slider_height + S(4.f)));

		ImGui::PopID();
		return changed;
	}

	bool SliderFloat(const char* label, float* value, float min, float max, const char* format, const char* tooltip = nullptr) {
		char text[32];
		snprintf(text, sizeof(text), format, *value);

		return SliderRow(label, value, min, max, text, tooltip);
	}

	bool SliderInt(const char* label, int* value, int min, int max, const char* format, const char* tooltip = nullptr) {
		char text[32];
		snprintf(text, sizeof(text), format, *value);

		float f = static_cast<float>(*value);
		if (!SliderRow(label, &f, static_cast<float>(min), static_cast<float>(max), text, tooltip))
			return false;

		int rounded = static_cast<int>(std::round(f));
		if (rounded == *value)
			return false;

		*value = rounded;
		return true;
	}

	std::string KeyName(int key) {
		switch (key) {
		case VK_LBUTTON:	return "Mouse 1";
		case VK_RBUTTON:	return "Mouse 2";
		case VK_MBUTTON:	return "Mouse 3";
		case VK_XBUTTON1:	return "Mouse 4";
		case VK_XBUTTON2:	return "Mouse 5";
		}

		// Extended keys need the flag for the right name (arrows, insert, ...)
		UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC);
		LONG param = static_cast<LONG>(scan << 16);

		switch (key) {
		case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
		case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
		case VK_INSERT: case VK_DELETE: case VK_RCONTROL: case VK_RMENU:
			param |= 1 << 24;
			break;
		}

		char name[32]{};
		if (GetKeyNameTextA(param, name, sizeof(name)) > 0)
			return name;

		return std::format("Key {}", key);
	}

	// Click, then press the key to bind. Escape cancels
	bool KeybindRow(const char* label, int* key, const char* tooltip = nullptr) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto storage = ImGui::GetStateStorage();
		auto waiting_id = ImGui::GetID("##waiting");
		bool waiting = storage->GetBool(waiting_id);
		bool changed = false;

		auto text = waiting ? std::string("Press a key") : KeyName(*key);
		auto text_size = ImGui::CalcTextSize(text.c_str());
		auto box_size = ImVec2(std::max(S(70.f), text_size.x + S(20.f)), S(24.f));
		auto box_pos = RowSlot(row, box_size);

		ImGui::SetCursorScreenPos(box_pos);
		if (ImGui::InvisibleButton("##bind", box_size) && !waiting) {
			waiting = true;
			storage->SetBool(waiting_id, true);
		}

		float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || waiting ? 1.f : 0.f, 16.f);

		if (waiting) {
			// Skipping the left mouse button, it is the click that started the binding.
			// Generic modifiers are skipped too, their left & right versions follow
			for (int vk = VK_RBUTTON; vk <= 0xFE; vk++) {
				if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || !(GetAsyncKeyState(vk) & 0x8000))
					continue;

				if (vk != VK_ESCAPE) {
					*key = vk;
					changed = true;
				}

				storage->SetBool(waiting_id, false);
				break;
			}
		}

		auto d = ImGui::GetWindowDrawList();
		float pulse = waiting ? 0.5f + 0.5f * sinf(static_cast<float>(ImGui::GetTime()) * 6.f) : 0.f;

		d->AddRectFilled(box_pos, box_pos + box_size, LerpColor(col::track, col::track_hover, hover), S(6.f));
		if (waiting)
			d->AddRect(box_pos, box_pos + box_size, C(col::accent, 0.4f + 0.6f * pulse), S(6.f), 0, 1.f);

		d->AddText(
			box_pos + (box_size - text_size) * 0.5f,
			waiting ? LerpColor(col::text_dim, col::accent, pulse) : C(col::text),
			text.c_str()
		);

		EndRow(row);
		ImGui::PopID();

		return changed;
	}

	// Pick one of a few options, the selection slides between them
	bool SegmentRow(const char* label, int* value, std::initializer_list<const char*> items, const char* tooltip = nullptr) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto d = ImGui::GetWindowDrawList();

		const float padding = S(10.f);
		const float height = S(24.f);
		const float inset = S(2.f);

		float total = inset * 2.f;
		for (auto item : items)
			total += ImGui::CalcTextSize(item).x + padding * 2.f;

		auto box = RowSlot(row, ImVec2(total, height));
		d->AddRectFilled(box, box + ImVec2(total, height), C(col::track), S(7.f));

		// Selection first, the labels go on top
		float x = inset, selected_x = 0.f, selected_w = 0.f;
		int index = 0;

		for (auto item : items) {
			float w = ImGui::CalcTextSize(item).x + padding * 2.f;
			if (index == *value) {
				selected_x = x;
				selected_w = w;
			}

			x += w;
			index++;
		}

		float slide_x = Animate(ImGui::GetID("##selection_x"), selected_x, 16.f);
		float slide_w = Animate(ImGui::GetID("##selection_w"), selected_w, 16.f);

		d->AddRectFilled(box + ImVec2(slide_x, inset), box + ImVec2(slide_x + slide_w, height - inset), C(col::accent), S(5.f));

		bool changed = false;
		x = inset;
		index = 0;

		for (auto item : items) {
			auto text_size = ImGui::CalcTextSize(item);
			float w = text_size.x + padding * 2.f;
			auto min = box + ImVec2(x, 0.f);

			ImGui::PushID(index);
			ImGui::SetCursorScreenPos(min);

			if (ImGui::InvisibleButton("##segment", ImVec2(w, height)) && *value != index) {
				*value = index;
				changed = true;
			}

			float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);
			auto color = index == *value ? C(col::on_accent) : LerpColor(col::text_dim, col::text, hover);

			d->AddText(min + ImVec2(padding, (height - text_size.y) * 0.5f), color, item);

			ImGui::PopID();
			x += w;
			index++;
		}

		EndRow(row);
		ImGui::PopID();

		return changed;
	}

	void TextBlock(const char* text) {
		ImGui::Dummy(ImVec2(0, 6));
		ImGui::PushStyleColor(ImGuiCol_Text, col::text_dim);
		ImGui::PushTextWrapPos(0.f);
		ImGui::TextUnformatted(text);
		ImGui::PopTextWrapPos();
		ImGui::PopStyleColor();
		ImGui::Dummy(ImVec2(0, 6));
	}

	// Field at the right of the row, opens a list below it (above when there is no room), the description of an item
	// shows while hovering it. Its own popup: the combo of ImGui takes the zero padding & spacing of the panels
	bool DropdownRow(const char* label, int* value, const std::vector<const char*>& items, const char* const* descriptions = nullptr,
		const char* tooltip = nullptr, float width = S(150.f)) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto d = ImGui::GetWindowDrawList();

		const float height = S(26.f);
		const float item_height = S(28.f);
		const float padding = S(6.f);

		auto pos = RowSlot(row, ImVec2(width, height));
		ImGui::SetCursorScreenPos(pos);

		if (ImGui::InvisibleButton("##field", ImVec2(width, height)))
			ImGui::OpenPopup("##list");

		bool open = ImGui::IsPopupOpen("##list");
		float h = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || open ? 1.f : 0.f, 16.f);

		d->AddRectFilled(pos, pos + ImVec2(width, height), LerpColor(col::track, col::track_hover, h), S(7.f));
		if (open)
			d->AddRect(pos, pos + ImVec2(width, height), C(col::accent, 0.8f), S(7.f), 0, 1.f);

		int current = std::clamp(*value, 0, static_cast<int>(items.size()) - 1);
		auto text_size = ImGui::CalcTextSize(items[current]);
		d->PushClipRect(pos, pos + ImVec2(width - S(24.f), height), true);
		d->AddText(pos + ImVec2(S(10.f), (height - text_size.y) * 0.5f), C(col::text), items[current]);
		d->PopClipRect();

		// Chevron, points up while open
		auto center = pos + ImVec2(width - S(13.f), height * 0.5f);
		float s = S(3.5f), flip = open ? -1.f : 1.f;
		d->AddTriangleFilled(center + ImVec2(-s, -s * 0.5f * flip), center + ImVec2(s, -s * 0.5f * flip), center + ImVec2(0.f, s * 0.6f * flip),
			LerpColor(col::text_dim, open ? col::accent : col::text, h));

		// The list, all items when they fit on the screen, scrolls otherwise
		float list_height = items.size() * item_height + padding * 2.f;
		float display = ImGui::GetIO().DisplaySize.y;
		float below = display - (pos.y + height + S(4.f)) - S(8.f);
		float above = pos.y - S(4.f) - S(8.f);
		bool up = list_height > below && above > below;
		float max_height = std::max(item_height * 3.f, up ? above : below);
		float shown = std::min(list_height, max_height);

		ImGui::SetNextWindowPos(up ? ImVec2(pos.x, pos.y - S(4.f) - shown) : ImVec2(pos.x, pos.y + height + S(4.f)));
		ImGui::SetNextWindowSize(ImVec2(width, shown));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(4.f));

		bool changed = false;
		if (ImGui::BeginPopup("##list", ImGuiWindowFlags_NoMove)) {
			auto list = ImGui::GetWindowDrawList();
			float inner = ImGui::GetContentRegionAvail().x;

			for (int i = 0; i < static_cast<int>(items.size()); i++) {
				ImGui::PushID(i);
				auto min = ImGui::GetCursorScreenPos();

				if (ImGui::InvisibleButton("##item", ImVec2(inner, item_height))) {
					changed = *value != i;
					*value = i;
					ImGui::CloseCurrentPopup();
				}

				bool selected = i == current;
				float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);

				if (selected || hover > 0.01f)
					list->AddRectFilled(min, min + ImVec2(inner, item_height), selected ? C(col::accent, 0.16f) : C(col::hover, hover), S(6.f));
				if (selected)
					list->AddRectFilled(min + ImVec2(0.f, S(7.f)), min + ImVec2(S(2.5f), item_height - S(7.f)), C(col::accent), 1.f);

				auto size = ImGui::CalcTextSize(items[i]);
				list->AddText(min + ImVec2(S(10.f) + hover * 2.f, (item_height - size.y) * 0.5f),
					selected ? C(col::text) : LerpColor(col::text_dim, col::text, hover), items[i]);

				if (descriptions && ImGui::IsItemHovered())
					ImGui::SetTooltip("%s", descriptions[i]);

				ImGui::PopID();
			}

			// Opened at the selection
			if (ImGui::IsWindowAppearing())
				ImGui::SetScrollY(std::max(0.f, current * item_height - shown * 0.5f));

			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(4);

		EndRow(row);
		ImGui::PopID();
		return changed;
	}

	// Text input over the whole width of a panel
	void TextField(const char* id, const char* hint, char* buffer, size_t size, const char* tooltip = nullptr) {
		ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10.f), S(6.f)));
		ImGui::Dummy(ImVec2(0, S(4.f)));
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint(id, hint, buffer, size);
		if (tooltip && ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", tooltip);
		ImGui::Dummy(ImVec2(0, S(4.f)));
		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);
	}

	// Combo row, call EndComboRow() only when it returns true
	Row combo_row{};

	bool BeginComboRow(const char* label, const char* preview, const char* tooltip = nullptr, float width = S(150.f)) {
		ImGui::PushID(label);

		combo_row = BeginRow(label, tooltip);
		ImGui::SetCursorScreenPos(RowSlot(combo_row, ImVec2(width, ImGui::GetFrameHeight())));
		ImGui::SetNextItemWidth(width);

		ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
		ImGui::PushStyleColor(ImGuiCol_Button, col::track);
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::track_hover);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.f), S(4.f)));

		bool open = ImGui::BeginCombo("##combo", preview, ImGuiComboFlags_HeightLarge);

		ImGui::PopStyleColor(4);

		if (!open) {
			ImGui::PopStyleVar(2);
			EndRow(combo_row);
			ImGui::PopID();
		}

		return open;
	}

	void EndComboRow() {
		ImGui::EndCombo();
		ImGui::PopStyleVar(2);
		EndRow(combo_row);
		ImGui::PopID();
	}

	bool InputIntRow(const char* label, int* value, const char* tooltip = nullptr, float width = S(90.f)) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		ImGui::SetCursorScreenPos(RowSlot(row, ImVec2(width, ImGui::GetFrameHeight())));
		ImGui::SetNextItemWidth(width);

		ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));

		bool changed = ImGui::InputInt("##input", value, 0, 0);

		ImGui::PopStyleVar();
		ImGui::PopStyleColor(2);

		EndRow(row);
		ImGui::PopID();

		return changed;
	}

	void SearchBox(const char* id, char* buffer, size_t size) {
		ImGui::Dummy(ImVec2(0, 4));

		ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10.f), S(6.f)));

		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint(id, "Search...", buffer, size);

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);

		ImGui::Dummy(ImVec2(0, 6));
	}

	// Selectable list entry with an optional dimmed text on the right
	bool ListItem(const char* label, const char* right_text, bool selected) {
		ImGui::PushID(label);

		auto pos = ImGui::GetCursorScreenPos();
		auto size = ImVec2(ImGui::GetContentRegionAvail().x, S(28.f));

		bool pressed = ImGui::InvisibleButton("##item", size);
		float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() && !selected ? 1.f : 0.f, 16.f);
		float active = Animate(ImGui::GetItemID() + 1, selected ? 1.f : 0.f, 14.f);

		auto d = ImGui::GetWindowDrawList();

		if (active > 0.01f) {
			d->AddRectFilled(pos, pos + size, C(col::selected, active), S(6.f));
			d->AddRectFilled(pos + ImVec2(0.f, S(8.f)), ImVec2(pos.x + S(2.f), pos.y + size.y - S(8.f)), C(col::accent, active), 1.f);
		}
		if (hover > 0.01f)
			d->AddRectFilled(pos, pos + size, C(col::hover, hover), S(6.f));

		auto text_y = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
		d->AddText(ImVec2(pos.x + S(10.f) + active * 2.f, text_y), LerpColor(col::text_label, col::text, std::max(hover, active)), label);

		if (right_text && *right_text) {
			auto right_size = ImGui::CalcTextSize(right_text);
			d->AddText(ImVec2(pos.x + size.x - right_size.x - S(10.f), text_y), LerpColor(col::text_dim, col::accent, active), right_text);
		}

		ImGui::PopID();
		return pressed;
	}

	// Small button, filled with the accent for the main action
	bool PillButton(const char* label, ImVec2 pos, ImVec2 size, bool accent = false, const char* tooltip = nullptr) {
		ImGui::SetCursorScreenPos(pos);
		bool pressed = ImGui::InvisibleButton(label, size);
		bool hovered = ImGui::IsItemHovered();
		float hover = Animate(ImGui::GetItemID(), hovered ? 1.f : 0.f, 16.f);

		auto d = ImGui::GetWindowDrawList();
		if (accent)
			d->AddRectFilled(pos, pos + size, C(col::accent, 0.8f + 0.2f * hover), S(6.f));
		else {
			d->AddRectFilled(pos, pos + size, C(LerpColor(col::track, col::track_hover, hover)), S(6.f));
			d->AddRect(pos, pos + size, C(LerpColor(col::border, col::border_hover, hover)), S(6.f));
		}

		auto text = ImGui::FindRenderedTextEnd(label);
		auto text_size = ImGui::CalcTextSize(label, text);
		d->AddText(pos + (size - text_size) * 0.5f, accent ? C(col::on_accent) : C(LerpColor(col::text_dim, col::text, hover)), label, text);

		if (tooltip && hovered)
			ImGui::SetTooltip("%s", tooltip);

		return pressed;
	}

	// Exported configs & skin loadouts: a name to export with & the saved ones, picked one by one for the
	// buttons under the list. What is clicked runs at the start of the next frame, the skin tab holds the
	// skin lock while drawing
	struct PresetState {
		enum class Op { NONE, SAVE, LOAD, RENAME, REMOVE, DEFAULT };

		char name[64]{};
		std::vector<std::string> names;
		std::string default_name;
		std::chrono::steady_clock::time_point listed{};

		std::string selected;
		bool renaming = false;
		char rename[64]{};
		bool confirm_delete = false;

		Op op = Op::NONE;
		std::string target, to;

		std::string message;
		bool error = false;
		std::chrono::steady_clock::time_point message_at{};
	};

	PresetState config_presets, skin_presets;

	// Runs what was clicked, true when one was imported
	bool ProcessPreset(Config::Preset kind, PresetState& state) {
		if (state.op == PresetState::Op::NONE)
			return false;

		std::string error, done;
		bool imported = false;

		switch (state.op) {
		case PresetState::Op::SAVE:
			error = Config::SavePreset(kind, state.target);
			done = "Exported \"" + Config::CleanPresetName(state.target) + "\"";
			if (error.empty())
				state.selected = Config::CleanPresetName(state.target);
			break;
		case PresetState::Op::LOAD:
			error = Config::LoadPreset(kind, state.target);
			done = "Imported \"" + state.target + "\"";
			imported = error.empty();
			break;
		case PresetState::Op::RENAME:
			error = Config::RenamePreset(kind, state.target, state.to);
			done = "Renamed to \"" + Config::CleanPresetName(state.to) + "\"";
			if (error.empty())
				state.selected = Config::CleanPresetName(state.to);
			break;
		case PresetState::Op::REMOVE:
			error = Config::DeletePreset(kind, state.target);
			done = "Deleted \"" + state.target + "\"";
			if (error.empty())
				state.selected.clear();
			break;
		case PresetState::Op::DEFAULT: {
			bool unset = Config::GetDefaultPreset(kind) == state.target;
			Config::SetDefaultPreset(kind, unset ? "" : state.target);
			done = unset ? "No default anymore" : "\"" + state.target + "\" is imported on every start";
			break;
		}
		default:
			break;
		}

		state.op = PresetState::Op::NONE;
		state.error = !error.empty();
		state.message = state.error ? error : done;
		state.message_at = std::chrono::steady_clock::now();
		state.listed = {};

		return imported;
	}

	void PresetList(Config::Preset kind, PresetState& state) {
		auto now = std::chrono::steady_clock::now();

		// Files can be added or removed by hand too
		if (now - state.listed > std::chrono::seconds(1)) {
			state.names = Config::ListPresets(kind);
			state.default_name = Config::GetDefaultPreset(kind);
			state.listed = now;

			if (std::find(state.names.begin(), state.names.end(), state.selected) == state.names.end()) {
				state.selected.clear();
				state.renaming = false;
			}
		}

		auto d = ImGui::GetWindowDrawList();
		const float width = ImGui::GetContentRegionAvail().x;
		const float button_height = S(26.f);
		const float gap = S(6.f);

		ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(6.f));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10.f), (button_height - ImGui::GetTextLineHeight()) * 0.5f));

		// Name & export
		{
			ImGui::Dummy(ImVec2(0, S(4.f)));
			auto start = ImGui::GetCursorScreenPos();
			const float export_width = S(76.f);

			ImGui::SetNextItemWidth(width - export_width - gap);
			bool enter = ImGui::InputTextWithHint("##name", kind == Config::Preset::SKINS ? "Loadout name..." : "Config name...",
				state.name, sizeof(state.name), ImGuiInputTextFlags_EnterReturnsTrue);

			if (PillButton("Export", ImVec2(start.x + width - export_width, start.y), ImVec2(export_width, button_height), true,
				"Saves the current settings under this name, one with the same name is overwritten") || enter) {
				state.op = PresetState::Op::SAVE;
				state.target = state.name;
				state.name[0] = 0;
			}

			ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + button_height));
			ImGui::Dummy(ImVec2(width, S(8.f)));
		}

		// Saved ones, the whole width for the name
		if (state.names.empty())
			TextBlock("Nothing exported yet");

		for (const auto& name : state.names) {
			bool selected = state.selected == name;
			if (ListItem(name.c_str(), name == state.default_name ? "DEFAULT" : "", selected)) {
				state.selected = selected ? "" : name;
				state.renaming = false;
				state.confirm_delete = false;
			}
		}

		// What to do with the picked one
		if (!state.selected.empty()) {
			ImGui::Dummy(ImVec2(0, S(6.f)));
			auto bar = ImGui::GetCursorScreenPos();

			if (state.renaming) {
				const float ok_width = S(44.f), cancel_width = S(60.f);

				ImGui::SetCursorScreenPos(bar);
				ImGui::SetNextItemWidth(width - ok_width - cancel_width - gap * 2.f);
				if (!ImGui::IsAnyItemActive())
					ImGui::SetKeyboardFocusHere();
				bool enter = ImGui::InputText("##rename", state.rename, sizeof(state.rename), ImGuiInputTextFlags_EnterReturnsTrue);

				if (PillButton("OK", ImVec2(bar.x + width - cancel_width - gap - ok_width, bar.y), ImVec2(ok_width, button_height), true) || enter) {
					state.op = PresetState::Op::RENAME;
					state.target = state.selected;
					state.to = state.rename;
					state.renaming = false;
				}
				if (PillButton("Cancel", ImVec2(bar.x + width - cancel_width, bar.y), ImVec2(cancel_width, button_height)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
					state.renaming = false;

				ImGui::SetCursorScreenPos(bar);
				ImGui::Dummy(ImVec2(width, button_height));
			}
			else {
				// Two rows: import & overwrite, then the rest
				float half = (width - gap) * 0.5f;
				float third = (width - gap * 2.f) / 3.f;
				bool is_default = state.selected == state.default_name;

				if (PillButton("Load", bar, ImVec2(half, button_height), true, "Imports it into the current settings")) {
					state.op = PresetState::Op::LOAD;
					state.target = state.selected;
				}
				if (PillButton("Save", ImVec2(bar.x + half + gap, bar.y), ImVec2(half, button_height), false, "Overwrites it with the current settings")) {
					state.op = PresetState::Op::SAVE;
					state.target = state.selected;
				}

				auto second = ImVec2(bar.x, bar.y + button_height + gap);

				if (PillButton("Rename", second, ImVec2(third, button_height))) {
					state.renaming = true;
					state.confirm_delete = false;
					snprintf(state.rename, sizeof(state.rename), "%s", state.selected.c_str());
				}
				if (PillButton(is_default ? "Default##unset" : "Set Default", ImVec2(second.x + third + gap, second.y), ImVec2(third, button_height), is_default,
					is_default ? "Imported on every start, click to stop that" : "Imports it on every start of the program")) {
					state.op = PresetState::Op::DEFAULT;
					state.target = state.selected;
				}
				if (PillButton(state.confirm_delete ? "Sure?##delete" : "Delete##delete", ImVec2(second.x + (third + gap) * 2.f, second.y), ImVec2(third, button_height),
					state.confirm_delete, state.confirm_delete ? "Click again to delete the file" : nullptr)) {
					if (state.confirm_delete) {
						state.op = PresetState::Op::REMOVE;
						state.target = state.selected;
					}
					state.confirm_delete = !state.confirm_delete;
				}

				ImGui::SetCursorScreenPos(bar);
				ImGui::Dummy(ImVec2(width, button_height * 2.f + gap));
			}
		}

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(2);

		// Folder & what the last click did
		ImGui::Dummy(ImVec2(0, S(10.f)));
		auto footer = ImGui::GetCursorScreenPos();
		const float folder_width = S(96.f);

		if (PillButton("Open Folder", footer, ImVec2(folder_width, button_height), false,
			"Files put in this folder by hand show up in the list too, like ones from a friend")) {
			std::error_code error;
			std::filesystem::create_directories(Config::PresetFolder(kind), error);
			auto folder = std::filesystem::absolute(Config::PresetFolder(kind), error);
			ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		float age = std::chrono::duration<float>(now - state.message_at).count();
		if (!state.message.empty() && age < 5.f) {
			float alpha = std::clamp(5.f - age, 0.f, 1.f);
			auto text_pos = ImVec2(footer.x + folder_width + S(10.f), footer.y + (button_height - ImGui::GetTextLineHeight()) * 0.5f);

			d->PushClipRect(text_pos, ImVec2(footer.x + width, footer.y + button_height), true);
			d->AddText(text_pos, C(state.error ? IM_COL32(235, 90, 80, 255) : col::text_dim, alpha), state.message.c_str());
			d->PopClipRect();
		}

		ImGui::SetCursorScreenPos(footer);
		ImGui::Dummy(ImVec2(width, button_height));
	}

	bool ContainsInsensitive(const std::string& haystack, const char* needle) {
		if (!needle || !*needle)
			return true;

		auto it = std::search(
			haystack.begin(), haystack.end(),
			needle, needle + strlen(needle),
			[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }
		);

		return it != haystack.end();
	}

	std::string ToUpper(std::string str) {
		std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		return str;
	}

	// Skin cards

	// Steam serves smaller versions of item pictures
	std::string SmallImage(const std::string& url) {
		return url.empty() ? url : url + "/256fx192f";
	}

	ImU32 RarityColor(uint32_t rgb) {
		if (!rgb)
			return col::border_hover;

		return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
	}

	#define CARD_GAP S(10.f)
	#define CARD_MIN_WIDTH S(128.f)
	#define CARD_HEIGHT S(128.f)

	// Picture, name, a second line & a stripe in the rarity color
	bool ItemCard(const char* id, ImVec2 size, const std::string& image, const char* icon, const char* title, const char* subtitle, ImU32 rarity, bool selected, bool marked = false) {
		ImGui::PushID(id);

		auto pos = ImGui::GetCursorScreenPos();
		bool pressed = ImGui::InvisibleButton("##card", size);
		auto item = ImGui::GetItemID();

		float hover = Animate(item, ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
		float active = Animate(item + 1, selected ? 1.f : 0.f, 12.f);

		auto d = ImGui::GetWindowDrawList();
		auto min = pos - ImVec2(0.f, S(2.f) * hover);
		auto max = min + size;

		d->AddRectFilled(min, max, LerpColor(col::panel, col::selected, std::max(hover * 0.5f, active)), S(10.f));

		// Picture area with a soft glow in the rarity color
		auto picture_min = min + ImVec2(S(10.f), S(10.f));
		auto picture_max = ImVec2(max.x - S(10.f), min.y + size.y - S(50.f));
		auto center = (picture_min + picture_max) * 0.5f;

		d->AddCircleFilled(center, (picture_max.y - picture_min.y) * 0.5f, C(rarity, 0.08f + 0.10f * hover), 40);

		auto texture = image.empty() ? ImTextureID{} : ImageCache::Get(image);
		if (texture) {
			// Fit, keeping the 4:3 of the pictures
			float w = picture_max.x - picture_min.x, h = picture_max.y - picture_min.y;
			float fit_w = std::min(w, h * 4.f / 3.f) * (1.f + 0.04f * hover);
			auto fit = ImVec2(fit_w, fit_w * 3.f / 4.f);

			d->AddImage(texture, center - fit * 0.5f, center + fit * 0.5f, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE));
		}
		else if (icon) {
			const float icon_size = S(36.f);
			auto icon_extent = font_regular->CalcTextSizeA(icon_size, FLT_MAX, 0.f, icon);
			d->AddText(font_regular, icon_size, center - icon_extent * 0.5f, LerpColor(col::text_faint, col::text_dim, hover), icon);
		}

		// Text, clipped to the card
		d->PushClipRect(min + ImVec2(S(8.f), 0.f), max - ImVec2(S(8.f), 0.f), true);
		d->AddText(font_bold, S(13.f), ImVec2(min.x + S(10.f), max.y - S(42.f)), C(col::text), title);
		if (subtitle && *subtitle)
			d->AddText(font_regular, S(12.f), ImVec2(min.x + S(10.f), max.y - S(25.f)), active > 0.5f ? C(col::accent) : C(col::text_dim), subtitle);
		d->PopClipRect();

		d->AddRectFilled(ImVec2(min.x + S(12.f), max.y - S(4.f)), ImVec2(max.x - S(12.f), max.y - S(2.f)), C(rarity, 0.5f + 0.5f * hover), 1.f);

		// Item in hand
		if (marked)
			d->AddCircleFilled(ImVec2(max.x - S(13.f), min.y + S(13.f)), S(3.5f), C(col::online), 16);

		d->AddRect(min, max, LerpColor(LerpColor(col::border, col::border_hover, hover), col::accent, active), S(10.f), 0, 1.f + 0.5f * active);

		ImGui::PopID();
		return pressed;
	}

	// Cards left to right, as many per line as fit, stretched to the full width
	struct Grid {
		ImVec2 start;
		ImVec2 card;
		int columns = 1;
		int count = 0;
	};

	Grid BeginGrid(float height = CARD_HEIGHT) {
		Grid grid{};
		grid.start = ImGui::GetCursorScreenPos();

		float available = ImGui::GetContentRegionAvail().x;
		grid.columns = std::max(1, static_cast<int>((available + CARD_GAP) / (CARD_MIN_WIDTH + CARD_GAP)));
		grid.card = ImVec2((available - CARD_GAP * (grid.columns - 1)) / grid.columns, height);

		return grid;
	}

	void NextCard(Grid& grid) {
		int column = grid.count % grid.columns;
		int row = grid.count / grid.columns;

		ImGui::SetCursorScreenPos(grid.start + ImVec2(column * (grid.card.x + CARD_GAP), row * (grid.card.y + CARD_GAP)));
		grid.count++;
	}

	void EndGrid(const Grid& grid) {
		int rows = (grid.count + grid.columns - 1) / grid.columns;

		ImGui::SetCursorScreenPos(grid.start);
		ImGui::Dummy(ImVec2(1.f, rows ? rows * (grid.card.y + CARD_GAP) - CARD_GAP : 0.f));
		ImGui::Dummy(ImVec2(0.f, S(6.f)));
	}

	void SectionTitle(const char* text) {
		ImGui::Dummy(ImVec2(0.f, S(4.f)));

		auto pos = ImGui::GetCursorScreenPos();
		ImGui::GetWindowDrawList()->AddText(font_bold, S(11.f), pos + ImVec2(S(2.f), 0.f), C(col::text_faint), text);

		ImGui::Dummy(ImVec2(0.f, S(20.f)));
	}

	// Tabs at the top, the selection slides between them
	bool TabSwitch(const char* id, int* value, std::initializer_list<const char*> items, ImVec2 pos, float height = S(32.f)) {
		ImGui::PushID(id);

		auto d = ImGui::GetWindowDrawList();
		const float padding = S(16.f);
		const float inset = S(3.f);

		float total = inset * 2.f;
		for (auto item : items)
			total += ImGui::CalcTextSize(item).x + padding * 2.f;

		d->AddRectFilled(pos, pos + ImVec2(total, height), C(col::panel), S(9.f));
		d->AddRect(pos, pos + ImVec2(total, height), C(col::border), S(9.f));

		float x = inset, selected_x = 0.f, selected_w = 0.f;
		int index = 0;

		for (auto item : items) {
			float w = ImGui::CalcTextSize(item).x + padding * 2.f;
			if (index == *value) {
				selected_x = x;
				selected_w = w;
			}

			x += w;
			index++;
		}

		float slide_x = Animate(ImGui::GetID("##x"), selected_x, 16.f);
		float slide_w = Animate(ImGui::GetID("##w"), selected_w, 16.f);
		d->AddRectFilled(pos + ImVec2(slide_x, inset), pos + ImVec2(slide_x + slide_w, height - inset), C(col::accent), S(7.f));

		bool changed = false;
		x = inset;
		index = 0;

		for (auto item : items) {
			auto text_size = ImGui::CalcTextSize(item);
			float w = text_size.x + padding * 2.f;

			ImGui::PushID(index);
			ImGui::SetCursorScreenPos(pos + ImVec2(x, 0.f));

			if (ImGui::InvisibleButton("##tab", ImVec2(w, height)) && *value != index) {
				*value = index;
				changed = true;
			}

			float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);
			auto color = index == *value ? C(col::on_accent) : LerpColor(col::text_dim, col::text, hover);
			d->AddText(pos + ImVec2(x + padding, (height - text_size.y) * 0.5f), color, item);

			ImGui::PopID();
			x += w;
			index++;
		}

		ImGui::PopID();
		return changed;
	}

	// Rounded button with an arrow to the left, true when clicked
	bool BackButton(const char* label, ImVec2 pos) {
		auto text_size = ImGui::CalcTextSize(label);
		auto size = ImVec2(text_size.x + S(34.f), S(28.f));

		ImGui::SetCursorScreenPos(pos);
		bool pressed = ImGui::InvisibleButton(label, size);
		float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);

		auto d = ImGui::GetWindowDrawList();
		d->AddRectFilled(pos, pos + size, LerpColor(col::panel, col::selected, hover), S(8.f));
		d->AddRect(pos, pos + size, LerpColor(col::border, col::border_hover, hover), S(8.f));

		auto arrow = pos + ImVec2(S(14.f) - hover * 2.f, size.y * 0.5f);
		d->AddLine(arrow, arrow + ImVec2(S(5.f), -S(5.f)), C(col::text), 1.5f);
		d->AddLine(arrow, arrow + ImVec2(S(5.f), S(5.f)), C(col::text), 1.5f);

		d->AddText(pos + ImVec2(S(26.f), (size.y - text_size.y) * 0.5f), LerpColor(col::text_dim, col::text, hover), label);
		return pressed;
	}

	// Toggle row with visible & behind a wall color swatches
	bool ToggleColors(const char* label, bool* value, const char* tooltip, color_t* visible, color_t* invisible) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		bool changed = ToggleSwitch("##toggle", value, RowSlot(row, TOGGLE_SIZE));

		ImGui::BeginDisabled(!*value);
		ColorSwatch("##invisible", invisible, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), "Behind a wall");
		ColorSwatch("##visible", visible, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), "Visible");
		ImGui::EndDisabled();

		EndRow(row);
		ImGui::PopID();

		return changed;
	}

	// ESP flag layout

	const char* FlagName(int flag) {
		switch (flag) {
		case cfg::esp::FLAG_NAME:			return "Name";
		case cfg::esp::FLAG_HEALTH_BAR:		return "Health";
		case cfg::esp::FLAG_HEALTH:			return "Health";
		case cfg::esp::FLAG_ARMOR_BAR:		return "Armor";
		case cfg::esp::FLAG_MONEY:			return "Money";
		case cfg::esp::FLAG_PING:			return "Ping";
		case cfg::esp::FLAG_WEAPON:			return "Weapon";
		case cfg::esp::FLAG_WEAPON_NAME:	return "Weapon Name";
		case cfg::esp::FLAG_AMMO:			return "Ammo";
		case cfg::esp::FLAG_DISTANCE:		return "Distance";
		case cfg::esp::FLAG_FLASHED:		return "Flashed Icon";
		case cfg::esp::FLAG_RELOADING:		return "Reloading Icon";
		case cfg::esp::FLAG_DEFUSING:		return "Defusing";
		case cfg::esp::FLAG_SCOPED:			return "Scoped Icon";
		case cfg::esp::FLAG_FLASHED_TEXT:	return "Flashed";
		case cfg::esp::FLAG_RELOADING_TEXT:	return "Reloading";
		case cfg::esp::FLAG_SCOPED_TEXT:	return "Scoped";
		case cfg::esp::FLAG_KIT:			return "Kit";
		case cfg::esp::FLAG_C4:				return "C4";
		}
		return "?";
	}

	bool IsBarFlag(int flag) {
		return flag == cfg::esp::FLAG_HEALTH_BAR || flag == cfg::esp::FLAG_ARMOR_BAR;
	}

	constexpr const char* FLAG_PAYLOAD = "ESP_FLAG";
	#define CHIP_HEIGHT S(22.f)
	#define CHIP_GAP S(4.f)

	ImVec2 ChipSize(int flag) {
		auto text = font_regular->CalcTextSizeA(S(12.f), FLT_MAX, 0.f, FlagName(flag));
		return ImVec2(text.x + S(18.f), CHIP_HEIGHT);
	}

	// Follows the mouse while a flag is dragged
	void DraggedChip(int flag) {
		auto size = ChipSize(flag);
		auto fd = ImGui::GetForegroundDrawList();
		auto mouse = ImGui::GetIO().MousePos - size * 0.5f;
		fd->AddRectFilled(mouse, mouse + size, col::accent, S(6.f));
		fd->AddText(font_regular, S(12.f), mouse + ImVec2(S(9.f), (size.y - S(12.f)) * 0.5f - 1.f), col::on_accent, FlagName(flag));
	}

	// A flag that can be dragged. 1 when clicked, 2 when right clicked
	int FlagChip(int flag, ImVec2 pos, bool placed) {
		ImGui::PushID(flag);

		auto size = ChipSize(flag);
		ImGui::SetCursorScreenPos(pos);
		bool pressed = ImGui::InvisibleButton("##chip", size);
		bool hovered = ImGui::IsItemHovered();
		float h = Animate(ImGui::GetItemID(), hovered ? 1.f : 0.f, 16.f);

		bool dragging = false;
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
			ImGui::SetDragDropPayload(FLAG_PAYLOAD, &flag, sizeof(flag));
			dragging = true;
			ImGui::EndDragDropSource();
		}

		auto d = ImGui::GetWindowDrawList();
		auto fill = placed ? LerpColor(col::selected, col::track_hover, h) : LerpColor(col::track, col::track_hover, h);
		float alpha = dragging ? 0.35f : 1.f;

		d->AddRectFilled(pos, pos + size, C(fill, alpha), S(6.f));
		d->AddRect(pos, pos + size, C(placed ? LerpColor(col::border_hover, col::accent, h) : col::border, alpha), S(6.f));

		// Bars get a small stripe, they are drawn as bars & not as text
		if (IsBarFlag(flag))
			d->AddRectFilled(pos + ImVec2(S(5.f), S(6.f)), pos + ImVec2(S(7.f), size.y - S(6.f)), C(col::accent, alpha), 1.f);

		d->AddText(font_regular, S(12.f), pos + ImVec2(IsBarFlag(flag) ? S(11.f) : S(9.f), (size.y - S(12.f)) * 0.5f - 1.f), C(placed ? col::text : col::text_dim, alpha), FlagName(flag));

		if (dragging)
			DraggedChip(flag);

		if (hovered && !dragging)
			ImGui::SetTooltip(placed ? "Drag to move, right click to remove" : "Drag next to the box, or click to add it");

		int result = pressed ? 1 : hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) ? 2 : 0;

		ImGui::PopID();
		return result;
	}

	// Removes the flag from every side
	void RemoveFlag(cfg::esp::group_t& group, int flag) {
		for (auto& side : group.layout)
			std::erase(side, flag);
	}

	// Lays out the chips of a side inside its zone, returns where each one is
	std::vector<ImVec2> LayoutChips(const std::vector<int>& flags, ImVec2 min, ImVec2 max, bool vertical) {
		std::vector<ImVec2> result;
		result.reserve(flags.size());

		if (vertical) {
			// Stacked from the top, centered in the zone
			float y = min.y + S(4.f);
			for (int flag : flags) {
				auto size = ChipSize(flag);
				result.push_back(ImVec2(roundf((min.x + max.x - size.x) * 0.5f), y));
				y += size.y + CHIP_GAP;
			}
			return result;
		}

		// Rows, wrapped & centered, the top side grows upwards from the box
		std::vector<std::vector<int>> rows(1);
		float width = 0.f;

		for (size_t i = 0; i < flags.size(); i++) {
			float w = ChipSize(flags[i]).x;
			if (!rows.back().empty() && width + w > max.x - min.x - S(8.f)) {
				rows.emplace_back();
				width = 0.f;
			}

			rows.back().push_back(static_cast<int>(i));
			width += w + CHIP_GAP;
		}

		result.resize(flags.size());
		float y = min.y + S(4.f);

		for (const auto& row : rows) {
			float row_width = -CHIP_GAP;
			for (int i : row)
				row_width += ChipSize(flags[i]).x + CHIP_GAP;

			float x = roundf((min.x + max.x - row_width) * 0.5f);
			for (int i : row) {
				result[i] = ImVec2(x, y);
				x += ChipSize(flags[i]).x + CHIP_GAP;
			}

			y += CHIP_HEIGHT + CHIP_GAP;
		}

		return result;
	}

	// Camera looking at target, projecting into the rect with the same math the game matrix goes through
	view_matrix_t PreviewCamera(ImVec2 min, ImVec2 max, Vec3_t target, float yaw, float pitch, float distance, float fov) {
		auto display = ImGui::GetIO().DisplaySize;

		float forward[3] = { cosf(pitch) * cosf(yaw), cosf(pitch) * sinf(yaw), -sinf(pitch) };
		float right[3] = { sinf(yaw), -cosf(yaw), 0.f };
		float up[3] = {
			right[1] * forward[2] - right[2] * forward[1],
			right[2] * forward[0] - right[0] * forward[2],
			right[0] * forward[1] - right[1] * forward[0],
		};

		float eye[3] = { target.x - forward[0] * distance, target.y - forward[1] * distance, target.z - forward[2] * distance };
		auto row = [&](const float* axis, float* out) {
			out[0] = axis[0]; out[1] = axis[1]; out[2] = axis[2];
			out[3] = -(axis[0] * eye[0] + axis[1] * eye[1] + axis[2] * eye[2]);
		};

		float depth[4], side[4], height[4];
		row(forward, depth);
		row(right, side);
		row(up, height);

		float focal = (max.y - min.y) * 0.5f / tanf(fov * 0.5f);
		float cx = (min.x + max.x) * 0.5f, cy = (min.y + max.y) * 0.5f;

		view_matrix_t m{};
		for (int k = 0; k < 4; k++) {
			m.matrix[0][k] = (2.f / display.x) * ((cx - display.x * 0.5f) * depth[k] + focal * side[k]);
			m.matrix[1][k] = (2.f / display.y) * ((display.y * 0.5f - cy) * depth[k] + focal * height[k]);
			m.matrix[2][k] = 0.f;
			m.matrix[3][k] = depth[k];
		}
		return m;
	}

	// Floor of the scene previews
	void DrawPreviewGrid(ImDrawList* d, view_matrix_t& m, float half, float step, ImU32 color) {
		auto display = ImGui::GetIO().DisplaySize;

		// A bit lighter than the background, so shadows show on it
		ImVec2 floor[4];
		const float corners[4][2] = { { -half, -half }, { half, -half }, { half, half }, { -half, half } };
		bool floor_visible = true;

		for (int k = 0; k < 4; k++) {
			Vec2_t screen;
			floor_visible &= m.wts(Vec3_t(corners[k][0], corners[k][1], 0.f), display, screen, false);
			floor[k] = screen;
		}

		if (floor_visible)
			d->AddConvexPolyFilled(floor, 4, IM_COL32(52, 52, 60, 255));

		for (float v = -half; v <= half + 0.01f; v += step) {
			Vec2_t a, b;
			if (m.wts(Vec3_t(v, -half, 0.f), display, a, false) && m.wts(Vec3_t(v, half, 0.f), display, b, false))
				d->AddLine(a, b, color);
			if (m.wts(Vec3_t(-half, v, 0.f), display, a, false) && m.wts(Vec3_t(half, v, 0.f), display, b, false))
				d->AddLine(a, b, color);
		}
	}

	// Ground area of cells, round with a wavy edge, like a shaped smoke or fire
	AreaShape PreviewArea(float x, float y, float radius, float wave, float cell, float top) {
		AreaShape area;
		area.cell_size = cell;
		area.origin_x = x;
		area.origin_y = y;

		int range = static_cast<int>(ceilf((radius + wave) / cell));
		for (int cy = -range; cy <= range; cy++) {
			for (int cx = -range; cx <= range; cx++) {
				float angle = atan2f(static_cast<float>(cy), static_cast<float>(cx));
				float edge = radius + wave * (sinf(angle * 3.f) * 0.6f + cosf(angle * 5.f + 1.f) * 0.4f);

				if (sqrtf(static_cast<float>(cx * cx + cy * cy)) * cell <= edge)
					area.cells.push_back({ cx, cy, 0.f, top });
			}
		}
		return area;
	}

	// Dark box the scene previews are drawn in, dragging turns the camera
	ImRect ScenePreview(const char* id, float height, float& yaw) {
		auto min = ImGui::GetCursorScreenPos();
		auto size = ImVec2(ImGui::GetContentRegionAvail().x, height);

		ImGui::InvisibleButton(id, size);

		// Still unless dragged, a still scene is drawn once & kept
		if (ImGui::IsItemActive())
			yaw -= ImGui::GetIO().MouseDelta.x * 0.01f;

		if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
			ImGui::SetTooltip("Drag to turn the camera");

		ImGui::GetWindowDrawList()->AddRectFilled(min, min + size, C(col::window), S(8.f));
		return ImRect(min, min + size);
	}

	// Made up players for the preview, every flag has something to show
	std::pair<Player, Player> PreviewPlayers() {
		Player local, player;
		local.pos = Vec3_t(0.f, 590.f, 0.f);	// 15m

		strcpy_s(player.name, "Player");
		player.bot = false;
		player.alive = true;
		player.health = 80;
		player.armor = 65;
		player.money = 4750;
		player.ping = 24;
		player.ammo = 25;
		player.weapon.item_index = -1;
		player.weapon.name = "AK-47";
		player.weapon.icon = WeaponIcons::AK47;

		return { local, player };
	}

	// Simple player figure for the preview, head, body, arms & legs
	void DrawFigure(ImDrawList* d, ImVec2 min, ImVec2 max, ImU32 color) {
		float w = max.x - min.x, h = max.y - min.y;
		auto at = [&](float x, float y) { return ImVec2(min.x + w * x, min.y + h * y); };
		float thickness = std::max(S(2.f), w * 0.05f);

		d->AddCircle(at(0.5f, 0.09f), w * 0.11f, color, 20, thickness * 0.8f);
		d->AddLine(at(0.5f, 0.17f), at(0.5f, 0.52f), color, thickness);	// Body
		d->AddLine(at(0.5f, 0.24f), at(0.2f, 0.42f), color, thickness);	// Arms
		d->AddLine(at(0.2f, 0.42f), at(0.14f, 0.55f), color, thickness);
		d->AddLine(at(0.5f, 0.24f), at(0.8f, 0.42f), color, thickness);
		d->AddLine(at(0.8f, 0.42f), at(0.86f, 0.55f), color, thickness);
		d->AddLine(at(0.5f, 0.52f), at(0.3f, 0.76f), color, thickness);	// Legs
		d->AddLine(at(0.3f, 0.76f), at(0.27f, 0.98f), color, thickness);
		d->AddLine(at(0.5f, 0.52f), at(0.7f, 0.76f), color, thickness);
		d->AddLine(at(0.7f, 0.76f), at(0.73f, 0.98f), color, thickness);
	}

	// Player model of the ESP preview, made by scripts/export_player_models.py
	struct PreviewModel {
		std::vector<ImVec4> vertices;	// x, y (up), z
		std::vector<uint32_t> indices;
		float height = 1.f;				// Feet at 0
		float center_x = 0.f, center_z = 0.f;

		// Made once with the model, so turning it costs no more than placing it: the unit normal of each
		// triangle, & the vertices furthest out in some direction, all an outline needs
		std::vector<ImVec4> normals;
		std::vector<uint32_t> outside;
	};

	// CS2M v1: vertex & triangle counts, float3 positions (y up), uint16 indices below 65536 vertices, uint32 otherwise
	std::unique_ptr<PreviewModel> ParsePreviewModel(const uint8_t* data, size_t size) {
		if (size < 16 || memcmp(data, "CS2M", 4) != 0)
			return nullptr;

		uint32_t version, vertex_count, triangle_count;
		memcpy(&version, data + 4, 4);
		memcpy(&vertex_count, data + 8, 4);
		memcpy(&triangle_count, data + 12, 4);

		if (version != 1 || !vertex_count || !triangle_count || vertex_count > 1000000 || triangle_count > 1000000)
			return nullptr;

		size_t index_size = vertex_count < 65536 ? 2 : 4;
		size_t positions = 16, indices = positions + static_cast<size_t>(vertex_count) * 12;
		if (indices + static_cast<size_t>(triangle_count) * 3 * index_size > size)
			return nullptr;

		auto model = std::make_unique<PreviewModel>();
		model->vertices.reserve(vertex_count);
		model->indices.resize(static_cast<size_t>(triangle_count) * 3);

		float min_x = FLT_MAX, max_x = -FLT_MAX, min_z = FLT_MAX, max_z = -FLT_MAX, max_y = 0.f;
		for (uint32_t i = 0; i < vertex_count; i++) {
			float v[3];
			memcpy(v, data + positions + i * 12, 12);
			model->vertices.push_back(ImVec4(v[0], v[1], v[2], 0.f));

			min_x = std::min(min_x, v[0]); max_x = std::max(max_x, v[0]);
			min_z = std::min(min_z, v[2]); max_z = std::max(max_z, v[2]);
			max_y = std::max(max_y, v[1]);
		}

		for (size_t i = 0; i < model->indices.size(); i++) {
			uint32_t index = 0;
			memcpy(&index, data + indices + i * index_size, index_size);
			if (index >= vertex_count)
				return nullptr;
			model->indices[i] = index;
		}

		model->height = std::max(max_y, 0.1f);
		model->center_x = (min_x + max_x) * 0.5f;
		model->center_z = (min_z + max_z) * 0.5f;

		model->normals.resize(triangle_count);
		for (size_t t = 0; t < triangle_count; t++) {
			const auto& a = model->vertices[model->indices[t * 3]];
			const auto& b = model->vertices[model->indices[t * 3 + 1]];
			const auto& c = model->vertices[model->indices[t * 3 + 2]];
			float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
			float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			float length = sqrtf(nx * nx + ny * ny + nz * nz);
			model->normals[t] = length > 0.f ? ImVec4(nx / length, ny / length, nz / length, 1.f) : ImVec4(0.f, 0.f, 0.f, 0.f);
		}

		// The furthest vertex along directions spread over a sphere: the outline from any side is close to theirs
		constexpr int DIRECTIONS = 96;
		std::vector<uint32_t> outside;
		for (int k = 0; k < DIRECTIONS; k++) {
			float y = 1.f - 2.f * (k + 0.5f) / DIRECTIONS;
			float r = sqrtf(std::max(0.f, 1.f - y * y));
			float angle = k * 2.399963f; // Golden angle
			float dx = cosf(angle) * r, dz = sinf(angle) * r;

			uint32_t best = 0;
			float best_dot = -FLT_MAX;
			for (uint32_t i = 0; i < vertex_count; i++) {
				const auto& v = model->vertices[i];
				float dot = v.x * dx + v.y * y + v.z * dz;
				if (dot > best_dot) {
					best_dot = dot;
					best = i;
				}
			}
			if (std::find(outside.begin(), outside.end(), best) == outside.end())
				outside.push_back(best);
		}
		model->outside = std::move(outside);
		return model;
	}

	// models/<name>.mesh next to the executable when there is one, the models built into the program otherwise
	const PreviewModel* GetPreviewModel(const std::string& name) {
		static std::map<std::string, std::unique_ptr<PreviewModel>> cache;

		auto it = cache.find(name);
		if (it != cache.end())
			return it->second.get();

		auto& entry = cache[name];

		char exe_path[MAX_PATH]{};
		if (GetModuleFileNameA(nullptr, exe_path, MAX_PATH)) {
			std::ifstream f(std::filesystem::path(exe_path).parent_path() / "models" / (name + ".mesh"), std::ios::binary);
			std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

			if (!bytes.empty())
				entry = ParsePreviewModel(bytes.data(), bytes.size());
		}

		if (!entry) {
			if (name == "t")
				entry = ParsePreviewModel(player_model_t, player_model_t_size);
			else if (name == "ct")
				entry = ParsePreviewModel(player_model_ct, player_model_ct_size);
			else if (name == "c4")
				entry = ParsePreviewModel(player_model_c4, player_model_c4_size);
		}

		return entry.get();
	}

	// Triangles of a model already on screen, kept & drawn again until the view changes. Sorting thousands
	// of triangles every frame is what costs, so a turning model is rebuilt at most REBUILD_RATE times a second.
	// Points are relative to an anchor, so moving the window does not need a rebuild
	struct MeshCache {
		uint64_t shape = 0;		// Model, color & size, rebuilt right away when it changes
		uint64_t view = 0;		// Rotation & camera, rebuilt at the rate when it changes
		std::chrono::steady_clock::time_point built;
		bool valid = false;

		std::vector<ImVec2> points;	// 3 per triangle
		std::vector<ImU32> colors;		// 1 per triangle
		std::vector<std::vector<ImVec2>> shadows;
		std::vector<ImVec2> hull;		// Outline of the drawing, for a glow that does not draw every triangle again
	};

	constexpr auto REBUILD_INTERVAL = std::chrono::milliseconds(33);

	uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
		auto bytes = static_cast<const uint8_t*>(data);
		for (size_t i = 0; i < size; i++)
			hash = (hash ^ bytes[i]) * 1099511628211ull;
		return hash;
	}

	// True when the cache has to be built again now
	bool NeedsRebuild(MeshCache& cache, uint64_t shape, uint64_t view) {
		auto now = std::chrono::steady_clock::now();

		bool rebuild = !cache.valid || cache.shape != shape
			|| (cache.view != view && now - cache.built >= REBUILD_INTERVAL);

		if (rebuild) {
			cache.shape = shape;
			cache.view = view;
			cache.built = now;
			cache.valid = true;
			cache.points.clear();
			cache.colors.clear();
			cache.shadows.clear();
			cache.hull.clear();
		}

		return rebuild;
	}

	// scale: stretched from the anchor, the idle breathing moves the cached drawing without a rebuild.
	// tint: every triangle in that one color & no shadow, the shape only, faded already.
	// The cache has no fade of the menu in it, a fading page would rebuild it every frame: it is put on here
	void DrawMeshCache(ImDrawList* d, const MeshCache& cache, ImVec2 anchor, ImVec2 scale = ImVec2(1.f, 1.f), ImU32 tint = 0) {
		const float fade = ImGui::GetStyle().Alpha;
		auto faded = [fade](ImU32 color) {
			if (fade >= 1.f)
				return color;
			auto alpha = static_cast<ImU32>(((color >> IM_COL32_A_SHIFT) & 0xFF) * fade);
			return (color & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
		};

		for (const auto& shadow : cache.shadows) {
			if (tint)
				break;

			std::vector<ImVec2> points(shadow.size());
			for (size_t i = 0; i < shadow.size(); i++)
				points[i] = anchor + shadow[i] * scale;
			d->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), faded(IM_COL32(0, 0, 0, 60)));
		}

		// Antialiased fills leave seams between the triangles, written straight into the buffers
		const ImVec2 uv = d->_Data->TexUvWhitePixel;
		constexpr size_t BATCH = 20000; // Triangles per reserve, below 65536 vertices for 16 bit indices

		for (size_t start = 0; start < cache.colors.size(); start += BATCH) {
			size_t count = std::min(BATCH, cache.colors.size() - start);
			d->PrimReserve(static_cast<int>(count * 3), static_cast<int>(count * 3));

			for (size_t t = start; t < start + count; t++) {
				auto color = tint ? tint : faded(cache.colors[t]);
				d->PrimVtx(anchor + cache.points[t * 3] * scale, uv, color);
				d->PrimVtx(anchor + cache.points[t * 3 + 1] * scale, uv, color);
				d->PrimVtx(anchor + cache.points[t * 3 + 2] * scale, uv, color);
			}
		}
	}

	// The outline glow of the game: a soft halo around the thing, drawn before it. Copies of the shape in the
	// color, fainter the further out, from the outside in so the bright edge is on top.
	// draw(offset, color) draws the shape moved by the offset in the color
	template <typename Draw>
	void DrawGlowAround(ImU32 color, Draw draw) {
		constexpr int STEPS = 12;
		constexpr float RINGS[][2] = { { 6.f, 0.07f }, { 4.5f, 0.12f }, { 3.f, 0.2f }, { 1.5f, 0.35f } }; // Radius, alpha

		ImVec4 base = ImGui::ColorConvertU32ToFloat4(color);
		for (const auto& [radius, alpha] : RINGS) {
			ImU32 ring = ImGui::ColorConvertFloat4ToU32(ImVec4(base.x, base.y, base.z, base.w * alpha)); // The fade of the menu is in already
			for (int i = 0; i < STEPS; i++) {
				float angle = (i + (radius > 4.f ? 0.5f : 0.f)) * 2.f * std::numbers::pi_v<float> / STEPS;
				draw(ImVec2(cosf(angle) * S(radius), sinf(angle) * S(radius)), ring);
			}
		}
	}

	// Color of a glow in the preview, 0 when it is off or cannot be written into the game
	ImU32 PreviewGlow(bool enabled, const color_t& color) {
		if (!enabled || !Visuals::IsAvailable())
			return 0;
		return ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, color.a));
	}

	// A weapon icon lying on the floor where the model would be, the preview has no model of it.
	// Under the point, the ESP icon is above it
	void DrawStandIn(ImDrawList* d, view_matrix_t& m, Vec3_t pos, const char* icon, float size, ImU32 glow) {
		Vec2_t screen;
		auto font = Esp::GetIconFont();
		if (!font || !icon || !*icon || !m.wts(pos, ImGui::GetIO().DisplaySize, screen, false))
			return;

		auto text_size = font->CalcTextSizeA(size, FLT_MAX, 0.f, icon);
		ImVec2 at(floorf(screen.x - text_size.x * 0.5f), floorf(screen.y + S(3.f)));

		if (glow)
			DrawGlowAround(glow, [&](ImVec2 offset, ImU32 shade) { d->AddText(font, size, at + offset, shade, icon); });

		d->AddText(font, size, at, C(IM_COL32(150, 152, 160, 255)), icon);
	}

	// Convex hull of points, counter clockwise on screen (y down), empty below 3 points
	std::vector<ImVec2> ConvexHull(std::vector<ImVec2> points) {
		std::sort(points.begin(), points.end(), [](const ImVec2& a, const ImVec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });

		std::vector<ImVec2> hull;
		auto turn = [](const ImVec2& o, const ImVec2& a, const ImVec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };

		for (int pass = 0; pass < 2 && !points.empty(); pass++) {
			size_t start = hull.size();
			for (size_t k = 0; k < points.size(); k++) {
				const auto& p = pass == 0 ? points[k] : points[points.size() - 1 - k];
				while (hull.size() >= start + 2 && turn(hull[hull.size() - 2], hull.back(), p) <= 0.f)
					hull.pop_back();
				hull.push_back(p);
			}
			hull.pop_back();
		}

		if (hull.size() < 3)
			hull.clear();
		return hull;
	}

	// Model standing in a scene preview, the file is in meters with y up, the game in inches with z up.
	// Its shadow falls on the floor (z 0) away from the light
	void DrawSceneModel(ImDrawList* d, const PreviewModel& model, view_matrix_t& m, Vec3_t origin, float yaw, ImU32 color, bool shadow, ImU32 glow = 0) {
		constexpr float INCHES = 39.37f;
		auto display = ImGui::GetIO().DisplaySize;

		// Everything is relative to where the origin is on screen
		Vec2_t anchor_screen;
		ImVec2 anchor = m.wts(origin, display, anchor_screen, false) ? ImVec2(anchor_screen.x, anchor_screen.y) : ImVec2(0.f, 0.f);

		// Camera & turn, the drawing stays right in between rebuilds since it follows the anchor
		uint64_t view = HashBytes(1469598103934665603ull, &yaw, sizeof(yaw));
		view = HashBytes(view, &origin, sizeof(origin));
		view = HashBytes(view, m.matrix, sizeof(m.matrix));

		uint64_t shape = HashBytes(1469598103934665603ull, &model, sizeof(&model));
		shape = HashBytes(shape, &color, sizeof(color));
		shape = HashBytes(shape, &shadow, sizeof(shadow));

		static std::map<const PreviewModel*, MeshCache> caches;
		auto& cache = caches[&model];

		// The glow around the outline of the drawing: a few points each time, not every triangle again
		auto draw = [&]() {
			if (glow && !cache.hull.empty()) {
				std::vector<ImVec2> points(cache.hull.size());
				DrawGlowAround(glow, [&](ImVec2 offset, ImU32 shade) {
					for (size_t i = 0; i < points.size(); i++)
						points[i] = anchor + offset + cache.hull[i];
					d->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), shade);
				});
			}
			DrawMeshCache(d, cache, anchor);
		};

		if (!NeedsRebuild(cache, shape, view)) {
			draw();
			return;
		}

		// Towards the light, from above & a side
		const float light[3] = { -0.62f, 0.5f, 0.6f };
		float cos_yaw = cosf(yaw), sin_yaw = sinf(yaw);

		// Kept between rebuilds, a rebuild then allocates nothing
		static std::vector<Vec3_t> world;
		static std::vector<ImVec2> screen;
		static std::vector<float> depth;
		static std::vector<uint8_t> visible;
		world.resize(model.vertices.size());
		screen.resize(model.vertices.size());
		depth.resize(model.vertices.size());
		visible.resize(model.vertices.size());

		for (size_t i = 0; i < model.vertices.size(); i++) {
			const auto& v = model.vertices[i];
			float x = (v.x - model.center_x) * INCHES, y = -(v.z - model.center_z) * INCHES;

			auto& p = world[i];
			p = Vec3_t(
				origin.x + x * cos_yaw - y * sin_yaw,
				origin.y + x * sin_yaw + y * cos_yaw,
				origin.z + v.y * INCHES
			);

			Vec2_t s;
			visible[i] = m.wts(p, display, s, false);
			screen[i] = ImVec2(s.x, s.y) - anchor;
			depth[i] = m[3][0] * p.x + m[3][1] * p.y + m[3][2] * p.z + m[3][3];
		}

		// The outlines from the outside vertices only: the shadow pressed onto the floor along the light, & the glow
		std::vector<ImVec2> outline, floor;
		for (auto i : model.outside) {
			if (visible[i])
				outline.push_back(screen[i]);

			if (shadow) {
				const auto& p = world[i];
				float t = p.z / light[2];
				Vec2_t s;
				if (m.wts(Vec3_t(p.x - light[0] * t, p.y - light[1] * t, 0.f), display, s, false))
					floor.push_back(ImVec2(s.x, s.y));
			}
		}
		cache.hull = ConvexHull(std::move(outline));

		auto hull = ConvexHull(std::move(floor));
		if (!hull.empty()) {
			// Softer & a bit bigger first, so the edge fades
			ImVec2 center(0.f, 0.f);
			for (const auto& p : hull)
				center += p;
			center = center / static_cast<float>(hull.size());

			for (float grow : { 1.12f, 1.05f, 1.f }) {
				std::vector<ImVec2> scaled;
				for (const auto& p : hull)
					scaled.push_back(center + (p - center) * grow - anchor);
				cache.shadows.push_back(std::move(scaled));
			}
		}

		struct Face {
			float depth;
			uint32_t index;
			float light;
		};

		static std::vector<Face> faces;
		faces.clear();
		faces.reserve(model.indices.size() / 3);

		for (uint32_t t = 0; t < model.indices.size() / 3; t++) {
			uint32_t ia = model.indices[t * 3], ib = model.indices[t * 3 + 1], ic = model.indices[t * 3 + 2];
			const auto& n = model.normals[t];
			if (!visible[ia] || !visible[ib] || !visible[ic] || n.w == 0.f)
				continue;

			// The normal turned like the vertices: x, -z, y of the file, then the yaw
			float nx = n.x, ny = -n.z, nz = n.y;
			float wx = nx * cos_yaw - ny * sin_yaw, wy = nx * sin_yaw + ny * cos_yaw;
			float facing = fabsf(wx * light[0] + wy * light[1] + nz * light[2]);
			faces.push_back({ depth[ia] + depth[ib] + depth[ic], t, 0.35f + 0.65f * facing });
		}

		// Far first
		std::sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.depth > r.depth; });

		auto base = ImGui::ColorConvertU32ToFloat4(color);
		cache.points.reserve(faces.size() * 3);
		cache.colors.reserve(faces.size());

		for (const auto& face : faces) {
			cache.colors.push_back(ImGui::ColorConvertFloat4ToU32(ImVec4(base.x * face.light, base.y * face.light, base.z * face.light, base.w)));
			cache.points.push_back(screen[model.indices[face.index * 3]]);
			cache.points.push_back(screen[model.indices[face.index * 3 + 1]]);
			cache.points.push_back(screen[model.indices[face.index * 3 + 2]]);
		}

		draw();
	}

	// The player pre-rendered by scripts/render_player_previews.py: textured, holding the rifle, from FRAMES
	// angles around. Drawing it is one image, the one of the angle it is turned to. False without the image.
	// coat: the glow chams, the shape in that color over the model, its shading still showing through
	bool DrawPlayerPicture(ImDrawList* d, bool terrorist, ImVec2 min, ImVec2 max, float yaw, ImU32 glow = 0, ImU32 tint = 0, ImU32 coat = 0) {
		struct Picture {
			bool tried = false;
			ImTextureID texture = 0;
			bool shape_tried = false;
			ImTextureID shape = 0;	// All white, tinted for the glow
		};
		static Picture pictures[2];

		auto load = [&](bool silhouette) {
			return terrorist
				? ImageCache::FromMemory(player_preview_t, player_preview_t_size, player_preview_t_alpha, player_preview_t_alpha_size, silhouette)
				: ImageCache::FromMemory(player_preview_ct, player_preview_ct_size, player_preview_ct_alpha, player_preview_ct_alpha_size, silhouette);
		};

		auto& picture = pictures[terrorist ? 0 : 1];
		if (!picture.tried) {
			picture.tried = true;
			picture.texture = load(false);
		}

		// Only made once the glow is turned on
		if ((glow || coat) && !picture.shape_tried) {
			picture.shape_tried = true;
			picture.shape = load(true);
		}

		if (!picture.texture)
			return false;

		namespace pp = player_preview;
		constexpr int ROWS = (pp::FRAMES + pp::COLUMNS - 1) / pp::COLUMNS;

		// The angle nearest to the turn
		constexpr float STEP = 2.f * std::numbers::pi_v<float> / pp::FRAMES;
		int frame = static_cast<int>(roundf(yaw / STEP)) % pp::FRAMES;
		if (frame < 0)
			frame += pp::FRAMES;

		ImVec2 uv_min(static_cast<float>(frame % pp::COLUMNS) / pp::COLUMNS, static_cast<float>(frame / pp::COLUMNS) / ROWS);
		ImVec2 uv_max = uv_min + ImVec2(1.f / pp::COLUMNS, 1.f / ROWS);

		// Head to feet fills the rect, idle: breathing in & out from the feet
		float breath = sinf(static_cast<float>(ImGui::GetTime()) * 2.2f);
		float scale = (max.y - min.y) / (terrorist ? player_preview_t_height : player_preview_ct_height);
		float scale_x = scale * (1.f + 0.004f * breath), scale_y = scale * (1.f + 0.006f * breath);

		ImVec2 feet((min.x + max.x) * 0.5f, max.y);
		ImVec2 image_min(feet.x - pp::FRAME_WIDTH * 0.5f * scale_x, feet.y - pp::FEET * scale_y);
		ImVec2 image_max(image_min.x + pp::FRAME_WIDTH * scale_x, image_min.y + pp::FRAME_HEIGHT * scale_y);

		if (glow && picture.shape)
			DrawGlowAround(glow, [&](ImVec2 offset, ImU32 shade) {
				d->AddImage(ImTextureRef(picture.shape), image_min + offset, image_max + offset, uv_min, uv_max, shade);
			});

		// Chams multiply the colors of the model, like the tint of the image
		d->AddImage(ImTextureRef(picture.texture), image_min, image_max, uv_min, uv_max, tint ? tint : C(IM_COL32_WHITE));

		if (coat && picture.shape)
			d->AddImage(ImTextureRef(picture.shape), image_min, image_max, uv_min, uv_max, coat);
		return true;
	}

	// Flat shaded, sorted back to front, standing on the bottom of the rect
	void DrawModel(ImDrawList* d, const PreviewModel& model, ImVec2 min, ImVec2 max, float yaw, ImU32 color, ImU32 glow = 0) {
		float scale = (max.y - min.y) / model.height;
		ImVec2 anchor((min.x + max.x) * 0.5f, max.y);

		uint64_t shape = HashBytes(1469598103934665603ull, &model, sizeof(&model));
		shape = HashBytes(shape, &color, sizeof(color));
		shape = HashBytes(shape, &scale, sizeof(scale));
		uint64_t view = HashBytes(1469598103934665603ull, &yaw, sizeof(yaw));

		static std::map<const PreviewModel*, MeshCache> caches;
		auto& cache = caches[&model];

		// Idle: breathing in & out, a little taller & wider, from the feet
		float breath = sinf(static_cast<float>(ImGui::GetTime()) * 2.2f);
		ImVec2 idle(1.f + 0.004f * breath, 1.f + 0.006f * breath);

		auto draw = [&]() {
			if (glow)
				DrawGlowAround(glow, [&](ImVec2 offset, ImU32 shade) { DrawMeshCache(d, cache, anchor + offset, idle, shade); });
			DrawMeshCache(d, cache, anchor, idle);
		};

		if (!NeedsRebuild(cache, shape, view)) {
			draw();
			return;
		}

		float cos_yaw = cosf(yaw), sin_yaw = sinf(yaw);
		auto base = ImGui::ColorConvertU32ToFloat4(color);

		// Turned around the middle of the model, x to the right & z towards the camera
		std::vector<ImVec4> view_space(model.vertices.size());
		for (size_t i = 0; i < model.vertices.size(); i++) {
			const auto& v = model.vertices[i];
			float x = v.x - model.center_x, z = v.z - model.center_z;

			view_space[i] = ImVec4(x * cos_yaw - z * sin_yaw, v.y, x * sin_yaw + z * cos_yaw, 0.f);
		}

		struct Face {
			float depth;
			uint32_t index;
			float light;
		};

		std::vector<Face> faces;
		faces.reserve(model.indices.size() / 3);

		// Light from the front, a bit from above & the left
		const float light_x = -0.35f, light_y = 0.5f, light_z = 0.8f;

		for (uint32_t t = 0; t < model.indices.size() / 3; t++) {
			const auto& a = view_space[model.indices[t * 3]];
			const auto& b = view_space[model.indices[t * 3 + 1]];
			const auto& c = view_space[model.indices[t * 3 + 2]];

			float ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z;
			float vx = c.x - a.x, vy = c.y - a.y, vz = c.z - a.z;
			float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
			float length = sqrtf(nx * nx + ny * ny + nz * nz);
			if (length <= 0.f)
				continue;

			nx /= length; ny /= length; nz /= length;

			// Both sides, the export does not keep a consistent winding everywhere
			float facing = fabsf(nx * light_x + ny * light_y + nz * light_z);
			faces.push_back({ a.z + b.z + c.z, t, 0.35f + 0.65f * facing });
		}

		std::sort(faces.begin(), faces.end(), [](const Face& l, const Face& r) { return l.depth < r.depth; });

		auto to_screen = [&](const ImVec4& v) { return ImVec2(v.x * scale, -v.y * scale); };

		cache.points.reserve(faces.size() * 3);
		cache.colors.reserve(faces.size());

		for (const auto& face : faces) {
			// Includes the alpha of the menu fade
			cache.colors.push_back(ImGui::ColorConvertFloat4ToU32(ImVec4(base.x * face.light, base.y * face.light, base.z * face.light, base.w)));
			cache.points.push_back(to_screen(view_space[model.indices[face.index * 3]]));
			cache.points.push_back(to_screen(view_space[model.indices[face.index * 3 + 1]]));
			cache.points.push_back(to_screen(view_space[model.indices[face.index * 3 + 2]]));
		}

		draw();
	}

	// Sidebar tab, the selection highlight is drawn separately so it can slide between tabs
	bool SidebarItem(const char* icon, const char* label, bool active, ImVec2 pos, ImVec2 size) {
		ImGui::PushID(label);

		ImGui::SetCursorScreenPos(pos);
		bool pressed = ImGui::InvisibleButton("##tab", size);
		bool hovered = ImGui::IsItemHovered();

		float t = Animate(ImGui::GetItemID(), active ? 1.f : 0.f, 12.f);
		float h = Animate(ImGui::GetItemID() + 1, hovered && !active ? 1.f : 0.f, 16.f);

		auto d = ImGui::GetWindowDrawList();

		if (h > 0.01f)
			d->AddRectFilled(pos, pos + size, C(col::hover, h), S(8.f));

		float slide = h * 3.f + t * 2.f;
		auto text_y = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;

		// Icons fit a box in front of the label, wide weapon icons are made smaller
		const float icon_box = S(22.f);
		auto font = ImGui::GetFont();
		float font_size = ImGui::GetFontSize();
		auto icon_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, icon);
		if (icon_size.x > icon_box) {
			font_size *= icon_box / icon_size.x;
			icon_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, icon);
		}

		auto icon_pos = ImVec2(pos.x + S(12.f) + slide + (icon_box - icon_size.x) * 0.5f, pos.y + (size.y - icon_size.y) * 0.5f);
		d->AddText(font, font_size, ImVec2(floorf(icon_pos.x), floorf(icon_pos.y)), LerpColor(LerpColor(col::text_faint, col::text_dim, h), col::accent, t), icon);
		d->AddText(ImVec2(pos.x + S(42.f) + slide, text_y), LerpColor(col::text_dim, col::text, std::max(t, h)), label);

		ImGui::PopID();
		return pressed;
	}
}

bool Menu::Init() {
	return GetInstance().InitImpl();
}

std::string Menu::GetKeyName(int key) {
	return KeyName(key);
}

void Menu::Render() {
	return GetInstance().RenderImpl();
}

void Menu::RenderStartupHelp() {
	return GetInstance().RenderStartupHelpImpl();
}

ImVec2 Menu::GetPos() {
	return GetInstance().pos;
}

ImVec2 Menu::GetSize() {
	return GetInstance().size;
}

bool Menu::InitImpl() {
	SetScale(1.f);
	SetupStyles();

	LOGF(INFO, "Successfully initialized menu...");
	return true;
}

void Menu::RenderImpl() {
	if (!isSetup)
		return;

	// Clicked in the preset lists last frame
	if (ProcessPreset(Config::Preset::CONFIG, config_presets)) {
		Window::vsync = cfg::settings::vsync;
		Window::SetAffinity(Window::hwnd, cfg::settings::streamproof ? WindowAffinity::Invisible : WindowAffinity::Disabled);
	}
	ProcessPreset(Config::Preset::SKINS, skin_presets);

	col::accent = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg::settings::accent.r, cfg::settings::accent.g, cfg::settings::accent.b, 1.f));

	auto& io = ImGui::GetIO();
	bool open = Renderer::IsOpen();

	// A new scale is taken once the mouse is let go, the slider would move away under it
	if (applied_scale <= 0.f || !ImGui::IsMouseDown(ImGuiMouseButton_Left))
		applied_scale = std::round(cfg::settings::ui_scale * 20.f) / 20.f;

	// Never bigger than the screen
	float scale = applied_scale;
	if (io.DisplaySize.x > 0.f && io.DisplaySize.y > 0.f)
		scale = std::min(scale, std::min(io.DisplaySize.x * 0.96f / BASE_MENU_SIZE.x, io.DisplaySize.y * 0.94f / BASE_MENU_SIZE.y));

	SetScale(std::clamp(scale, 0.6f, 2.f));

	// Fades in & out, the content slides in again on every open
	if (open && !was_open)
		tab_progress = 0.f;
	was_open = open;

	open_progress = std::clamp(open_progress + io.DeltaTime * (open ? MENU_OPEN_SPEED : -MENU_CLOSE_SPEED), 0.f, 1.f);
	tab_progress = std::min(1.f, tab_progress + io.DeltaTime * TAB_SWITCH_SPEED);

	if (open_progress <= 0.f)
		return;

	float alpha = EaseOutCubic(open_progress);

	ImGui::SetNextWindowSize(MENU_SIZE, ImGuiCond_Always);
	ImGui::SetNextWindowPos(io.DisplaySize * 0.5f - MENU_SIZE * 0.5f, ImGuiCond_FirstUseEver);

	// Text of the menu follows its scale, the fonts are made at the size they are used
	ImGui::PushFont(font_regular, S(16.f));

	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

	auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;

	// Closing, it is only fading out
	if (!open)
		flags |= ImGuiWindowFlags_NoInputs;

	bool visible = ImGui::Begin("##menu", nullptr, flags);
	ImGui::PopStyleVar(2);

	if (visible) {
		// Bigger after a scale change, keep it on the screen
		auto window_pos = ImGui::GetWindowPos();
		auto clamped = ImVec2(
			std::clamp(window_pos.x, 0.f, std::max(0.f, io.DisplaySize.x - MENU_SIZE.x)),
			std::clamp(window_pos.y, 0.f, std::max(0.f, io.DisplaySize.y - MENU_SIZE.y))
		);
		if (clamped.x != window_pos.x || clamped.y != window_pos.y)
			ImGui::SetWindowPos(clamped);

		this->pos = ImGui::GetWindowPos();
		this->size = ImGui::GetWindowSize();

		RenderBackground();
		RenderSidebar();
		RenderHeader();

		// Two columns below the header, sliding & fading in after a tab switch
		float tab = EaseOutCubic(tab_progress);

		column_layout.x = SIDEBAR_WIDTH + CONTENT_PADDING;
		column_layout.y = HEADER_HEIGHT + CONTENT_PADDING * 0.75f + (1.f - tab) * TAB_SLIDE;
		column_layout.w = (size.x - column_layout.x - CONTENT_PADDING - COLUMN_SPACING) * 0.5f;
		column_layout.h = size.y - column_layout.y - CONTENT_PADDING * 0.5f;

		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha * tab);

		switch (active_tab) {
		case Tab::PLAYERS:	RenderPlayersTab();		break;
		case Tab::BOMB:			RenderBombTab();		break;
		case Tab::PROJECTILES:	RenderProjectilesTab();	break;
		case Tab::ITEMS:		RenderItemsTab();		break;
		case Tab::SKINS:	RenderSkinsTab();		break;
		case Tab::MOVEMENT:	RenderMovementTab();	break;
		case Tab::MISC:		RenderMiscTab();		break;
		case Tab::CONFIGS:	RenderConfigsTab();		break;
		case Tab::SETTINGS:	RenderSettingsTab();	break;
		}

		ImGui::PopStyleVar();
	}

	ImGui::End();
	ImGui::PopStyleVar();

	RenderPreviewPanel(alpha);
	ImGui::PopFont();
}

// Previews of the tabs in their own panel next to the menu, so the options fit without scrolling
void Menu::RenderPreviewPanel(float alpha) {
	if (active_tab != Tab::PLAYERS && active_tab != Tab::BOMB && active_tab != Tab::PROJECTILES && active_tab != Tab::ITEMS)
		return;

	auto& io = ImGui::GetIO();
	const float width = S(380.f);
	const float gap = S(12.f);

	// Right of the menu, on its left when the screen ends first
	ImVec2 at(pos.x + size.x + gap, pos.y);
	if (at.x + width > io.DisplaySize.x && pos.x - gap - width >= 0.f)
		at.x = pos.x - gap - width;

	float tab = EaseOutCubic(tab_progress);

	ImGui::SetNextWindowPos(at, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, size.y), ImGuiCond_Always);

	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CONTENT_PADDING, CONTENT_PADDING));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

	auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;

	// Closing, it is only fading out
	if (!Renderer::IsOpen())
		flags |= ImGuiWindowFlags_NoInputs;

	bool visible = ImGui::Begin("##preview_panel", nullptr, flags);
	ImGui::PopStyleVar(2);

	if (visible) {
		auto d = ImGui::GetWindowDrawList();
		auto min = ImGui::GetWindowPos();
		auto max = min + ImGui::GetWindowSize();

		Glow(ImGui::GetBackgroundDrawList(), min, max, col::shadow, WINDOW_ROUNDING, S(24.f), 0.8f);
		d->AddRectFilled(min, max, C(col::window), WINDOW_ROUNDING);
		d->AddRect(min, max, C(col::border), WINDOW_ROUNDING, 0, 1.f);

		// Slides & fades in with the tab
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.f - tab) * TAB_SLIDE);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha * tab);

		switch (active_tab) {
		case Tab::PLAYERS:      RenderPlayersPreview();     break;
		case Tab::BOMB:         RenderBombPreview();        break;
		case Tab::PROJECTILES:  RenderProjectilesPreview(); break;
		case Tab::ITEMS:        RenderItemsPreview();       break;
		default: break;
		}

		ImGui::PopStyleVar();
	}

	ImGui::End();
	ImGui::PopStyleVar();
}

void Menu::RenderBackground() {
	auto d = ImGui::GetWindowDrawList();
	auto bg = ImGui::GetBackgroundDrawList();
	auto max = pos + size;

	// Dim the game a bit & drop a shadow under the window
	bg->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, C(col::shadow, 0.2f));
	Glow(bg, pos, max, col::shadow, WINDOW_ROUNDING, S(24.f), 0.8f);

	// Window & sidebar
	d->AddRectFilled(pos, max, C(col::window), WINDOW_ROUNDING);
	d->AddRectFilled(pos, ImVec2(pos.x + SIDEBAR_WIDTH, max.y), C(col::sidebar), WINDOW_ROUNDING, ImDrawFlags_RoundCornersLeft);
	d->AddLine(ImVec2(pos.x + SIDEBAR_WIDTH, pos.y), ImVec2(pos.x + SIDEBAR_WIDTH, max.y), C(col::border));

	d->AddRect(pos, max, C(col::border), WINDOW_ROUNDING, 0, 1.f);
}

void Menu::RenderSidebar() {
	auto d = ImGui::GetWindowDrawList();

	// Brand
	auto logo_min = pos + ImVec2(S(18.f), S(20.f));
	auto logo_max = logo_min + ImVec2(S(38.f), S(38.f));

	// The picture built into the program, the "CS" square when it cannot be made
	static ImTextureID logo = ImageCache::FromMemory(logo_image, logo_image_size);

	if (logo) {
		d->AddImageRounded(ImTextureRef(logo), logo_min, logo_max, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE), S(9.f));
		d->AddRect(logo_min, logo_max, C(col::border), S(9.f), 0, 1.f);
	}
	else {
		d->AddRectFilled(logo_min, logo_max, C(col::accent), S(9.f));

		auto logo_text_size = font_bold->CalcTextSizeA(S(15.f), FLT_MAX, 0.f, "CS");
		d->AddText(font_bold, S(15.f), logo_min + (ImVec2(S(38.f), S(38.f)) - logo_text_size) * 0.5f, C(col::on_accent), "CS");
	}

	d->AddText(font_bold, S(15.f), ImVec2(logo_max.x + S(11.f), logo_min.y + S(2.f)), C(col::text), BRAND_TITLE);
	d->AddText(font_regular, S(12.f), ImVec2(logo_max.x + S(11.f), logo_min.y + S(21.f)), C(col::text_dim), BRAND_SUBTITLE);

	// Tabs
	struct Entry {
		const char* caption; // Section title instead of a tab when set
		Tab tab;
		const char* label;
		const char* icon;
	};

	const Entry entries[] = {
		{ "VISUALS", {}, nullptr, nullptr },
		{ nullptr, Tab::PLAYERS, "Players", Icons::PEOPLE },
		{ nullptr, Tab::BOMB, "Bomb", WeaponIcons::C4 },
		{ nullptr, Tab::PROJECTILES, "Projectiles", WeaponIcons::SMOKEGRENADE },
		{ nullptr, Tab::ITEMS, "Items", WeaponIcons::DEAGLE },
		{ "INVENTORY", {}, nullptr, nullptr },
		{ nullptr, Tab::SKINS, "Skins", WeaponIcons::AK47 },
		{ "COMMON", {}, nullptr, nullptr },
		{ nullptr, Tab::MOVEMENT, "Movement", Icons::PERSON },
		{ nullptr, Tab::MISC, "Misc", Icons::GLOBE },
		{ nullptr, Tab::CONFIGS, "Configs", Icons::RELOAD },
		{ nullptr, Tab::SETTINGS, "Settings", Icons::SETTINGS },
	};

	const float margin = S(12.f);
	const ImVec2 item_size(SIDEBAR_WIDTH - margin * 2.f, S(36.f));

	// Layout first, so the highlight can be drawn below the items
	float tab_y[16]{};
	float y = pos.y + S(82.f);

	for (const auto& entry : entries) {
		if (entry.caption) {
			y += S(26.f);
			continue;
		}

		tab_y[entry.tab] = y;
		y += item_size.y + S(3.f);
	}

	// Highlight sliding towards the active tab
	float target = tab_y[active_tab] - pos.y;
	if (indicator_y < 0.f)
		indicator_y = target;
	indicator_y += (target - indicator_y) * std::min(1.f, ImGui::GetIO().DeltaTime * 14.f);

	auto highlight_min = ImVec2(pos.x + margin, pos.y + indicator_y);
	auto highlight_max = highlight_min + item_size;

	d->AddRectFilled(highlight_min, highlight_max, C(col::selected), S(8.f));

	auto bar_min = ImVec2(highlight_min.x - margin, highlight_min.y + S(11.f));
	d->AddRectFilled(bar_min, ImVec2(bar_min.x + S(2.f), highlight_max.y - S(11.f)), C(col::accent), 1.f);

	// Items
	y = pos.y + S(82.f);

	for (const auto& entry : entries) {
		if (entry.caption) {
			d->AddText(font_bold, S(11.f), ImVec2(pos.x + margin + S(14.f), y + S(8.f)), C(col::text_faint), entry.caption);
			y += S(26.f);
			continue;
		}

		if (SidebarItem(entry.icon, entry.label, active_tab == entry.tab, ImVec2(pos.x + margin, y), item_size) && active_tab != entry.tab) {
			active_tab = entry.tab;
			tab_progress = 0.f;
		}

		y += item_size.y + S(3.f);
	}
}

void Menu::RenderHeader() {
	auto d = ImGui::GetWindowDrawList();

	const char* title = "";

	switch (active_tab) {
	case Tab::PLAYERS:		title = "Players";		break;
	case Tab::BOMB:			title = "Bomb";			break;
	case Tab::ITEMS:		title = "Items";		break;
	case Tab::PROJECTILES:	title = "Projectiles";	break;
	case Tab::SKINS:		title = "Skins";		break;
	case Tab::MOVEMENT:		title = "Movement";		break;
	case Tab::MISC:			title = "Misc";			break;
	case Tab::CONFIGS:		title = "Configs";		break;
	case Tab::SETTINGS:		title = "Settings";		break;
	}

	// Title slides in with the content, in the middle of the header
	float tab = EaseOutCubic(tab_progress);
	float x = pos.x + SIDEBAR_WIDTH + CONTENT_PADDING;
	float slide = (1.f - tab) * 10.f;

	d->AddText(font_bold, S(21.f), ImVec2(x + slide, pos.y + (HEADER_HEIGHT - S(21.f)) * 0.5f), C(col::text, tab), title);

	// Underline growing under the title
	auto title_width = font_bold->CalcTextSizeA(S(21.f), FLT_MAX, 0.f, title).x;
	d->AddRectFilled(ImVec2(x, pos.y + HEADER_HEIGHT - S(2.f)), ImVec2(x + title_width * tab, pos.y + HEADER_HEIGHT), C(col::accent));

	d->AddLine(
		ImVec2(pos.x + SIDEBAR_WIDTH, pos.y + HEADER_HEIGHT),
		ImVec2(pos.x + size.x, pos.y + HEADER_HEIGHT),
		C(col::border)
	);

	// Right side: the DPI scale of the menu, then the notifications switch left of it
	const float pill_height = S(34.f);
	const float pill_y = pos.y + (HEADER_HEIGHT - pill_height) * 0.5f;
	float right = pos.x + size.x - CONTENT_PADDING;

	auto pill = [&](ImVec2 min, ImVec2 pill_size) {
		d->AddRectFilled(min, min + pill_size, C(col::panel), pill_size.y * 0.5f);
		d->AddRect(min, min + pill_size, C(col::border), pill_size.y * 0.5f);
	};

	// DPI scale, steps of 5%. Applied the same way as before, once the mouse is let go
	{
		constexpr auto label = "DPI";
		auto label_size = ImGui::CalcTextSize(label);
		auto value = std::format("{:.0f}%", std::round(cfg::settings::ui_scale * 20.f) * 5.f);
		auto value_size = ImGui::CalcTextSize("160%");

		const float button = S(24.f);
		auto pill_size = ImVec2(S(14.f) + label_size.x + S(10.f) + button + S(6.f) + value_size.x + S(6.f) + button + S(5.f), pill_height);
		auto min = ImVec2(right - pill_size.x, pill_y);
		pill(min, pill_size);

		float text_y = min.y + (pill_height - label_size.y) * 0.5f;
		d->AddText(ImVec2(min.x + S(14.f), text_y), C(col::text_dim), label);

		auto step_button = [&](const char* id, const char* sign, ImVec2 at, float delta) {
			ImGui::SetCursorScreenPos(at);
			bool pressed = ImGui::InvisibleButton(id, ImVec2(button, button));
			float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);

			auto center = at + ImVec2(button, button) * 0.5f;
			d->AddCircleFilled(center, button * 0.5f, C(LerpColor(col::track, col::track_hover, hover)), 20);

			auto sign_size = ImGui::CalcTextSize(sign);
			d->AddText(center - sign_size * 0.5f, C(LerpColor(col::text_dim, col::accent, hover)), sign);

			if (pressed)
				cfg::settings::ui_scale = std::clamp(std::round((cfg::settings::ui_scale + delta) * 20.f) / 20.f, 0.8f, 1.6f);
		};

		float x = min.x + S(14.f) + label_size.x + S(10.f);
		float button_y = min.y + (pill_height - button) * 0.5f;
		step_button("##scale_down", "-", ImVec2(x, button_y), -0.05f);

		x += button + S(6.f);
		auto shown_size = ImGui::CalcTextSize(value.c_str());
		d->AddText(ImVec2(x + (value_size.x - shown_size.x) * 0.5f, text_y), C(col::text), value.c_str());

		x += value_size.x + S(6.f);
		step_button("##scale_up", "+", ImVec2(x, button_y), 0.05f);

		right = min.x - S(8.f);
	}

	// Notifications: map building, notices of the program, the ESP off message
	{
		constexpr auto label = "Notifications";
		auto label_size = ImGui::CalcTextSize(label);

		auto pill_size = ImVec2(TOGGLE_SIZE.x + label_size.x + S(44.f), pill_height);
		auto min = ImVec2(right - pill_size.x, pill_y);
		pill(min, pill_size);

		float on = Animate(ImGui::GetID("##notifications_state"), cfg::settings::notifications ? 1.f : 0.f, 10.f);
		auto dot = ImVec2(min.x + S(15.f), min.y + pill_height * 0.5f);

		d->AddCircleFilled(dot, S(3.5f), LerpColor(col::text_faint, col::accent, on), 16);
		d->AddText(ImVec2(dot.x + S(10.f), min.y + (pill_height - label_size.y) * 0.5f), LerpColor(col::text_dim, col::text, on), label);

		auto switch_pos = ImVec2(min.x + pill_size.x - TOGGLE_SIZE.x - S(8.f), min.y + (pill_height - TOGGLE_SIZE.y) * 0.5f);
		ToggleSwitch("##notifications", &cfg::settings::notifications, switch_pos);
	}
}

void Menu::RenderPlayersPreview() {
	auto& group = esp_group == 0 ? cfg::esp::team : cfg::esp::enemy;

	// The layout of the flags around a preview of the player
	ImGui::BeginDisabled(!group.enabled);
	{
		BeginPanel("LAYOUT");

		auto d = ImGui::GetWindowDrawList();
		auto preview_min = ImGui::GetCursorScreenPos();
		auto preview_size = ImVec2(ImGui::GetContentRegionAvail().x, S(370.f));
		auto preview_max = preview_min + preview_size;

		d->AddRectFilled(preview_min, preview_max, C(col::window), S(8.f));

		// Box in the middle, room around it for the flags
		auto box_size = ImVec2(floorf(preview_size.x * 0.36f), floorf(preview_size.y * 0.66f));
		auto box_min = ImVec2(floorf(preview_min.x + (preview_size.x - box_size.x) * 0.5f), floorf(preview_min.y + preview_size.y * 0.16f));
		auto box_max = box_min + box_size;

		auto payload = ImGui::GetDragDropPayload();
		bool dragging = payload && payload->IsDataType(FLAG_PAYLOAD);
		auto mouse = ImGui::GetIO().MousePos;

		d->PushClipRect(preview_min, preview_max, true);

		// Enemies are the other team of ours, T when we are not in a game
		int local_team = Cache::CopySnapshot().local.team;
		bool terrorist = esp_group == 0 ? local_team == 2 : local_team != 2;

		// Stands idle facing us, drag to turn it
		ImGui::SetCursorScreenPos(box_min);
		ImGui::InvisibleButton("##model", box_size);

		if (ImGui::IsItemActive())
			model_yaw += ImGui::GetIO().MouseDelta.x * 0.012f;

		if (ImGui::IsItemHovered() && !ImGui::IsItemActive() && !dragging)
			ImGui::SetTooltip("Drag to turn the model");

		// The pre-rendered picture, the plain model when the picture cannot be made, a figure without both
		auto model_min = box_min + ImVec2(0.f, box_size.y * 0.04f), model_max = box_max - ImVec2(0.f, box_size.y * 0.015f);
		auto glow = esp_group == 0
			? PreviewGlow(cfg::visuals::glow::team, cfg::visuals::glow::team_color)
			: PreviewGlow(cfg::visuals::glow::enemies, cfg::visuals::glow::enemy_color);

		// Textured chams multiply the model by the color. The glow of the game covers it in the color, about
		// two thirds by the strength (the alpha), the shading still showing: tinted, then the color over it
		bool chams = Visuals::IsAvailable() && (esp_group == 0 ? cfg::visuals::chams::team : cfg::visuals::chams::enemies);
		const auto& chams_color = esp_group == 0 ? cfg::visuals::chams::team_color : cfg::visuals::chams::enemy_color;
		int chams_type = Visuals::HasModelGlow()
			? (esp_group == 0 ? cfg::visuals::chams::team_type : cfg::visuals::chams::enemy_type)
			: cfg::visuals::chams::TYPE_TEXTURED;
		namespace chams_cfg = cfg::visuals::chams;
		bool chams_glow = chams && chams_type != chams_cfg::TYPE_TEXTURED;
		ImU32 tint = 0;
		if (chams && chams_type != chams_cfg::TYPE_GLOW)
			tint = ImGui::GetColorU32(ImVec4(chams_color.r, chams_color.g, chams_color.b, 1.f));
		float coat_alpha = 0.45f * chams_color.a;
		ImU32 coat = chams_glow ? ImGui::GetColorU32(ImVec4(chams_color.r, chams_color.g, chams_color.b, coat_alpha)) : 0;

		if (!DrawPlayerPicture(d, terrorist, model_min, model_max, model_yaw, glow, tint, coat)) {
			if (auto model = GetPreviewModel(terrorist ? "t" : "ct"))
				DrawModel(d, *model, model_min, model_max, model_yaw, chams ? ImGui::ColorConvertFloat4ToU32(ImVec4(chams_color.r, chams_color.g, chams_color.b, 1.f)) : IM_COL32(205, 205, 212, 255), glow);
			else {
				auto figure_inset = ImVec2(box_size.x * 0.14f, box_size.y * 0.05f);
				DrawFigure(d, box_min + figure_inset, box_max - figure_inset, C(col::text_faint, 0.9f));
			}
		}

		// What the ESP draws on a player, with the same code
		auto [preview_local, preview_player] = PreviewPlayers();

		if (group.box)
			Esp::DrawBox(d, box_min, box_max, group.box_visible);
		else if (dragging)
			d->AddRect(box_min, box_max, C(col::border_hover), 0.f, 0, 1.f);

		if (group.head_tracker)
			Esp::DrawTracker(d, ImVec2(box_min.x + box_size.x * 0.5f, box_min.y + box_size.y * 0.085f), box_size.x, group.tracker_visible);

		std::vector<Esp::FlagArea> areas;
		Esp::DrawFlags(d, preview_local, preview_player, box_min, box_max, group, true, &areas);

		d->PopClipRect();

		std::optional<std::pair<int, int>> moved;	// Flag & side it was dropped on
		int insert_at = 0;
		std::optional<int> removed;

		// The drawn flags are what gets dragged
		for (const auto& area : areas) {
			auto rect = area.rect;
			rect.Expand(ImVec2(S(3.f), S(2.f)));	// Bars are thin
			rect.ClipWith(ImRect(preview_min, preview_max));

			if (rect.GetWidth() < 1.f || rect.GetHeight() < 1.f)
				continue;

			ImGui::PushID(area.flag);
			ImGui::SetCursorScreenPos(rect.Min);
			ImGui::InvisibleButton("##flag", rect.GetSize());
			bool hovered = ImGui::IsItemHovered();

			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
				removed = area.flag;

			bool dragged = false;
			if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
				ImGui::SetDragDropPayload(FLAG_PAYLOAD, &area.flag, sizeof(area.flag));
				dragged = true;
				ImGui::EndDragDropSource();
			}

			if (dragged) {
				d->AddRect(rect.Min, rect.Max, C(col::accent, 0.5f), S(3.f));
				DraggedChip(area.flag);
			}
			else if (hovered && !dragging) {
				d->AddRect(rect.Min, rect.Max, C(col::accent, 0.9f), S(3.f));
				ImGui::SetTooltip("%s\nDrag to move, right click to remove", FlagName(area.flag));
			}

			ImGui::PopID();
		}

		// Small strips next to the box to drop on, grown over the flags already there
		const float strip = S(26.f), side_strip = S(40.f), gap = S(3.f);
		ImRect zones[cfg::esp::SIDE_COUNT] = {
			ImRect(ImVec2(box_min.x - side_strip, box_min.y - strip), ImVec2(box_max.x + side_strip, box_min.y - gap)),
			ImRect(ImVec2(box_min.x - side_strip, box_max.y + gap), ImVec2(box_max.x + side_strip, box_max.y + strip)),
			ImRect(ImVec2(box_min.x - side_strip, box_min.y), ImVec2(box_min.x - gap, box_max.y)),
			ImRect(ImVec2(box_max.x + gap, box_min.y), ImVec2(box_max.x + side_strip, box_max.y)),
		};

		for (const auto& area : areas)
			zones[area.side].Add(ImRect(area.rect.Min - ImVec2(gap, gap), area.rect.Max + ImVec2(gap, gap)));

		const char* zone_ids[cfg::esp::SIDE_COUNT] = { "##zone_top", "##zone_bottom", "##zone_left", "##zone_right" };

		for (int side = 0; side < cfg::esp::SIDE_COUNT; side++) {
			auto zone = zones[side];
			zone.ClipWith(ImRect(preview_min, preview_max));

			// Only there while something is dragged
			bool hovered = dragging && zone.Contains(mouse);
			float h = Animate(ImGui::GetID(zone_ids[side]), hovered ? 1.f : dragging ? 0.45f : 0.f, 14.f);

			if (h > 0.01f) {
				d->AddRectFilled(zone.Min, zone.Max, C(col::accent, 0.08f * h), S(4.f));
				d->AddRect(zone.Min, zone.Max, C(col::accent, 0.8f * h), S(4.f));
			}

			if (!dragging || !ImGui::BeginDragDropTargetCustom(zone, ImGui::GetID(zone_ids[side])))
				continue;

			// Dropped in front of the first flag further from the box than the mouse
			if (auto accepted = ImGui::AcceptDragDropPayload(FLAG_PAYLOAD)) {
				int index = 0;

				for (const auto& area : areas) {
					if (area.side != side)
						continue;

					auto center = area.rect.GetCenter();
					bool bar = IsBarFlag(area.flag);
					bool before = false;

					switch (side) {
					case cfg::esp::SIDE_TOP:	before = center.y > mouse.y; break;
					case cfg::esp::SIDE_BOTTOM:	before = center.y < mouse.y; break;
					case cfg::esp::SIDE_LEFT:	before = bar ? center.x > mouse.x : center.y < mouse.y; break;
					default:					before = bar ? center.x < mouse.x : center.y < mouse.y; break;
					}

					if (before)
						index = std::max(index, area.index + 1);
				}

				moved = { *static_cast<const int*>(accepted->Data), side };
				insert_at = index;
			}

			ImGui::EndDragDropTarget();
		}

		ImGui::SetCursorScreenPos(ImVec2(preview_min.x, preview_max.y));
		ImGui::Dummy(ImVec2(preview_size.x, S(12.f)));

		// Flags that are not placed, dropping one here removes it
		auto palette_title = ImGui::GetCursorScreenPos();
		d->AddText(font_bold, S(11.f), palette_title, C(col::text_faint), "AVAILABLE");
		ImGui::Dummy(ImVec2(preview_size.x, S(18.f)));

		std::vector<int> unused;
		for (int flag = 0; flag < cfg::esp::FLAG_COUNT; flag++) {
			if (flag == cfg::esp::FLAG_HEALTH)
				continue;

			bool placed = false;
			for (const auto& side : group.layout)
				placed |= std::find(side.begin(), side.end(), flag) != side.end();

			if (!placed)
				unused.push_back(flag);
		}

		auto palette_min = ImGui::GetCursorScreenPos();
		auto palette_positions = LayoutChips(unused, palette_min - ImVec2(0.f, S(4.f)), palette_min + ImVec2(preview_size.x, 0.f), false);

		float palette_height = CHIP_HEIGHT;
		for (const auto& p : palette_positions)
			palette_height = std::max(palette_height, p.y + CHIP_HEIGHT - palette_min.y);

		auto palette_max = palette_min + ImVec2(preview_size.x, palette_height);
		std::optional<int> added;

		if (unused.empty()) {
			auto text = "Every flag is placed";
			auto text_size = ImGui::CalcTextSize(text);
			d->AddText(ImVec2(palette_min.x + (preview_size.x - text_size.x) * 0.5f, palette_min.y + (CHIP_HEIGHT - text_size.y) * 0.5f), C(col::text_faint), text);
		}

		for (size_t i = 0; i < unused.size(); i++)
			if (FlagChip(unused[i], palette_positions[i], false) == 1)
				added = unused[i];

		bool palette_hovered = dragging && ImRect(palette_min, palette_max).Contains(mouse);
		if (palette_hovered)
			d->AddRect(palette_min - ImVec2(S(4.f), S(4.f)), palette_max + ImVec2(S(4.f), S(4.f)), C(col::accent, 0.6f), S(6.f));

		if (ImGui::BeginDragDropTargetCustom(ImRect(palette_min - ImVec2(S(4.f), S(4.f)), palette_max + ImVec2(S(4.f), S(4.f))), ImGui::GetID("##palette"))) {
			if (auto accepted = ImGui::AcceptDragDropPayload(FLAG_PAYLOAD))
				removed = *static_cast<const int*>(accepted->Data);

			ImGui::EndDragDropTarget();
		}

		ImGui::SetCursorScreenPos(ImVec2(palette_min.x, palette_max.y));
		ImGui::Dummy(ImVec2(preview_size.x, S(4.f)));

		// Changes after drawing, the lists were in use
		if (moved) {
			auto [flag, side] = *moved;
			auto& target = group.layout[side];

			// Moving within the same side, the index shifts once the flag is taken out
			auto current = std::find(target.begin(), target.end(), flag);
			if (current != target.end() && std::distance(target.begin(), current) < insert_at)
				insert_at--;

			RemoveFlag(group, flag);
			target.insert(target.begin() + std::clamp(insert_at, 0, static_cast<int>(target.size())), flag);
		}
		else if (removed) {
			RemoveFlag(group, *removed);
		}
		else if (added) {
			// Clicked, goes where it usually is
			int flag = *added;
			int side = cfg::esp::SIDE_RIGHT;

			if (IsBarFlag(flag))
				side = cfg::esp::SIDE_LEFT;
			else if (flag == cfg::esp::FLAG_NAME)
				side = cfg::esp::SIDE_TOP;
			else if (flag == cfg::esp::FLAG_WEAPON || flag == cfg::esp::FLAG_WEAPON_NAME || flag == cfg::esp::FLAG_AMMO || flag == cfg::esp::FLAG_DISTANCE)
				side = cfg::esp::SIDE_BOTTOM;

			group.layout[side].push_back(flag);
		}

		EndPanel();
	}
	ImGui::EndDisabled();
}

void Menu::RenderPlayersTab() {
	auto& group = esp_group == 0 ? cfg::esp::team : cfg::esp::enemy;

	// The whole content area, like the skins tab
	auto origin = ImVec2(column_layout.x, column_layout.y);
	auto region = ImVec2(column_layout.w * 2.f + COLUMN_SPACING, column_layout.h);
	auto toolbar = ImGui::GetWindowPos() + origin;
	const float toolbar_height = S(46.f);

	// Toolbar: group tabs on the left, the switch of the group on the right
	TabSwitch("##esp_group", &esp_group, { "Team", "Enemy" }, toolbar);

	{
		auto label = esp_group == 0 ? "Team ESP" : "Enemy ESP";
		auto label_size = ImGui::CalcTextSize(label);
		auto switch_pos = ImVec2(toolbar.x + region.x - TOGGLE_SIZE.x - S(2.f), toolbar.y + (S(32.f) - TOGGLE_SIZE.y) * 0.5f);

		ToggleSwitch("##group_enabled", &group.enabled, switch_pos);

		ImGui::GetWindowDrawList()->AddText(
			ImVec2(switch_pos.x - label_size.x - S(10.f), toolbar.y + (S(32.f) - label_size.y) * 0.5f),
			C(group.enabled ? col::text : col::text_dim),
			label
		);
	}

	// Options in two columns under the toolbar, the preview is in the panel next to the menu
	auto columns = column_layout;
	column_layout.y += toolbar_height;
	column_layout.h -= toolbar_height;

	// Left: what is drawn
	BeginColumn(0);
	ImGui::BeginDisabled(!group.enabled);
	{
		BeginPanel("ESP");
		ToggleColors("Box", &group.box, nullptr, &group.box_visible, &group.box_invisible);
		ToggleColors("Skeleton", &group.skeleton, nullptr, &group.skeleton_visible, &group.skeleton_invisible);
		ToggleColors("Head Tracker", &group.head_tracker, nullptr, &group.tracker_visible, &group.tracker_invisible);
		ToggleColors("Tracers", &group.tracers, nullptr, &group.tracer_visible, &group.tracer_invisible);

		// Written into the game
		ImGui::BeginDisabled(!Visuals::IsAvailable());
		Toggle("Outline Glow", esp_group == 0 ? &cfg::visuals::glow::team : &cfg::visuals::glow::enemies,
			Visuals::IsAvailable() ? "Outline of the game, seen through walls" : "Only available when the game is launched with -insecure",
			nullptr, esp_group == 0 ? &cfg::visuals::glow::team_color : &cfg::visuals::glow::enemy_color);
		Toggle("Chams", esp_group == 0 ? &cfg::visuals::chams::team : &cfg::visuals::chams::enemies,
			Visuals::IsAvailable()
				? "The model colored by the game itself, where it can be seen.\nThe color mixes with the model, light colors show best. Behind walls: the outline glow"
				: "Only available when the game is launched with -insecure",
			nullptr, esp_group == 0 ? &cfg::visuals::chams::team_color : &cfg::visuals::chams::enemy_color);
		if (esp_group == 0 ? cfg::visuals::chams::team : cfg::visuals::chams::enemies) {
			static const std::vector<const char*> chams_types = { "Textured", "Glow", "Textured + Glow" };
			static const char* chams_descriptions[] = {
				"The model in the color, mixed with its textures",
				"The spawn protection shader of the game in the color, at half: the model still shows",
				"Both: the model in the color with the glow over it",
			};

			// Glow needs code of the game that might not be found
			ImGui::BeginDisabled(!Visuals::HasModelGlow());
			int* type = esp_group == 0 ? &cfg::visuals::chams::team_type : &cfg::visuals::chams::enemy_type;
			DropdownRow("Chams Type", type, chams_types, chams_descriptions, Visuals::HasModelGlow()
				? chams_descriptions[std::clamp(*type, 0, cfg::visuals::chams::TYPE_COUNT - 1)]
				: "Only textured: the code of the game for the glow was not found");
			ImGui::EndDisabled();
		}
		ImGui::EndDisabled();
		Toggle("Visible Only", &group.visible_only, "Hides players behind walls & smokes");

		if (esp_group == 0 && Cache::CopySnapshot().game.deathmatch)
			TextBlock("Deathmatch: everyone is an enemy, the Enemy ESP is used for all players");
		EndPanel();

		BeginPanel("BARS");
		Toggle("Health Number", &group.health_number, "Shows the health on the health bar");
		Toggle("Armor Number", &group.armor_number, "Shows the armor on the armor bar");
		EndPanel();
	}
	ImGui::EndDisabled();
	EndColumn();

	// Right: sizes & colors of the flags
	BeginColumn(1);
	ImGui::BeginDisabled(!group.enabled);
	{
		BeginPanel("FLAG SIZES");
		SliderFloat("Text", &group.text_size, 8.f, 24.f, "%.0f px", "Name, weapon, ping, money...");
		SliderFloat("States", &group.state_size, 8.f, 24.f, "%.0f px", "FLASHED, RELOADING, SCOPED & DEFUSING");
		SliderFloat("Icons", &group.icon_size, 8.f, 28.f, "%.0f px", "Weapon, kit, C4 & the other icons");
		EndPanel();

		BeginPanel("FLAG COLORS");
		ColorRow("Text", &group.text, "Name, weapon, ping, money...");
		ColorRow("Icons", &group.icon, "Flashed, reloading, scoped & kit icons");
		ColorRow("Flashed", &group.flashed, "The FLASHED text");
		ColorRow("Reloading", &group.reloading, "The RELOADING text");
		ColorRow("Scoped", &group.scoped, "The SCOPED text");
		ColorRow("Defusing", &group.defusing, "The DEFUSING text");
		EndPanel();
	}
	ImGui::EndDisabled();
	EndColumn();

	column_layout = columns;
}

void Menu::RenderBombPreview() {
	// A planted bomb drawn like in game
	{
		BeginPanel("PREVIEW");

		auto d = ImGui::GetWindowDrawList();
		auto rect = ScenePreview("##bomb_scene", S(400.f), bomb_yaw);
		auto matrix = PreviewCamera(rect.Min, rect.Max, Vec3_t(0.f, 0.f, 1.5f), bomb_yaw, 0.5f, 27.f, 0.9f);

		d->PushClipRect(rect.Min, rect.Max, true);
		DrawPreviewGrid(d, matrix, 24.f, 4.f, IM_COL32(70, 70, 82, 255));

		Bomb bomb;
		bomb.pos = Vec3_t(0.f, 0.f, 0.f);
		bomb.is_planted = true;
		bomb.site = BombSite::A;
		bomb.time_left = 40.f - fmodf(static_cast<float>(ImGui::GetTime()), 40.f);

		// With a kit from 26s, done in time. Without one from 8s, too late
		if (bomb.time_left <= 26.f && bomb.time_left > 21.f) {
			bomb.defusing = bomb.defuse_kit = true;
			bomb.defuse_length = 5.f;
			bomb.defuse_left = bomb.time_left - 21.f;
		}
		else if (bomb.time_left <= 8.f) {
			bomb.defusing = true;
			bomb.defuse_length = 10.f;
			bomb.defuse_left = bomb.time_left + 2.f;
		}

		if (auto model = GetPreviewModel("c4"))
			DrawSceneModel(d, *model, matrix, bomb.pos, 0.f, IM_COL32(150, 140, 112, 255), true,
				PreviewGlow(cfg::visuals::glow::bomb, cfg::visuals::glow::bomb_color));

		Esp::RenderPreview(d, matrix, &bomb, {}, nullptr);

		// Under the bomb while the bomb ESP is off, where its window is otherwise
		if (cfg::world::bomb::location || cfg::world::bomb::timer) {
			auto card = Overlays::DrawBombCard(nullptr, ImVec2(), bomb);

			Vec2_t screen;
			ImVec2 card_pos = rect.Min + ImVec2(S(10.f), S(10.f));
			if (!cfg::esp::bomb && matrix.wts(bomb.pos, ImGui::GetIO().DisplaySize, screen))
				card_pos = ImVec2(floorf(screen.x - card.x * 0.5f), floorf(screen.y + 4.f));

			Overlays::DrawBombCard(d, card_pos, bomb);
		}

		d->PopClipRect();
		EndPanel();
	}
}

void Menu::RenderBombTab() {
	// What is drawn, the preview is in the panel next to the menu
	BeginColumn(0);
	{
		BeginPanel("BOMB ESP");
		Toggle("Bomb ESP", &cfg::esp::bomb, "Box & icon on the planted bomb", nullptr, &cfg::esp::colors::bomb);

		ImGui::BeginDisabled(!Visuals::IsAvailable());
		Toggle("Outline Glow", &cfg::visuals::glow::bomb, Visuals::IsAvailable()
			? "Outline of the game around the planted bomb, seen through walls.\nThe dropped bomb: in the items tab"
			: "Only available when the game is launched with -insecure", nullptr, &cfg::visuals::glow::bomb_color);
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("BOMB WINDOW");
		Toggle("Bomb Location", &cfg::world::bomb::location, "Site the bomb was planted on");
		Toggle("Bomb Timer", &cfg::world::bomb::timer, "Time left until it explodes");
		EndPanel();
	}
	EndColumn();
}

void Menu::RenderProjectilesPreview() {
	// A smoke, a fire, a grenade in the air & a throw preview, drawn like in game
	{
		BeginPanel("PREVIEW");

		auto d = ImGui::GetWindowDrawList();
		auto rect = ScenePreview("##projectile_scene", S(400.f), projectile_yaw);
		auto matrix = PreviewCamera(rect.Min, rect.Max, Vec3_t(0.f, -40.f, 20.f), projectile_yaw, 0.75f, 760.f, 0.95f);
		float time = static_cast<float>(ImGui::GetTime());

		d->PushClipRect(rect.Min, rect.Max, true);
		DrawPreviewGrid(d, matrix, 400.f, 50.f, IM_COL32(70, 70, 82, 255));

		std::vector<Grenade> grenades;

		Grenade smoke;
		smoke.type = GrenadeType::Smoke;
		smoke.pos = Vec3_t(-160.f, 90.f, 0.f);
		smoke.detonated = true;
		smoke.duration = 20.f;
		smoke.time_left = 20.f - fmodf(time, 20.f);
		// The shapes stay the same, made once
		static const auto smoke_area = PreviewArea(smoke.pos.x, smoke.pos.y, 135.f, 12.f, 16.f, 120.f);
		smoke.area = smoke_area;
		grenades.push_back(smoke);

		Grenade fire;
		fire.type = GrenadeType::Fire;
		fire.pos = Vec3_t(170.f, 20.f, 0.f);
		fire.detonated = true;
		fire.duration = 7.f;
		fire.time_left = 7.f - fmodf(time, 7.f);
		static const auto fire_area = PreviewArea(fire.pos.x, fire.pos.y, 95.f, 28.f, 16.f, 0.f);
		fire.area = fire_area;
		grenades.push_back(fire);

		// HE flying over, its trail behind it
		Grenade he;
		he.type = GrenadeType::HE;
		float flight = fmodf(time, 2.4f) / 2.4f;
		auto arc = [](float t) { return Vec3_t(-320.f + 420.f * t, -260.f + 60.f * t, 40.f + 260.f * t * (1.f - t)); };

		for (int i = 0; i <= 24; i++)
			he.trail.push_back(arc(flight * i / 24.f));
		he.pos = arc(flight);
		grenades.push_back(he);

		// Flash in hand, bouncing off the ground
		GrenadePath path;
		path.valid = true;
		path.type = GrenadeType::Flash;
		path.on_ground = true;
		path.time = 1.6f;

		Vec3_t start(260.f, -330.f, 64.f), bounce(140.f, -150.f, 0.f), end(95.f, -85.f, 0.f);
		for (int i = 0; i <= 30; i++) {
			float t = i / 30.f;
			path.points.push_back(Vec3_t(start.x + (bounce.x - start.x) * t, start.y + (bounce.y - start.y) * t, start.z * (1.f - t) + 140.f * t * (1.f - t)));
		}
		for (int i = 1; i <= 10; i++) {
			float t = i / 10.f;
			path.points.push_back(Vec3_t(bounce.x + (end.x - bounce.x) * t, bounce.y + (end.y - bounce.y) * t, 30.f * t * (1.f - t)));
		}
		path.bounces.push_back(bounce);
		path.end = end;

		// The HE in the air, as an icon where its model would be
		DrawStandIn(d, matrix, he.pos, WeaponIcons::FRAG_GRENADE, S(16.f),
			PreviewGlow(cfg::visuals::glow::thrown, cfg::visuals::glow::thrown_colors::he));

		Esp::RenderPreview(d, matrix, nullptr, grenades, &path);

		d->PopClipRect();
		EndPanel();
	}
}

void Menu::RenderProjectilesTab() {
	// What is drawn, the preview is in the panel next to the menu
	BeginColumn(0);
	{
		BeginPanel("GRENADES");
		Toggle("Grenade ESP", &cfg::esp::grenades::enabled, "Smokes, molotovs, flashes, HEs & decoys");
		ImGui::BeginDisabled(!cfg::esp::grenades::enabled);
		Toggle("Trails", &cfg::esp::grenades::trails, "Path of grenades that are still in the air", nullptr, &cfg::esp::colors::grenades::trail);
		ImGui::BeginDisabled(!cfg::esp::grenades::trails);
		Toggle("Grenade Color Trails", &cfg::esp::grenades::trail_type_color, "Color each trail like its grenade instead of using the trail color");
		ImGui::EndDisabled();
		Toggle("Throw Preview", &cfg::esp::grenades::prediction, "Path, bounces & landing spot of the grenade in your hand\nUses the map collision, built from the game files the first time a map is played");
		Toggle("Landing Prediction", &cfg::esp::grenades::landing, "Where grenades in the air are going to land or explode, thrown by anyone\nUses the map collision too");
		ImGui::EndDisabled();

		ImGui::BeginDisabled(!Visuals::IsAvailable());
		Toggle("Outline Glow", &cfg::visuals::glow::thrown, Visuals::IsAvailable()
			? "Outline of the game around grenades in the air, seen through walls.\nGrenades on the ground: in the items tab"
			: "Only available when the game is launched with -insecure");
		if (cfg::visuals::glow::thrown) {
			namespace thrown = cfg::visuals::glow::thrown_colors;
			ColorRow("Smoke Glow", &thrown::smoke);
			ColorRow("Molotov Glow", &thrown::molotov, "Also the incendiary");
			ColorRow("Flash Glow", &thrown::flash);
			ColorRow("HE Glow", &thrown::he);
			ColorRow("Decoy Glow", &thrown::decoy);
		}
		ImGui::EndDisabled();
		EndPanel();

		BeginPanel("LOOK");
		ImGui::BeginDisabled(!cfg::esp::grenades::enabled);
		Toggle("Smoke Area", &cfg::esp::grenades::smoke_area, "Outline & fill of a popped smoke on the ground.\nIts icon & timer stay");
		Toggle("Fire Area", &cfg::esp::grenades::fire_area, "Outline & fill of a molotov or incendiary fire on the ground.\nIts icon & timer stay");
		Toggle("Glow", &cfg::esp::grenades::glow, "Popped smokes & burning fires glow on the ground");
		Toggle("Icons", &cfg::esp::grenades::icons, "Icon of the grenade above it");
		Toggle("Names", &cfg::esp::grenades::names, "Smoke, Molotov, Flash...");
		Toggle("Timers", &cfg::esp::grenades::timers, "Time left of smokes & fires");
		ImGui::BeginDisabled(!cfg::esp::grenades::timers);
		Toggle("Timer Bars", &cfg::esp::grenades::timer_bars, "Bar under the time left");
		ImGui::EndDisabled();
		SliderFloat("Text Size", &cfg::esp::grenades::text_size, 8.f, 28.f, "%.0f px", "Size of the names & timers");
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("COLORS");
		ImGui::BeginDisabled(!cfg::esp::grenades::enabled);
		ColorRow("Smoke", &cfg::esp::colors::grenades::smoke);
		ColorRow("Molotov", &cfg::esp::colors::grenades::molotov, "Also used for the fire area");
		ColorRow("Flash", &cfg::esp::colors::grenades::flash);
		ColorRow("HE", &cfg::esp::colors::grenades::he);
		ColorRow("Decoy", &cfg::esp::colors::grenades::decoy);
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();
}

void Menu::RenderItemsPreview() {
	// A few things lying on the floor, drawn like in game
	{
		BeginPanel("PREVIEW");

		auto d = ImGui::GetWindowDrawList();
		auto rect = ScenePreview("##items_scene", S(400.f), items_yaw);
		auto matrix = PreviewCamera(rect.Min, rect.Max, Vec3_t(0.f, 0.f, 10.f), items_yaw, 0.65f, 520.f, 0.95f);

		d->PushClipRect(rect.Min, rect.Max, true);
		DrawPreviewGrid(d, matrix, 240.f, 40.f, IM_COL32(70, 70, 82, 255));

		auto make = [](ItemKind kind, int index, const char* name, const char* icon, Vec3_t pos, int ammo) {
			Item item;
			item.kind = kind;
			item.item_index = index;
			item.name = name;
			item.icon = icon;
			item.pos = pos;
			item.ammo = ammo;
			return item;
		};

		std::vector<Item> items = {
			make(ItemKind::Weapon, weapon_ak47, "AK-47", WeaponIcons::AK47, Vec3_t(-130.f, 90.f, 0.f), 30),
			make(ItemKind::Weapon, weapon_awp, "AWP", WeaponIcons::AWP, Vec3_t(120.f, 130.f, 0.f), 5),
			make(ItemKind::Utility, weapon_smokegrenade, "Smoke Grenade", WeaponIcons::SMOKEGRENADE, Vec3_t(-150.f, -110.f, 0.f), -1),
			make(ItemKind::Utility, weapon_flashbang, "Flashbang", WeaponIcons::FLASHBANG, Vec3_t(-40.f, -160.f, 0.f), -1),
			make(ItemKind::Bomb, weapon_c4, "C4", WeaponIcons::C4, Vec3_t(30.f, 10.f, 0.f), -1),
			make(ItemKind::Kit, 0, "Defuse Kit", WeaponIcons::CUTTERS, Vec3_t(150.f, -90.f, 0.f), -1),
		};

		// The dropped bomb with its model, the rest as icons on the floor. Each kind with its own glow
		if (auto model = GetPreviewModel("c4"))
			DrawSceneModel(d, *model, matrix, items[4].pos, 0.4f, IM_COL32(150, 140, 112, 255), true,
				PreviewGlow(cfg::visuals::glow::dropped_bomb, cfg::visuals::glow::dropped_bomb_color));

		for (const auto& item : items) {
			if (item.kind == ItemKind::Bomb)
				continue;

			auto glow = item.kind == ItemKind::Utility
				? PreviewGlow(cfg::visuals::glow::utility, cfg::visuals::glow::utility_color)
				: PreviewGlow(cfg::visuals::glow::items, cfg::visuals::glow::item_color);
			DrawStandIn(d, matrix, item.pos, item.icon, S(22.f), glow);
		}

		Esp::RenderPreview(d, matrix, nullptr, {}, nullptr, &items, Vec3_t(-420.f, -380.f, 64.f));

		d->PopClipRect();
		EndPanel();
	}
}

void Menu::RenderItemsTab() {
	namespace it = cfg::esp::items;

	// What is drawn, the preview is in the panel next to the menu
	auto category_panel = [&](const char* title, const char* label, it::category_t& category, bool has_ammo) {
		BeginPanel(title);
		ImGui::PushID(title);
		Toggle(label, &category.enabled, nullptr, nullptr, &category.color);
		ImGui::BeginDisabled(!category.enabled);
		Toggle("Icon", &category.icon);
		Toggle("Name", &category.name, "Always shown when the icon is off");
		if (has_ammo)
			Toggle("Ammo", &category.ammo, "Bullets left in the magazine");
		Toggle("Distance", &category.distance);
		ImGui::EndDisabled();
		ImGui::PopID();
		EndPanel();
	};

	BeginColumn(0);
	{
		BeginPanel("ITEMS");
		Toggle("Item ESP", &it::enabled, "Weapons, grenades, the bomb & defuse kits nobody holds");
		ImGui::BeginDisabled(!it::enabled);
		SliderFloat("Max Distance", &it::max_distance, 5.f, 200.f, "%.0f m", "Items further away are not drawn");
		SliderFloat("Text Size", &it::text_size, 8.f, 24.f, "%.0f px");
		SliderFloat("Icon Size", &it::icon_size, 8.f, 28.f, "%.0f px");
		ImGui::EndDisabled();

		ImGui::BeginDisabled(!Visuals::IsAvailable());
		Toggle("Weapon Outline Glow", &cfg::visuals::glow::items, Visuals::IsAvailable()
			? "Outline of the game around weapons & defuse kits on the ground, seen through walls"
			: "Only available when the game is launched with -insecure", nullptr, &cfg::visuals::glow::item_color);
		Toggle("Utility Outline Glow", &cfg::visuals::glow::utility, Visuals::IsAvailable()
			? "Outline of the game around grenades on the ground, seen through walls.\nThrown grenades: in the projectiles tab"
			: "Only available when the game is launched with -insecure", nullptr, &cfg::visuals::glow::utility_color);
		Toggle("Bomb Outline Glow", &cfg::visuals::glow::dropped_bomb, Visuals::IsAvailable()
			? "Outline of the game around the dropped bomb, seen through walls.\nThe planted bomb: in the bomb tab"
			: "Only available when the game is launched with -insecure", nullptr, &cfg::visuals::glow::dropped_bomb_color);
		ImGui::EndDisabled();
		EndPanel();

		ImGui::BeginDisabled(!it::enabled);
		category_panel("WEAPONS", "Weapons", it::weapons, true);
		ImGui::EndDisabled();
	}
	EndColumn();

	BeginColumn(1);
	{
		ImGui::BeginDisabled(!it::enabled);
		category_panel("UTILITY", "Grenades", it::utility, false);
		category_panel("BOMB", "Dropped Bomb", it::bomb, false);
		category_panel("KITS", "Defuse Kits", it::kits, false);
		ImGui::EndDisabled();
	}
	EndColumn();
}

void Menu::OpenSkinPage(SkinPage page, int item) {
	skin_page = page;
	selected_item = item;
	skin_search[0] = '\0';
	skin_page_progress = 0.f;
}

void Menu::RenderSkinsTab() {
	static const std::vector<ItemInfo> no_items;

	const bool available = Skins::IsAvailable();
	const bool loaded = Skins::IsLoaded();
	const auto& all_items = loaded ? Skins::GetItems() : no_items;

	// Item in hand, copied before locking as the cache has its own lock
	auto local = Cache::CopySnapshot().local;
	int in_hand = local.weapon.item_index > 0 ? local.weapon.item_index : 0;

	std::lock_guard<std::mutex> lock(cfg::skins::mutex);
	auto& loadout = cfg::skins::loadouts[skin_team];

	auto assigned_skin = [&](const ItemInfo& item) -> const SkinInfo* {
		auto it = loadout.items.find(item.definition_index);
		return it != loadout.items.end() && it->second.paint_kit ? item.FindSkin(it->second.paint_kit) : nullptr;
	};

	skin_page_progress = std::min(1.f, skin_page_progress + ImGui::GetIO().DeltaTime * TAB_SWITCH_SPEED);
	float page = EaseOutCubic(skin_page_progress);

	// The whole content area, no columns
	auto origin = ImVec2(column_layout.x, column_layout.y);
	auto region = ImVec2(column_layout.w * 2.f + COLUMN_SPACING, column_layout.h);
	auto toolbar = ImGui::GetWindowPos() + origin;
	const float toolbar_height = S(46.f);

	// Toolbar: team tabs on the left, master switch on the right
	if (TabSwitch("##team", &skin_team, { "Terrorist", "Counter-Terrorist" }, toolbar))
		skin_page_progress = 0.f;

	{
		constexpr auto label = "Skin Changer";
		auto label_size = ImGui::CalcTextSize(label);
		auto switch_pos = ImVec2(toolbar.x + region.x - TOGGLE_SIZE.x - S(2.f), toolbar.y + (S(32.f) - TOGGLE_SIZE.y) * 0.5f);

		ImGui::BeginDisabled(!available);
		ToggleSwitch("##skins_enabled", &cfg::skins::enabled, switch_pos);
		ImGui::EndDisabled();

		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("%s", available
				? "Applies the skins of your team, only you can see them"
				: Engine::IsInsecure()
					? "The skin code was not found in this game version"
					: "Only available when the game is launched with -insecure");

		ImGui::GetWindowDrawList()->AddText(
			ImVec2(switch_pos.x - label_size.x - S(10.f), toolbar.y + (S(32.f) - label_size.y) * 0.5f),
			C(available ? col::text : col::text_dim),
			label
		);

		// Export & import of the loadouts
		const auto presets_size = ImVec2(ImGui::CalcTextSize("Export / Import").x + S(22.f), S(28.f));
		auto presets_pos = ImVec2(switch_pos.x - label_size.x - S(24.f) - presets_size.x, toolbar.y + (S(32.f) - presets_size.y) * 0.5f);
		if (PillButton("Export / Import", presets_pos, presets_size, skin_page == SkinPage::PRESETS, "Saved skin loadouts, apart from the configs")) {
			skin_page = skin_page == SkinPage::PRESETS ? SkinPage::ITEMS : SkinPage::PRESETS;
			skin_page_progress = 0.f;
		}
	}

	// Page below, slides in like the tabs
	ImGui::SetCursorPos(origin + ImVec2(0.f, toolbar_height + (1.f - page) * TAB_SLIDE));
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * page);
	ImGui::BeginChild("##skins_page", ImVec2(region.x, region.y - toolbar_height), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);

	std::optional<SkinPage> go_to;

	switch (skin_page) {
	case SkinPage::ITEMS: {
		if (!loaded)
			TextBlock(Skins::HasFailed()
				? "Could not download the skin list. Hold a weapon in game and set its paint kit id by hand"
				: "Downloading skin list...");

		// Agent & gloves first, like the loadout of the game
		SectionTitle("CHARACTER");
		{
			auto grid = BeginGrid();
			NextCard(grid);

			auto agent = loadout.agent ? Skins::FindAgent(loadout.agent) : nullptr;

			if (ItemCard("##agent", grid.card,
				agent ? SmallImage(agent->image) : "", nullptr,
				agent ? agent->name.c_str() : "Agent",
				agent ? agent->group.c_str() : "Default",
				agent ? RarityColor(agent->rarity_color) : col::border_hover,
				false))
				go_to = SkinPage::AGENTS;

			NextCard(grid);

			auto glove = loadout.glove ? Skins::FindItem(loadout.glove) : nullptr;
			auto glove_skin = glove ? assigned_skin(*glove) : nullptr;

			if (ItemCard("##gloves", grid.card,
				glove_skin ? SmallImage(glove_skin->image) : "", nullptr,
				glove ? glove->name.c_str() : "Gloves",
				glove_skin ? glove_skin->name.c_str() : "Default",
				glove_skin ? RarityColor(glove_skin->rarity_color) : col::border_hover,
				false))
				go_to = SkinPage::GLOVES;

			NextCard(grid);

			auto knife = loadout.knife ? Skins::FindItem(loadout.knife) : nullptr;
			auto knife_skin = knife ? assigned_skin(*knife) : nullptr;
			auto knife_image = knife_skin ? knife_skin->image : (knife ? knife->image : "");

			if (ItemCard("##knife", grid.card,
				knife_image.empty() ? "" : SmallImage(knife_image), knife ? nullptr : Weapon::IconFor(skin_team == cfg::skins::TERRORIST ? 59 : 42),
				knife ? knife->name.c_str() : "Knife",
				knife_skin ? knife_skin->name.c_str() : (knife ? "Vanilla" : "Default"),
				knife_skin ? RarityColor(knife_skin->rarity_color) : col::border_hover,
				false))
				go_to = SkinPage::KNIVES;

			NextCard(grid);

			auto kit = cfg::skins::music_kit ? Skins::FindMusicKit(cfg::skins::music_kit) : nullptr;

			if (ItemCard("##music_kit", grid.card,
				kit ? kit->image : "", nullptr,
				"Music Kit",
				kit ? kit->name.c_str() : (cfg::skins::music_kit ? "Custom" : "Default"),
				kit ? RarityColor(kit->rarity_color) : col::border_hover,
				false))
				go_to = SkinPage::MUSIC_KITS;

			EndGrid(grid);
		}

		// Without the list, at least the weapon in hand can be set by id
		if (!loaded && in_hand) {
			SectionTitle("IN HAND");

			auto grid = BeginGrid();
			NextCard(grid);

			auto label = std::format("Item #{}", in_hand);
			if (ItemCard("##in_hand", grid.card, "", Weapon::IconFor(in_hand), label.c_str(), "", col::border_hover, false, true)) {
				go_to = SkinPage::SKINS;
				selected_item = in_hand;
			}

			EndGrid(grid);
		}

		// Weapons of the team, by category
		std::string category;
		Grid grid{};
		bool grid_open = false;

		for (const auto& item : all_items) {
			if (item.category == "Gloves" || item.category == "Knives")
				continue;

			if (!(skin_team == cfg::skins::TERRORIST ? item.terrorist : item.counter_terrorist))
				continue;

			if (item.category != category) {
				if (grid_open)
					EndGrid(grid);

				category = item.category;
				SectionTitle(ToUpper(category).c_str());

				grid = BeginGrid();
				grid_open = true;
			}

			NextCard(grid);

			auto skin = assigned_skin(item);
			auto id = std::to_string(item.definition_index);

			if (ItemCard(id.c_str(), grid.card,
				skin ? SmallImage(skin->image) : "", Weapon::IconFor(item.definition_index),
				item.name.c_str(),
				skin ? skin->name.c_str() : "Default",
				skin ? RarityColor(skin->rarity_color) : col::border_hover,
				false, item.definition_index == in_hand)) {
				go_to = SkinPage::SKINS;
				selected_item = item.definition_index;
			}
		}

		if (grid_open)
			EndGrid(grid);

		break;
	}

	case SkinPage::GLOVES: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		Toggle("Hide In Third Person", &cfg::skins::glove_hide_third_person,
			"Keeps the new gloves in first person only\nFor player models with gloves built in, they would show both in third person");

		SectionTitle("GLOVE TYPE");

		auto grid = BeginGrid();
		NextCard(grid);

		if (ItemCard("##default", grid.card, "", nullptr, "Default", "From the player model", col::border_hover, loadout.glove == 0))
			loadout.glove = 0;

		for (const auto& item : all_items) {
			if (item.category != "Gloves")
				continue;

			NextCard(grid);

			bool equipped = loadout.glove == item.definition_index;
			auto skin = equipped ? assigned_skin(item) : nullptr;
			auto preview = skin ? skin : (item.skins.empty() ? nullptr : &item.skins.front());
			auto subtitle = skin ? skin->name : std::format("{} skins", item.skins.size());
			auto id = std::to_string(item.definition_index);

			if (ItemCard(id.c_str(), grid.card,
				preview ? SmallImage(preview->image) : "", nullptr,
				item.name.c_str(), subtitle.c_str(),
				preview ? RarityColor(preview->rarity_color) : col::border_hover,
				equipped)) {
				go_to = SkinPage::SKINS;
				selected_item = item.definition_index;
			}
		}

		EndGrid(grid);
		break;
	}

	case SkinPage::KNIVES: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		SectionTitle("KNIFE TYPE");

		auto grid = BeginGrid();
		NextCard(grid);

		if (ItemCard("##default", grid.card, "", Weapon::IconFor(skin_team == cfg::skins::TERRORIST ? 59 : 42), "Default", "From the game", col::border_hover, loadout.knife == 0))
			loadout.knife = 0;

		for (const auto& item : all_items) {
			if (item.category != "Knives")
				continue;

			NextCard(grid);

			bool equipped = loadout.knife == item.definition_index;
			auto skin = equipped ? assigned_skin(item) : nullptr;
			auto image = skin ? skin->image : item.image;
			auto subtitle = skin ? skin->name : (equipped ? std::string("Vanilla") : std::format("{} skins", item.skins.size()));
			auto id = std::to_string(item.definition_index);

			// Picking one equips it right away, the skin is optional
			if (ItemCard(id.c_str(), grid.card,
				image.empty() ? "" : SmallImage(image), Weapon::IconFor(item.definition_index),
				item.name.c_str(), subtitle.c_str(),
				skin ? RarityColor(skin->rarity_color) : col::border_hover,
				equipped)) {
				loadout.knife = item.definition_index;
				go_to = SkinPage::SKINS;
				selected_item = item.definition_index;
			}
		}

		EndGrid(grid);
		break;
	}

	case SkinPage::AGENTS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		SearchBox("##agent_search", skin_search, sizeof(skin_search));

		auto grid = BeginGrid(S(150.f));
		NextCard(grid);

		if (ItemCard("##default", grid.card, "", nullptr, "Default", "From the game", col::border_hover, loadout.agent == 0))
			loadout.agent = 0;

		bool terrorist = skin_team == cfg::skins::TERRORIST;

		for (const auto& agent : Skins::GetAgents()) {
			if (agent.terrorist != terrorist)
				continue;

			if (!ContainsInsensitive(agent.name, skin_search) && !ContainsInsensitive(agent.group, skin_search))
				continue;

			NextCard(grid);

			auto id = std::to_string(agent.definition_index);
			if (ItemCard(id.c_str(), grid.card, SmallImage(agent.image), nullptr, agent.name.c_str(), agent.group.c_str(),
				RarityColor(agent.rarity_color), loadout.agent == agent.definition_index))
				loadout.agent = agent.definition_index;
		}

		EndGrid(grid);
		break;
	}

	case SkinPage::MUSIC_KITS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		TextBlock("For both teams, plays in the menu, at the start & end of rounds and as your MVP anthem");
		SearchBox("##music_search", skin_search, sizeof(skin_search));

		auto grid = BeginGrid();
		NextCard(grid);

		if (ItemCard("##default", grid.card, "", nullptr, "Default", "From the game", col::border_hover, cfg::skins::music_kit == 0))
			cfg::skins::music_kit = 0;

		for (const auto& kit : Skins::GetMusicKits()) {
			if (!ContainsInsensitive(kit.name, skin_search))
				continue;

			NextCard(grid);

			// "Artist, Name" on two lines
			auto comma = kit.name.find(", ");
			auto title = comma != std::string::npos ? kit.name.substr(comma + 2) : kit.name;
			auto artist = comma != std::string::npos ? kit.name.substr(0, comma) : std::string();

			auto id = std::to_string(kit.definition_index);
			if (ItemCard(id.c_str(), grid.card, kit.image, nullptr, title.c_str(), artist.c_str(),
				RarityColor(kit.rarity_color), cfg::skins::music_kit == kit.definition_index))
				cfg::skins::music_kit = kit.definition_index;
		}

		EndGrid(grid);
		break;
	}

	case SkinPage::PRESETS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(10.f)));

		// Half the width, the list does not need more
		ImGui::BeginChild("##skin_presets", ImVec2(std::min(ImGui::GetContentRegionAvail().x, S(460.f)), 0.f),
			ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
		BeginPanel("SKIN LOADOUTS");
		TextBlock("Both teams, gloves, knives, agents & the music kit. Configs are exported apart, in Settings");
		PresetList(Config::Preset::SKINS, skin_presets);
		EndPanel();
		ImGui::EndChild();
		break;
	}

	case SkinPage::SKINS: {
		auto info = Skins::FindItem(selected_item);
		bool is_glove = Skins::IsGlove(selected_item);
		bool is_knife = Skins::IsKnife(selected_item);
		auto& entry = loadout.items[selected_item];

		// Header: back & the item name
		auto header = ImGui::GetCursorScreenPos();
		if (BackButton(is_glove ? "Gloves" : is_knife ? "Knives" : "Loadout", header))
			go_to = is_glove ? SkinPage::GLOVES : is_knife ? SkinPage::KNIVES : SkinPage::ITEMS;

		auto back_width = ImGui::GetItemRectSize().x;
		auto name = info ? info->name : std::format("Item #{}", selected_item);
		ImGui::GetWindowDrawList()->AddText(font_bold, S(17.f), header + ImVec2(back_width + S(14.f), S(4.f)), C(col::text), name.c_str());

		ImGui::SetCursorScreenPos(header + ImVec2(0.f, S(40.f)));

		const float properties_width = S(272.f);
		auto available_size = ImGui::GetContentRegionAvail();

		// Skins
		ImGui::BeginChild("##skin_grid", ImVec2(available_size.x - properties_width - S(14.f), available_size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
		{
			if (info) {
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(4.f));
				SearchBox("##skin_search", skin_search, sizeof(skin_search));
			}

			bool equipped = is_glove ? loadout.glove == selected_item : is_knife ? loadout.knife == selected_item : true;

			auto grid = BeginGrid();
			NextCard(grid);

			// Knives: the plain knife, still equipped
			auto default_image = is_knife && info && !info->image.empty() ? SmallImage(info->image) : "";
			if (ItemCard("##default", grid.card, default_image, is_glove ? nullptr : Weapon::IconFor(selected_item), is_knife ? "Vanilla" : "Default", "", col::border_hover, !entry.paint_kit || !equipped)) {
				entry.paint_kit = 0;
				if (is_glove && loadout.glove == selected_item)
					loadout.glove = 0;
				if (is_knife)
					loadout.knife = selected_item;
			}

			if (info) {
				for (const auto& skin : info->skins) {
					if (!ContainsInsensitive(skin.name, skin_search))
						continue;

					NextCard(grid);

					auto id = std::to_string(skin.paint_kit);
					if (ItemCard(id.c_str(), grid.card, SmallImage(skin.image), nullptr, skin.name.c_str(), "", RarityColor(skin.rarity_color), equipped && entry.paint_kit == skin.paint_kit)) {
						entry.paint_kit = skin.paint_kit;
						entry.wear = std::clamp(entry.wear, std::max(skin.min_float, 0.0001f), skin.max_float);

						if (is_glove)
							loadout.glove = selected_item;
						if (is_knife)
							loadout.knife = selected_item;
					}
				}
			}

			EndGrid(grid);
		}
		ImGui::EndChild();

		ImGui::SameLine(0.f, S(14.f));

		// Properties of the picked skin
		ImGui::BeginChild("##skin_properties", ImVec2(properties_width, available_size.y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
		{
			auto skin = info ? info->FindSkin(entry.paint_kit) : nullptr;

			BeginPanel(skin ? ToUpper(skin->name).c_str() : "DEFAULT");
			{
				// Big preview
				auto d = ImGui::GetWindowDrawList();
				auto pos = ImGui::GetCursorScreenPos();
				auto size = ImVec2(ImGui::GetContentRegionAvail().x, S(150.f));
				auto center = pos + size * 0.5f;

				d->AddRectFilled(pos, pos + size, C(col::window), S(8.f));
				d->AddCircleFilled(center, S(64.f), C(skin ? RarityColor(skin->rarity_color) : col::border_hover, 0.12f), 48);

				auto texture = skin ? ImageCache::Get(SmallImage(skin->image)) : ImTextureID{};
				if (texture) {
					auto fit = ImVec2(size.y * 4.f / 3.f, size.y);
					d->AddImage(texture, center - fit * 0.5f, center + fit * 0.5f, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE));
				}
				else if (!is_glove) {
					auto icon = Weapon::IconFor(selected_item);
					auto extent = font_regular->CalcTextSizeA(S(56.f), FLT_MAX, 0.f, icon);
					d->AddText(font_regular, S(56.f), center - extent * 0.5f, C(col::text_faint), icon);
				}

				ImGui::Dummy(size);
				ImGui::Dummy(ImVec2(0.f, S(6.f)));

				ImGui::BeginDisabled(!entry.paint_kit);
				{
					float min_wear = std::max(skin ? skin->min_float : 0.f, 0.0001f);
					float max_wear = skin ? skin->max_float : 1.f;

					SliderFloat("Wear", &entry.wear, min_wear, max_wear, "%.4f", "Float value, lower is cleaner");
					SliderInt("Seed", &entry.seed, 0, 1000, "%d", "Pattern seed, moves the pattern on skins like Case Hardened or Fade");
				}
				ImGui::EndDisabled();

				if (InputIntRow("Paint Kit ID", &entry.paint_kit, "Set the paint kit by hand, 0 for the default look", S(80.f)) && entry.paint_kit > 0) {
					if (is_glove)
						loadout.glove = selected_item;
					if (is_knife)
						loadout.knife = selected_item;
				}

				entry.paint_kit = std::max(entry.paint_kit, 0);
			}
			EndPanel();
		}
		ImGui::EndChild();

		// Dont keep empty entries around in the config
		if (!entry.paint_kit) {
			loadout.items.erase(selected_item);
			if (is_glove && loadout.glove == selected_item)
				loadout.glove = 0;
		}

		break;
	}
	}

	ImGui::EndChild();
	ImGui::PopStyleVar();

	if (go_to) {
		int item = selected_item;
		OpenSkinPage(*go_to, *go_to == SkinPage::SKINS ? item : 0);
	}
}

void Menu::RenderMovementTab() {
	constexpr auto unavailable = "Only available when the game is launched with -insecure";
	const bool available = Movement::IsAvailable();

	BeginColumn(0);
	{
		BeginPanel("JUMP");
		ImGui::BeginDisabled(!available);
		Toggle("Bunny Hop", &cfg::misc::bhop, available ? "Hold SPACE to jump automatically when landing" : unavailable);
		Toggle("Auto Strafe", &cfg::misc::auto_strafe, available
			? "In the air, moving the mouse left holds A, right holds D.\nTurn smoothly from side to side to gain speed"
			: unavailable);
		if (cfg::misc::auto_strafe) {
			// Subtick needs the input layout of this game build
			if (!Subtick::IsAvailable())
				cfg::misc::auto_strafe_mode = 0;

			ImGui::BeginDisabled(!Subtick::IsAvailable());
			SegmentRow("Mode", &cfg::misc::auto_strafe_mode, { "Legit", "Subtick" }, Subtick::IsAvailable()
				? "Legit: A & D follow your mouse, like doing it yourself\n"
				  "Subtick: every tick is split into steps of A & D with the view turned for each, speed without turning.\n"
				  "Flies where you look, or hold W A S D in the air to go that way from the view.\n"
				  "Writes input events the game turns into commands: experimental, may be rejected or get you kicked"
				: "Subtick: only with -insecure, on the game build it was made for");
			ImGui::EndDisabled();

			Toggle("Only With Space", &cfg::misc::auto_strafe_space, "Only while SPACE is held, so a normal jump stays untouched");
		}
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("GROUND");
		ImGui::BeginDisabled(!available);
		Toggle("Quick Stop", &cfg::misc::quick_stop, available
			? "Counter strafes for you when you let go of WASD, stopping right away"
			: unavailable);
		Toggle("Null Binds", &cfg::misc::null_binds, available
			? "Holding A & D (or W & S) moves towards the last one pressed instead of stopping,\nlike snap tap"
			: unavailable);
		ImGui::EndDisabled();

		if (available && cfg::misc::quick_stop && !Movement::UsesGameInput())
			TextBlock("Game input unavailable, quick stop keys are sent through Windows");
		EndPanel();
	}
	EndColumn();
}

void Menu::RenderMiscTab() {
	constexpr auto unavailable = "Only available when the game is launched with -insecure";

	misc_page_progress = std::min(1.f, misc_page_progress + ImGui::GetIO().DeltaTime * TAB_SWITCH_SPEED);
	float page = EaseOutCubic(misc_page_progress);

	// Pages in a toolbar like the players tab, two columns of panels below
	auto toolbar = ImGui::GetWindowPos() + ImVec2(column_layout.x, column_layout.y);
	const float toolbar_height = S(46.f);

	if (TabSwitch("##misc_page", &misc_page, { "Camera", "Profile", "Interface", "Visuals" }, toolbar))
		misc_page_progress = 0.f;

	auto columns = column_layout;
	column_layout.y += toolbar_height + (1.f - page) * TAB_SLIDE;
	column_layout.h -= toolbar_height;
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * page);

	switch (misc_page) {
	case 0: {
		BeginColumn(0);
		{
			BeginPanel("FIELD OF VIEW");
			ImGui::BeginDisabled(!View::IsAvailable());
			Toggle("FOV Override", &cfg::view::fov_enabled, View::IsAvailable()
				? "Camera field of view, scoped weapons keep their zoom"
				: unavailable);
			if (cfg::view::fov_enabled)
				SliderInt("FOV", &cfg::view::fov, 60, 140, "%d");
			ImGui::EndDisabled();
			EndPanel();

			BeginPanel("THIRD PERSON");
			ImGui::BeginDisabled(!View::IsThirdPersonAvailable());
			Toggle("Third Person", &cfg::view::third_person, View::IsThirdPersonAvailable()
				? "Camera behind your player, like the thirdperson command without sv_cheats"
				: View::IsAvailable()
					? "The third person code was not found in this game version"
					: unavailable);
			if (cfg::view::third_person) {
				SegmentRow("Activation", &cfg::view::third_person_mode, { "Toggle", "Hold", "Always" });
				if (cfg::view::third_person_mode != 2)
					KeybindRow("Key", &cfg::view::third_person_key);
				Toggle("Off While Scoped", &cfg::view::third_person_scoped_off, "Back to first person while looking through a scope");
			}
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			// The console variables of the game, so the same limits
			BeginPanel("VIEWMODEL");
			ImGui::BeginDisabled(!View::IsAvailable());
			Toggle("Viewmodel Override", &cfg::view::viewmodel_enabled, View::IsAvailable()
				? "Position & field of view of the weapon in your hands, like viewmodel_fov & viewmodel_offset_x/y/z in the console\nYour own values come back when turned off"
				: unavailable);
			if (cfg::view::viewmodel_enabled) {
				SliderFloat("FOV", &cfg::view::viewmodel_fov, 40.f, 120.f, "%.0f", "The game allows 60 - 68");
				SliderFloat("Offset X", &cfg::view::viewmodel_x, -20.f, 20.f, "%.1f", "Left & right, the game allows -2 - 2.5");
				SliderFloat("Offset Y", &cfg::view::viewmodel_y, -20.f, 20.f, "%.1f", "Forward & back, the game allows -2 - 2");
				SliderFloat("Offset Z", &cfg::view::viewmodel_z, -20.f, 20.f, "%.1f", "Up & down, the game allows -2 - 2");

				if (!View::IsViewmodelAvailable())
					TextBlock("Looking for the viewmodel settings of the game...");
			}
			ImGui::EndDisabled();
			EndPanel();

			const char* camera_missing = View::IsAvailable()
				? "The camera code was not found in this game version"
				: unavailable;

			BeginPanel("FREE CAM");
			ImGui::BeginDisabled(!Freecam::IsAvailable());
			Toggle("Free Cam", &cfg::view::freecam, Freecam::IsAvailable()
				? "The camera flies on its own while your player stands still, only while alive\n"
				  "The mouse looks around, W A S D fly where you look, space up, left ctrl down, left shift faster\n"
				  "Your player gets none of it: no turning, walking, shooting or scoping"
				: camera_missing);
			if (cfg::view::freecam) {
				KeybindRow("Key", &cfg::view::freecam_key);
				SliderFloat("Speed", &cfg::view::freecam_speed, 100.f, 3000.f, "%.0f", "Units per second, three times faster with shift");
				SliderFloat("Sensitivity", &cfg::view::freecam_sensitivity, 0.1f, 5.f, "%.2fx", "Turning with the mouse, times your sensitivity in the game");
			}
			ImGui::EndDisabled();
			EndPanel();

			BeginPanel("AFTER DEATH");
			ImGui::BeginDisabled(!Freecam::IsDeadAvailable());
			Toggle("Casual Spectate", &cfg::view::dead_spectate, Freecam::IsDeadAvailable()
				? "Once you die, watch anyone like in casual, enemies too, no key needed\n"
				  "Left & right click switch the player, space goes first person, third person, free cam"
				: Freecam::IsAvailable()
					? "The spectator camera was not found in this game version"
					: camera_missing);
			if (cfg::view::dead_spectate)
				SliderFloat("Distance", &cfg::view::spectate_distance, 40.f, 300.f, "%.0f", "Behind them in third person, W & S change it");
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();
		break;
	}

	case 1: {
		BeginColumn(0);
		{
			BeginPanel("CLAN TAG");
			ImGui::BeginDisabled(!ClanTag::IsAvailable());
			Toggle("Clan Tag", &cfg::misc::clantag, ClanTag::IsAvailable()
				? "Animated tag, as the game shows it to you"
				: unavailable);

			if (cfg::misc::clantag) {
				TextField("##clantag_text", "Tag... (tag 1|tag 2 take turns)", cfg::misc::clantag_text, sizeof(cfg::misc::clantag_text),
					"Separate texts with | to show them one after another,\neach one a whole round of the animation");

				// Where the tag goes, the name ones need the name update
				ImGui::BeginDisabled(!ClanTag::IsNameAvailable());
				SegmentRow("Position", &cfg::misc::clantag_target, { "Clan", "Before", "After", "Name" },
					"Clan: [tag] name, in the clan slot of the game\n"
					"Before: tag name, without brackets\n"
					"After: name tag\n"
					"Name: the tag is your whole name");
				ImGui::EndDisabled();

				static const char* descriptions[] = {
					"Always the same", "On & off", "Moves left through itself",
					"Typed in with a cursor & deleted again", "Letters found one by one, the rest guessed", "One capital moving through",
					"Every letter fades in & out on its own ( . : )", "All guessed, letters lock in random order", "Now & then letters turn into symbols",
					"Grows out of its middle & shrinks back", "- = # around the tag", "Comes in from the left, goes out to the right",
				};

				auto& modes = ClanTag::GetModes();
				int mode = std::clamp(cfg::misc::clantag_mode, 0, static_cast<int>(modes.size()) - 1);
				DropdownRow("Animation", &cfg::misc::clantag_mode, modes, descriptions, descriptions[mode]);

				SliderFloat("Speed", &cfg::misc::clantag_speed, 100.f, 1500.f, "%.0f ms", "Time of each step of the animation (Static: 8 steps per text)");
			}
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			BeginPanel("NAME");
			ImGui::BeginDisabled(!ClanTag::IsNameAvailable());
			Toggle("Change Name", &cfg::misc::name_change, ClanTag::IsNameAvailable()
				? "Your whole name in the scoreboard & kill feed, as the game shows it to you.\nEmpty keeps your real name"
				: unavailable);
			if (cfg::misc::name_change)
				TextField("##name_text", "Name...", cfg::misc::name_text, sizeof(cfg::misc::name_text));
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();
		break;
	}

	default: {
		BeginColumn(0);
		{
			BeginPanel("RADAR");
			Toggle("Radar", &cfg::world::radar::enabled);

			// The radar of the game needs memory writes
			if (!GameRadar::IsAvailable())
				cfg::world::radar::mode = cfg::world::radar::MODE_OVERLAY;

			if (cfg::world::radar::enabled) {
				ImGui::BeginDisabled(!GameRadar::IsAvailable());
				SegmentRow("Type", &cfg::world::radar::mode, { "External", "Internal" }, GameRadar::IsAvailable()
					? "External: our own radar window\nInternal: enemies show up on the radar of the game, even when nobody sees them"
					: "Internal is only available when the game is launched with -insecure");
				ImGui::EndDisabled();

				if (cfg::world::radar::mode == cfg::world::radar::MODE_OVERLAY) {
					Toggle("Disable Rotation", &cfg::world::radar::no_rotate);
					SliderFloat("Range", &cfg::world::radar::range, 100.f, 8000.f, "%.0f u");
				}
			}
			EndPanel();

			BeginPanel("SPECTATORS");
			Toggle("Spectator List", &cfg::world::spectators::enabled);
			if (cfg::world::spectators::enabled) {
				Toggle("Detailed", &cfg::world::spectators::detailed);
				Toggle("Only Self", &cfg::world::spectators::self_only, "Only display users spectating you");
			}
			EndPanel();

			BeginPanel("KEYBINDS");
			Toggle("Keybind List", &cfg::world::keybinds::enabled,
				"Shows the features turned on by a key while they are on: third person, free cam.\n"
				"With the menu open it shows them all & can be dragged");
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			BeginPanel("OVERLAY");
			Toggle("Sniper Crosshair", &cfg::world::crosshair::enabled, "Draws a crosshair while holding an unscoped sniper");
			if (cfg::world::crosshair::enabled) {
				SegmentRow("Style", &cfg::world::crosshair::style, { "Classic", "CS2" },
					"Classic: small white cross\nCS2: your crosshair from the game settings (size, gap, color, outline, dot)");
				if (cfg::world::crosshair::style == cfg::world::crosshair::STYLE_GAME && !GameCrosshair::Get().found)
					TextBlock("No CS2 settings found in the Steam userdata, using the default crosshair");
			}
			Toggle("Velocity Graph", &cfg::world::velocity::enabled);
		#ifdef _DEBUG // Part of the velocity graph for developers
			if (cfg::world::velocity::enabled) {
				SliderInt("Sample Rate", &cfg::world::velocity::sample_rate, 1, 100, "%d");
				SliderFloat("Sample Length", &cfg::world::velocity::sample_length, 1.f, 20.f, "%.1f s");
			}
		#endif
			EndPanel();

			BeginPanel("MATCHMAKING");
			Toggle("Auto Accept", &cfg::misc::auto_accept,
				"Accepts when a match is found, the cursor never moves.\n"
				"-insecure: from memory like the button does. Else a click sent to the game window,\n"
				"brought to the front if needed");
			EndPanel();
		}
		EndColumn();
		break;
	}

	case 3: {
		namespace vis = cfg::visuals;
		const bool writes = Visuals::IsAvailable();

		BeginColumn(0);
		{
			BeginPanel("REMOVALS");
			ImGui::BeginDisabled(!writes);
			Toggle("No Flash", &vis::no_flash, writes ? "Flashbangs blind you less, or not at all" : unavailable);
			if (vis::no_flash)
				SliderFloat("Flash Strength", &vis::flash_alpha, 0.f, 255.f, "%.0f", "0 is no white at all, 255 is the game");

			Toggle("No Smoke", &vis::no_smoke, writes
				? "Smokes are not drawn. The smoke ESP still shows where they are"
				: unavailable);
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();
		break;
	}
	}

	ImGui::PopStyleVar();
	column_layout = columns;
}

void Menu::RenderConfigsTab() {
	BeginColumn(0);
	{
		BeginPanel("CONFIGS");
		TextBlock("Everything but the skins, saved in export/configs");
		PresetList(Config::Preset::CONFIG, config_presets);
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("SKIN LOADOUTS");
		TextBlock("Both teams, gloves, knives, agents & the music kit, saved in export/skins");
		PresetList(Config::Preset::SKINS, skin_presets);
		EndPanel();
	}
	EndColumn();
}

void Menu::RenderSettingsTab() {
	BeginColumn(0);
	{
		BeginPanel("GENERAL");
		Toggle("ESP", &cfg::enabled, "Master switch of everything drawn over the game");

		if (Toggle("Streamproof", &cfg::settings::streamproof, "Hides the overlay from screen capture and streaming software"))
		{
			Window::SetAffinity(
				Window::hwnd,
				cfg::settings::streamproof ? WindowAffinity::Invisible : WindowAffinity::Disabled
			);
		}

		ColorRow("Menu Accent", &cfg::settings::accent, "The one color of the menu");
		EndPanel();

	#ifdef _DEBUG
		BeginPanel("DEV");
		if (Toggle("Console", &cfg::dev::console) && !cfg::dev::console)
			LogHelper::Free();

		SliderInt("Cache Refresh", &cfg::dev::cache_refresh_rate, 0, 100, "%d ms");
		Toggle("Force Show Flags", &cfg::dev::force_show_flags);
		EndPanel();
	#endif

	#ifdef _DEBUG
		// Warnings, errors & info lines always show
		BeginPanel("DEBUG LOG");
		namespace logs = cfg::settings::logs;
		Toggle("Skins", &logs::skins, "Debug lines of skins, knives & agents");
		Toggle("Other", &logs::other, "Every other debug line");
		EndPanel();
	#endif
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("PERFORMANCE");
		if (Toggle("VSync", &cfg::settings::vsync, "Overlay at the refresh rate of the screen\nOff when the overlay lags, V-Sync of the game too (Advanced Video)"))
			Window::vsync = cfg::settings::vsync;

		Toggle("Free CPU", &cfg::settings::free_cpu, "Let the CPU sleep to free resources\nOff as a last resort: more CPU use, less latency");
		Toggle("Watermark", &cfg::settings::watermark, "Name, frame rate & map in the top right corner");

	#ifdef _DEBUG
		ImGui::BeginDisabled(!cfg::settings::watermark);
		Toggle("Frame Times", &cfg::settings::frame_times, "Shows in the watermark where the time of a frame goes, in milliseconds");
		ImGui::EndDisabled();
	#endif
		EndPanel();
	}
	EndColumn();
}

void Menu::SetupStyles() {
	ImGuiStyle& style = ImGui::GetStyle();

	auto c = [](ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); };

	style.Colors[ImGuiCol_Text] = c(col::text);
	style.Colors[ImGuiCol_TextDisabled] = c(col::text_dim);
	style.Colors[ImGuiCol_TextLink] = c(col::accent);
	style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.55f, 1.00f, 0.35f);

	style.Colors[ImGuiCol_WindowBg] = c(col::window);
	style.Colors[ImGuiCol_ChildBg] = c(col::panel);
	style.Colors[ImGuiCol_PopupBg] = c(col::sidebar);
	style.Colors[ImGuiCol_Border] = c(col::border);
	style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);

	style.Colors[ImGuiCol_FrameBg] = c(col::track);
	style.Colors[ImGuiCol_FrameBgHovered] = c(col::track_hover);
	style.Colors[ImGuiCol_FrameBgActive] = c(col::track_hover);

	// Title bars of overlay windows (spectator list, radar, velocity graph)
	style.Colors[ImGuiCol_TitleBg] = c(col::sidebar);
	style.Colors[ImGuiCol_TitleBgActive] = c(col::sidebar);
	style.Colors[ImGuiCol_TitleBgCollapsed] = c(col::sidebar);

	style.Colors[ImGuiCol_MenuBarBg] = c(col::sidebar);
	style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
	style.Colors[ImGuiCol_ScrollbarGrab] = c(col::track);
	style.Colors[ImGuiCol_ScrollbarGrabHovered] = c(col::track_hover);
	style.Colors[ImGuiCol_ScrollbarGrabActive] = c(col::accent);

	style.Colors[ImGuiCol_CheckMark] = c(col::accent);
	style.Colors[ImGuiCol_SliderGrab] = c(col::accent);
	style.Colors[ImGuiCol_SliderGrabActive] = c(col::accent);

	style.Colors[ImGuiCol_Button] = c(col::track);
	style.Colors[ImGuiCol_ButtonHovered] = c(col::track_hover);
	style.Colors[ImGuiCol_ButtonActive] = c(col::border_hover);
	style.Colors[ImGuiCol_Header] = c(col::track);
	style.Colors[ImGuiCol_HeaderHovered] = c(col::track_hover);
	style.Colors[ImGuiCol_HeaderActive] = c(col::border_hover);
	style.Colors[ImGuiCol_Separator] = c(col::border);
	style.Colors[ImGuiCol_SeparatorHovered] = c(col::border_hover);
	style.Colors[ImGuiCol_SeparatorActive] = c(col::accent);

	style.Colors[ImGuiCol_ResizeGrip] = ImVec4(0.f, 0.f, 0.f, 0.f);
	style.Colors[ImGuiCol_ResizeGripHovered] = c(col::track_hover);
	style.Colors[ImGuiCol_ResizeGripActive] = c(col::accent);

	style.Colors[ImGuiCol_TableHeaderBg] = c(col::sidebar);
	style.Colors[ImGuiCol_TableBorderStrong] = c(col::border);
	style.Colors[ImGuiCol_TableBorderLight] = c(col::border);

	style.Colors[ImGuiCol_PlotLines] = c(col::accent);
	style.Colors[ImGuiCol_PlotHistogram] = c(col::accent);
	style.Colors[ImGuiCol_DragDropTarget] = c(col::accent);
	style.Colors[ImGuiCol_NavCursor] = c(col::accent);
	style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.f, 0.f, 0.f, 0.45f);

	style.WindowBorderSize = 1.f;
	style.ChildBorderSize = 1.f;
	style.PopupBorderSize = 1.f;
	style.FrameBorderSize = 0.f;

	style.WindowRounding = WINDOW_ROUNDING;
	style.ChildRounding = PANEL_ROUNDING;
	style.FrameRounding = 6.f;
	style.PopupRounding = 10.f;
	style.GrabRounding = 6.f;
	style.ScrollbarRounding = 3.f;
	style.ScrollbarSize = 6.f;

	style.WindowPadding = ImVec2(10.f, 10.f);
	style.FramePadding = ImVec2(8.f, 4.f);
	style.ItemSpacing = ImVec2(8.f, 6.f);

	// Fonts
	auto& io = ImGui::GetIO();
	io.Fonts->Clear();

	auto font_path = [](const char* preferred, const char* fallback) {
		return std::filesystem::exists(preferred) ? preferred : fallback;
	};

	font_regular = io.Fonts->AddFontFromFileTTF(
		font_path("C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"),
		16.0f
	);

	ImFontConfig merge_icon_cfg{};
	merge_icon_cfg.FontDataOwnedByAtlas = false;
	merge_icon_cfg.MergeMode = true;
	merge_icon_cfg.GlyphOffset = Vec2_t(0, 3.f);

	// the icons will use the size specified when getting added so it ignores the base size
	static const ImWchar icon_ranges[] = { 0xE100, 0xE108, 0 };
	io.Fonts->AddFontFromMemoryTTF(icons_font, icons_font_len, 18.f, &merge_icon_cfg, icon_ranges);

	// Semibold reads cleaner than bold for titles
	// Weapon silhouettes, used as tab icon
	ImFontConfig merge_weapon_cfg{};
	merge_weapon_cfg.FontDataOwnedByAtlas = false;
	merge_weapon_cfg.MergeMode = true;
	merge_weapon_cfg.GlyphOffset = Vec2_t(0, 1.f);

	static const ImWchar weapon_ranges[] = { 0xE000, 0xE046, 0 };
	io.Fonts->AddFontFromMemoryTTF(weapon_icon_font, weapon_icon_font_len, 13.f, &merge_weapon_cfg, weapon_ranges);

	font_bold = io.Fonts->AddFontFromFileTTF(
		font_path("C:\\Windows\\Fonts\\seguisb.ttf",
			font_path("C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf")),
		16.0f
	);
}

void Menu::RenderStartupHelpImpl() {
	col::accent = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg::settings::accent.r, cfg::settings::accent.g, cfg::settings::accent.b, 1.f));

	static bool has_opened_menu = false;
	static float fade = 0.f;

	if (Renderer::IsOpen())
		has_opened_menu = true;

	// Fades out once the menu was opened the first time
	auto& io = ImGui::GetIO();
	fade = std::clamp(fade + io.DeltaTime * (has_opened_menu ? -4.f : 3.f), 0.f, 1.f);

	if (fade <= 0.f)
		return;

	auto d = ImGui::GetBackgroundDrawList();
	float alpha = EaseOutCubic(fade);
	constexpr auto text = "Press  INSERT  or  RIGHT SHIFT  to open the menu,  END  to close";
	auto text_size = ImGui::CalcTextSize(text);

	auto toast_size = ImVec2(text_size.x + S(56.f), S(40.f));
	auto toast_min = ImVec2((io.DisplaySize.x - toast_size.x) * 0.5f, S(60.f) + (1.f - alpha) * -S(12.f));
	auto toast_max = toast_min + toast_size;

	auto with_alpha = [&](ImU32 color, float a) {
		return ImGui::GetColorU32(color, a * alpha);
	};

	d->AddRectFilled(toast_min, toast_max, with_alpha(col::sidebar, 0.95f), S(20.f));
	d->AddRect(toast_min, toast_max, with_alpha(col::border_hover, 1.f), S(20.f), 0, 1.f);

	auto dot = ImVec2(toast_min.x + S(20.f), toast_min.y + toast_size.y * 0.5f);
	d->AddCircleFilled(dot, S(4.f), with_alpha(col::accent, 1.f), 16);

	d->AddText(ImVec2(toast_min.x + S(36.f), toast_min.y + (toast_size.y - text_size.y) * 0.5f), with_alpha(col::text, 1.f), text);
}
