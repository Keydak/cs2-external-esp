#include "Menu.hpp"

#include "core/engine/Engine.hpp"
#include "core/engine/cache/Cache.hpp"
#include "core/offsets/Offsets.hpp"
#include "core/features/Movement.hpp"
#include "core/features/Subtick.hpp"
#include "core/features/View.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/MaterialChams.hpp"
#include "core/features/AgentPreview.hpp"
#include "core/features/KillEffect.hpp"
#include "core/features/Skins.hpp"
#include "core/features/CustomModels.hpp"
#include "core/features/ModBrowser.hpp"
#include "core/features/GameRadar.hpp"
#include "core/features/ClanTag.hpp"
#include "core/features/Visuals.hpp"
#include "assets/fonts/WeaponIcons.h"
#include "assets/fonts/Inter.h"
#include "assets/images/Logo.h"
#include "assets/models/PlayerModels.h"
#include "gui/frontend/images/ImageCache.hpp"
#include "gui/frontend/images/Avatars.hpp"
#include "updater/Updater.hpp"
#include "core/features/Sounds.hpp"
#include "core/engine/classes/Weapon.hpp"
#include "gui/frontend/esp/Esp.hpp"
#include "gui/frontend/esp/GameCrosshair.hpp"
#include "gui/frontend/overlays/Overlays.hpp"
#include "gui/renderer/Renderer.hpp" // Circular dependency
#include "gui/renderer/window/Window.hpp" // Circular dependency

#include "config/Config.hpp"

#include <imgui_internal.h>
#include <shellapi.h>

#include <fstream>
#include <numbers>


namespace {
	// Palette
	namespace col {
		// Of the theme (Amoled, the only one), set by ApplyTheme(). The window is a little see through: the game shows behind it
		inline ImU32 window = 0;
		inline ImU32 sidebar = 0;        // A shade darker than the rest
		inline ImU32 header = 0;
		inline ImU32 panel = 0;          // Group boxes, lighter so they stand off the window
		inline ImU32 panel_top = 0;      // Their title band
		inline ImU32 selected = 0;       // Picked rows of lists
		inline ImU32 track = 0;          // Fields & tracks, "frame inactive"
		inline ImU32 track_hover = 0;    // "frame active"
		inline ImU32 button = 0;
		inline ImU32 button_hover = 0;
		inline ImU32 text_label = 0;     // Labels of what is off
		inline ImU32 text_dim = 0;

		// The same in every theme
		constexpr ImU32 border = IM_COL32(255, 255, 255, 13);
		constexpr ImU32 border_hover = IM_COL32(255, 255, 255, 32);
		constexpr ImU32 highlight = IM_COL32(255, 255, 255, 16);    // Top edge of boxes, light from above
		constexpr ImU32 hover = IM_COL32(255, 255, 255, 6);
		constexpr ImU32 online = IM_COL32(126, 204, 142, 255);
		constexpr ImU32 text = IM_COL32(255, 255, 255, 255);
		constexpr ImU32 text_faint = IM_COL32(255, 255, 255, 120);    // Group & section titles
		constexpr ImU32 on_accent = IM_COL32(6, 10, 18, 255);        // Text on light accent fills
		constexpr ImU32 knob = IM_COL32(255, 255, 255, 255);
		constexpr ImU32 shadow = IM_COL32(0, 0, 0, 255);

		// The theme: Amoled, almost black with dark grey boxes. The only one, not picked in the menu (user's choice)
		struct Theme {
			ImU32 window, sidebar, panel, panel_top, selected, track, track_hover, button, button_hover, text_label, text_dim;
		};

		constexpr Theme AMOLED = {
			IM_COL32(8, 8, 10, 255), IM_COL32(5, 5, 6, 255), IM_COL32(19, 19, 22, 255), IM_COL32(24, 24, 28, 255), IM_COL32(40, 40, 48, 170),
			IM_COL32(32, 32, 38, 255), IM_COL32(46, 46, 54, 255), IM_COL32(22, 22, 26, 255), IM_COL32(32, 32, 38, 255),
			IM_COL32(150, 150, 158, 255), IM_COL32(134, 134, 142, 255),
		};

		void ApplyTheme() {
			const auto& theme = AMOLED;
			window = theme.window;
			sidebar = theme.sidebar;
			header = theme.window;
			panel = theme.panel;
			panel_top = theme.panel_top;
			selected = theme.selected;
			track = theme.track;
			track_hover = theme.track_hover;
			button = theme.button;
			button_hover = theme.button_hover;
			text_label = theme.text_label;
			text_dim = theme.text_dim;
		}

		// How much of the window background covers the game, the group boxes stay solid. From the config like the accent
		inline float backdrop = 0.86f;

		// From the config, refreshed every frame
		inline ImU32 accent = IM_COL32(255, 255, 255, 255);
	}

	// Layout, at a scale of 1. SetScale() fills in the scaled values below every frame
	float ui_scale = 1.f;

	float S(float value) {
		return value * ui_scale;
	}

	ImVec2 S(ImVec2 value) {
		return value * ui_scale;
	}

	constexpr ImVec2 BASE_MENU_SIZE = ImVec2(860.f, 580.f);

	ImVec2 MENU_SIZE;
	float WINDOW_ROUNDING;
	float SIDEBAR_WIDTH;
	float HEADER_HEIGHT;
	float CONTENT_PADDING;
	float COLUMN_SPACING;
	float ROW_HEIGHT;
	float PANEL_ROUNDING;
	float PANEL_TITLE_HEIGHT;

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

	void SetScale(float scale) {
		ui_scale = scale;

		MENU_SIZE = S(BASE_MENU_SIZE);
		WINDOW_ROUNDING = S(12.f);
		SIDEBAR_WIDTH = S(176.f);
		HEADER_HEIGHT = S(58.f);
		CONTENT_PADDING = S(16.f);
		COLUMN_SPACING = S(12.f);
		ROW_HEIGHT = S(28.f);
		PANEL_ROUNDING = S(9.f);
		PANEL_TITLE_HEIGHT = S(34.f);

		TOGGLE_SIZE = S(ImVec2(32.f, 18.f));
		SWATCH_SIZE = S(18.f);
		SLIDER_WIDTH = S(116.f);
		SLIDER_VALUE_WIDTH = S(54.f);
		WIDGET_SPACING = S(8.f);

		TAB_SLIDE = S(6.f);
	}

#ifdef _DEBUG
	constexpr auto BRAND_TITLE = "Cs2 External";
	constexpr auto BRAND_SUBTITLE = "Counter-Strike 2 [DEV]";
#else
	constexpr auto BRAND_TITLE = "Cs2 External";
	constexpr auto BRAND_SUBTITLE = "Counter-Strike 2";
#endif

	// Bottom left of the sidebar: the author of this fork, opens the credits & the accent
	constexpr auto AUTHOR = "Keydak";
	constexpr auto AUTHOR_AVATAR = "https://github.com/Keydak.png?size=96";
	constexpr auto AUTHOR_URL = L"https://github.com/Keydak";

	struct Credit {
		const char* name;
		const char* what;
	};

	constexpr Credit CREDITS[] = {
		{ "Keydak", "This fork & its rework" },
		{ "IMXNOOBX", "The original cs2-external-esp" },
		{ "a2x", "cs2-dumper, the offsets" },
	};

	// Activation of a feature with a key, cfg mode values 0, 1, 2
	const std::vector<const char*> ACTIVATION_MODES = { "Toggle", "Hold", "Always" };
	constexpr const char* ACTIVATION_DESCRIPTIONS[] = {
		"Press the key to switch it on & off",
		"On only while the key is held",
		"Always on, no key",
	};

	// Quick accents, the swatch after them picks any other
	constexpr ImU32 ACCENTS[] = {
		IM_COL32(255, 255, 255, 255),
		IM_COL32(74, 144, 255, 255),
		IM_COL32(150, 110, 255, 255),
		IM_COL32(255, 105, 180, 255),
		IM_COL32(255, 90, 90, 255),
		IM_COL32(255, 150, 60, 255),
		IM_COL32(90, 210, 130, 255),
	};

	ImFont* font_regular = nullptr;
	ImFont* font_bold = nullptr;

	struct ColumnLayout {
		float x, y, w, h;
	} column_layout{};

	// Applies the current style alpha, so fades & BeginDisabled() also reach custom drawings
	// Tooltips with their own padding: inside a panel the style has none above & below
	template<typename... Args>
	void ShowTooltip(const char* format, Args... args) {
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.f), S(8.f)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.f), S(4.f)));
		ImGui::SetTooltip(format, args...);
		ImGui::PopStyleVar(2);
	}

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

	// Thin dark edge around a see through window: strokes only, so nothing dark ends up under the window itself
	void Outline(ImDrawList* d, ImVec2 min, ImVec2 max, float rounding) {
		for (int i = 1; i <= 3; i++) {
			float grow = static_cast<float>(i);
			d->AddRect(min - ImVec2(grow, grow), max + ImVec2(grow, grow), C(col::shadow, 0.3f / i), rounding + grow, 0, 1.f);
		}
	}

	// Text & marks on an accent fill: dark on light accents like the white default, white on dark ones
	ImU32 OnAccent() {
		auto accent = ImGui::ColorConvertU32ToFloat4(col::accent);
		return accent.x * 0.299f + accent.y * 0.587f + accent.z * 0.114f > 0.6f ? col::on_accent : col::text;
	}

	// Soft light of a color around a shape: a few wider, fainter rounded strokes
	void SoftGlow(ImDrawList* d, ImVec2 min, ImVec2 max, ImU32 color, float rounding, float strength = 1.f) {
		for (int i = 1; i <= 4; i++) {
			float grow = S(1.5f) * i;
			d->AddRect(min - ImVec2(grow, grow), max + ImVec2(grow, grow), C(color, strength * 0.14f / i), rounding + grow, 0, S(1.5f));
		}
	}

	// Shadow under a box, a little lower than it: the box lifts off what is behind
	void SoftShadow(ImDrawList* d, ImVec2 min, ImVec2 max, float rounding) {
		for (int i = 1; i <= 4; i++) {
			float grow = S(2.f) * i;
			auto drop = ImVec2(0.f, S(3.f));
			d->AddRectFilled(min - ImVec2(grow, grow) + drop, max + ImVec2(grow, grow) + drop, C(col::shadow, 0.06f), rounding + grow);
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
			ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar
		);
	}

	void EndColumn() {
		ImGui::EndChild();
	}

	// A group box: its title inside at the top over a line, lighter than the window with a shadow under it
	void BeginPanel(const char* title) {
		ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
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

		auto d = ImGui::GetWindowDrawList();
		auto start = ImGui::GetCursorScreenPos();
		float width = ImGui::GetContentRegionAvail().x;
		const float title_size = S(11.5f);

		d->AddText(font_bold, title_size, ImVec2(start.x, floorf(start.y + (PANEL_TITLE_HEIGHT - title_size) * 0.5f)), C(col::text_faint),
			title, ImGui::FindRenderedTextEnd(title));
		d->AddLine(ImVec2(start.x, start.y + PANEL_TITLE_HEIGHT), ImVec2(start.x + width, start.y + PANEL_TITLE_HEIGHT), C(col::border));

		ImGui::Dummy(ImVec2(0, PANEL_TITLE_HEIGHT + S(5.f)));
	}

	void EndPanel() {
		ImGui::Dummy(ImVec2(0, S(6.f)));
		ImGui::EndChild();

		ImGui::PopStyleVar(4);
		ImGui::PopStyleColor();

		// On the column, so under what the panel drew: child windows are drawn after their parent
		auto min = ImGui::GetItemRectMin();
		auto max = ImGui::GetItemRectMax();
		auto d = ImGui::GetWindowDrawList();

		SoftShadow(d, min, max, PANEL_ROUNDING);
		d->AddRectFilled(min, max, C(col::panel), PANEL_ROUNDING);
		d->AddRectFilled(min, ImVec2(max.x, min.y + PANEL_TITLE_HEIGHT), C(col::panel_top), PANEL_ROUNDING, ImDrawFlags_RoundCornersTop);
		d->AddLine(ImVec2(min.x + PANEL_ROUNDING, min.y + 0.5f), ImVec2(max.x - PANEL_ROUNDING, min.y + 0.5f), C(col::highlight));
		d->AddRect(min, max, C(col::border), PANEL_ROUNDING, 0, 1.f);

		ImGui::Dummy(ImVec2(0, COLUMN_SPACING));
	}

	// The preview of the players sits on its window, no card under it (the agent of the game is seen through it): only
	// the title & its line
	void BeginPreview(const char* title) {
		ImGui::PushID(title);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.f), 0.f));
		ImGui::BeginGroup();

		auto d = ImGui::GetWindowDrawList();
		auto start = ImGui::GetCursorScreenPos();
		float width = ImGui::GetContentRegionAvail().x;
		const float title_size = S(11.5f);

		d->AddText(font_bold, title_size, ImVec2(start.x, floorf(start.y + (PANEL_TITLE_HEIGHT - title_size) * 0.5f)), C(col::text_faint),
			title, ImGui::FindRenderedTextEnd(title));
		d->AddLine(ImVec2(start.x, start.y + PANEL_TITLE_HEIGHT), ImVec2(start.x + width, start.y + PANEL_TITLE_HEIGHT), C(col::border));

		ImGui::Dummy(ImVec2(0, PANEL_TITLE_HEIGHT + S(5.f)));
	}

	void EndPreview() {
		ImGui::EndGroup();
		ImGui::PopStyleVar();
		ImGui::PopID();
	}

	// Rows, label on the left and widgets aligned to the right

	struct Row {
		ImVec2 start;
		float width;
		float right; // Where the next right-aligned widget ends
	};

	// Label of the next row in white: a toggle that is on
	bool row_lit = false;

	Row BeginRow(const char* label, const char* tooltip = nullptr) {
		Row row{};
		row.start = ImGui::GetCursorScreenPos();
		row.width = ImGui::GetContentRegionAvail().x;
		row.right = row.start.x + row.width;

		auto d = ImGui::GetWindowDrawList();

		auto hover_min = row.start + ImVec2(-S(6.f), 0.f);
		auto hover_max = row.start + ImVec2(row.width + S(6.f), ROW_HEIGHT);
		bool hovered = ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(hover_min, hover_max);
		float h = Animate(ImGui::GetID("##row_hover"), hovered ? 1.f : 0.f, 16.f);
		float lit = Animate(ImGui::GetID("##row_lit"), row_lit ? 1.f : 0.f, 12.f);
		row_lit = false;

		// What follows ## is only the id, like everywhere in ImGui
		auto label_end = ImGui::FindRenderedTextEnd(label);
		auto text_size = ImGui::CalcTextSize(label, label_end);
		auto text_pos = ImVec2(row.start.x, row.start.y + (ROW_HEIGHT - text_size.y) * 0.5f);

		d->AddText(text_pos, LerpColor(col::text_label, col::text, std::max(lit, h * 0.6f)), label, label_end);

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
				ShowTooltip("%s", tooltip);
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

		if (pressed)
			*value = !*value;

		float t = EaseOutCubic(Animate(item, *value ? 1.f : 0.f, 10.f));
		float hover = Animate(item + 1, ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);

		auto d = ImGui::GetWindowDrawList();
		auto max = pos + TOGGLE_SIZE;
		float radius = TOGGLE_SIZE.y * 0.5f;

		// Track: dark when off, the accent with a soft glow around it when on
		d->AddRectFilled(pos, max, LerpColor(col::track, col::track_hover, hover * 0.6f), radius);
		if (t > 0.01f) {
			SoftGlow(d, pos, max, col::accent, radius, t);
			d->AddRectFilled(pos, max, C(col::accent, t), radius);
		}
		d->AddRect(pos, max, LerpColor(col::border_hover, col::accent, t), radius);

		// Knob: grey on the left when off, light off the accent when on, a small shadow under it
		float knob_radius = radius - S(3.f);
		float x = pos.x + radius + t * (TOGGLE_SIZE.x - TOGGLE_SIZE.y);
		auto knob = ImVec2(x, pos.y + radius);
		d->AddCircleFilled(knob + ImVec2(0.f, S(1.f)), knob_radius, C(col::shadow, 0.35f), 24);
		d->AddCircleFilled(knob, knob_radius, LerpColor(LerpColor(col::text_dim, col::text, hover), OnAccent(), t), 24);

		return pressed;
	}

	void ColorSwatch(const char* id, color_t* color, ImVec2 pos, const char* tooltip) {
		ImGui::SetCursorScreenPos(pos);

		if (ImGui::InvisibleButton(id, ImVec2(SWATCH_SIZE, SWATCH_SIZE)))
			ImGui::OpenPopup(id);

		bool hovered = ImGui::IsItemHovered();
		float h = Animate(ImGui::GetItemID(), hovered || ImGui::IsPopupOpen(id) ? 1.f : 0.f, 16.f);

		if (tooltip && hovered)
			ShowTooltip("%s", tooltip);

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

		row_lit = *value;
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
	// The last slider was let go this frame, to act once the value is picked
	bool slider_released = false;

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
		slider_released = ImGui::IsItemDeactivated();

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
		const float half = S(2.5f);

		d->AddRectFilled(ImVec2(slider_pos.x, y - half), ImVec2(slider_pos.x + slider_width, y + half), C(col::track_hover), half);

		if (fill_x > slider_pos.x + 1.f)
			d->AddRectFilled(ImVec2(slider_pos.x, y - half), ImVec2(fill_x, y + half), C(col::accent), half);

		// Knob in the accent, a little bigger while held or hovered
		d->AddCircleFilled(ImVec2(fill_x, y), S(5.f) + hover * S(1.f), C(col::accent), 24);

		d->AddRectFilled(box_pos, box_pos + ImVec2(SLIDER_VALUE_WIDTH, box_height), LerpColor(col::track, col::track_hover, hover * 0.5f), S(4.f));
		d->AddRect(box_pos, box_pos + ImVec2(SLIDER_VALUE_WIDTH, box_height), C(col::border), S(4.f));

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

			fd->AddRectFilled(bubble_min, bubble_min + bubble_size, C(col::accent, drag), S(4.f));
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

		d->AddRectFilled(box_pos, box_pos + box_size, LerpColor(col::track, col::track_hover, hover), S(4.f));
		d->AddRect(box_pos, box_pos + box_size, waiting ? C(col::accent, 0.4f + 0.6f * pulse) : C(col::border), S(4.f), 0, 1.f);

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
		d->AddRectFilled(box, box + ImVec2(total, height), C(col::button), S(4.f));
		d->AddRect(box, box + ImVec2(total, height), C(col::border), S(4.f));

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

		d->AddRectFilled(box + ImVec2(slide_x, inset), box + ImVec2(slide_x + slide_w, height - inset), C(col::track_hover), S(3.f));

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
			auto color = index == *value ? C(col::text) : LerpColor(col::text_dim, col::text, hover * 0.6f);

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

		// As wide as its longest item wants, never over the label & its hint: a narrow column overlapped them
		float longest = 0.f;
		for (const char* item : items)
			longest = std::max(longest, ImGui::CalcTextSize(item).x);
		auto label_end = ImGui::FindRenderedTextEnd(label);
		float label_width = ImGui::CalcTextSize(label, label_end).x + (tooltip ? S(26.f) : 0.f) + S(12.f);
		width = std::max(width, longest + S(10.f) + S(26.f));
		width = std::max(S(80.f), std::min(width, row.width - label_width));
		// The list shows whole items, wider than the field when it has to be
		float list_width = std::max(width, longest + S(10.f) + S(12.f) + padding * 2.f);

		auto pos = RowSlot(row, ImVec2(width, height));
		ImGui::SetCursorScreenPos(pos);

		if (ImGui::InvisibleButton("##field", ImVec2(width, height)))
			ImGui::OpenPopup("##list");

		bool open = ImGui::IsPopupOpen("##list");
		float h = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || open ? 1.f : 0.f, 16.f);

		d->AddRectFilled(pos, pos + ImVec2(width, height), LerpColor(col::track, col::track_hover, h * 0.5f), S(3.f));
		d->AddRect(pos, pos + ImVec2(width, height), open ? C(col::border_hover) : C(col::border), S(3.f), 0, 1.f);

		int current = std::clamp(*value, 0, static_cast<int>(items.size()) - 1);
		auto text_size = ImGui::CalcTextSize(items[current]);
		d->PushClipRect(pos, pos + ImVec2(width - S(24.f), height), true);
		d->AddText(pos + ImVec2(S(10.f), (height - text_size.y) * 0.5f), C(col::text_dim), items[current]);
		d->PopClipRect();

		// Chevron, points up while open
		auto center = pos + ImVec2(width - S(13.f), height * 0.5f);
		float s = S(3.5f), flip = open ? -1.f : 1.f;
		d->AddTriangleFilled(center + ImVec2(-s, -s * 0.5f * flip), center + ImVec2(s, -s * 0.5f * flip), center + ImVec2(0.f, s * 0.6f * flip),
			LerpColor(col::text_dim, open ? col::accent : col::text, h));

		// The list: at most DROPDOWN_VISIBLE items, scrolls past that with a search on top. Less when the screen is short
		constexpr size_t DROPDOWN_VISIBLE = 8;
		bool searchable = items.size() > DROPDOWN_VISIBLE;
		const float search_height = searchable ? ImGui::GetTextLineHeight() + S(12.f) + S(6.f) : 0.f;
		float list_height = std::min(items.size(), DROPDOWN_VISIBLE) * item_height + padding * 2.f + search_height;
		float display = ImGui::GetIO().DisplaySize.y;
		float below = display - (pos.y + height + S(4.f)) - S(8.f);
		float above = pos.y - S(4.f) - S(8.f);
		bool up = list_height > below && above > below;
		float max_height = std::max(item_height * 3.f, up ? above : below);
		float shown = std::min(list_height, max_height);

		// Right edges lined up when the list is wider than the field
		float list_x = pos.x + width - list_width;
		ImGui::SetNextWindowPos(up ? ImVec2(list_x, pos.y - S(4.f) - shown) : ImVec2(list_x, pos.y + height + S(4.f)));
		ImGui::SetNextWindowSize(ImVec2(list_width, shown));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8.f));
		ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(4.f));

		bool changed = false;
		auto popup_flags = ImGuiWindowFlags_NoMove | (searchable ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
		if (ImGui::BeginPopup("##list", popup_flags)) {
			bool appearing = ImGui::IsWindowAppearing();

			// One list is open at a time, its search starts empty
			static char search[64] = "";
			if (appearing)
				search[0] = '\0';

			if (searchable) {
				ImGui::PushStyleColor(ImGuiCol_FrameBg, col::track);
				ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, col::track_hover);
				ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(4.f));
				ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(8.f), S(6.f)));

				if (appearing)
					ImGui::SetKeyboardFocusHere();
				ImGui::SetNextItemWidth(-FLT_MIN);
				ImGui::InputTextWithHint("##search", "Search...", search, sizeof(search));

				ImGui::PopStyleVar(2);
				ImGui::PopStyleColor(2);
				ImGui::Dummy(ImVec2(0.f, S(6.f)));
			}

			auto matches = [&](const char* text) {
				if (!search[0])
					return true;
				std::string a = text, b = search;
				std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return a.find(b) != std::string::npos;
			};

			ImGui::BeginChild("##items", ImVec2(0.f, 0.f), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
			auto list = ImGui::GetWindowDrawList();
			float inner = ImGui::GetContentRegionAvail().x;
			int found = 0;

			for (int i = 0; i < static_cast<int>(items.size()); i++) {
				if (!matches(items[i]))
					continue;
				found++;

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
					ShowTooltip("%s", descriptions[i]);

				ImGui::PopID();
			}

			// Opened at the selection
			if (appearing)
				ImGui::SetScrollY(std::max(0.f, current * item_height - (shown - search_height) * 0.5f));

			if (!found)
				ImGui::TextDisabled("Nothing found");

			ImGui::EndChild();
			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(4);

		EndRow(row);
		ImGui::PopID();
		return changed;
	}

	// Three dots that open more of a row in a small window under them
	ImVec2 DotsSize() {
		return ImVec2(S(20.f), S(20.f));
	}

	bool MoreDots(const char* popup, ImVec2 pos, const char* tooltip = nullptr) {
		auto size = DotsSize();

		ImGui::SetCursorScreenPos(pos);
		if (ImGui::InvisibleButton("##dots", size))
			ImGui::OpenPopup(popup);

		bool open = ImGui::IsPopupOpen(popup);
		bool hovered = ImGui::IsItemHovered();
		float h = Animate(ImGui::GetItemID(), hovered || open ? 1.f : 0.f, 16.f);

		if (tooltip && hovered && !open)
			ShowTooltip("%s", tooltip);

		auto d = ImGui::GetWindowDrawList();
		if (h > 0.01f)
			d->AddRectFilled(pos, pos + size, C(col::hover, h * 2.f), S(5.f));

		auto center = pos + size * 0.5f;
		auto color = open ? C(col::accent) : LerpColor(col::text_faint, col::text, h);
		for (int i = -1; i <= 1; i++)
			d->AddCircleFilled(center + ImVec2(i * S(4.5f), 0.f), S(1.7f), color, 8);

		return open;
	}

	// Width of the next window of dots, 0 for the usual one
	float next_more_width = 0.f;

	// The window of the dots at pos, under them with its right edge on theirs. Call EndMore() only when it returns true
	bool BeginMore(const char* popup, ImVec2 dots) {
		float width = next_more_width > 0.f ? next_more_width : S(260.f);
		next_more_width = 0.f;

		auto size = DotsSize();
		ImGui::SetNextWindowPos(ImVec2(dots.x + size.x, dots.y + size.y + S(4.f)), ImGuiCond_Appearing, ImVec2(1.f, 0.f));
		ImGui::SetNextWindowSize(ImVec2(width, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12.f), S(6.f)));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8.f), 0.f));

		if (ImGui::BeginPopup(popup, ImGuiWindowFlags_NoMove))
			return true;

		ImGui::PopStyleVar(2);
		return false;
	}

	void EndMore() {
		ImGui::EndPopup();
		ImGui::PopStyleVar(2);
	}

	// Toggle with three dots before it, they open what else the feature has. Its colors go before the dots
	template<typename Fn>
	bool ToggleMore(const char* label, bool* value, const char* tooltip, Fn&& more, color_t* color = nullptr, const char* color_tooltip = nullptr,
		color_t* second = nullptr, const char* second_tooltip = nullptr) {
		ImGui::PushID(label);

		row_lit = *value;
		auto row = BeginRow(label, tooltip);
		bool changed = ToggleSwitch("##toggle", value, RowSlot(row, TOGGLE_SIZE));
		auto dots = RowSlot(row, DotsSize());

		ImGui::BeginDisabled(!*value);
		if (color)
			ColorSwatch("##color", color, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), color_tooltip);
		if (second)
			ColorSwatch("##second", second, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), second_tooltip);
		ImGui::EndDisabled();

		MoreDots("##more", dots);
		if (BeginMore("##more", dots)) {
			more();
			EndMore();
		}

		EndRow(row);
		ImGui::PopID();
		return changed;
	}

	// A row of settings only: its colors in the order given, then the dots with the rest
	template<typename Fn>
	void MoreRow(const char* label, const char* tooltip, Fn&& more, std::initializer_list<std::pair<color_t*, const char*>> colors = {}) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto dots = RowSlot(row, DotsSize());

		// Slots go right to left, the last color first
		for (size_t i = colors.size(); i-- > 0;) {
			const auto& color = *(colors.begin() + i);
			ImGui::PushID(static_cast<int>(i));
			ColorSwatch("##color", color.first, RowSlot(row, ImVec2(SWATCH_SIZE, SWATCH_SIZE)), color.second);
			ImGui::PopID();
		}

		MoreDots("##more", dots);
		if (BeginMore("##more", dots)) {
			more();
			EndMore();
		}

		EndRow(row);
		ImGui::PopID();
	}

	// Toggle with three dots before it: they open the activation & the key, then what else is passed
	template<typename Fn>
	bool ToggleBindMore(const char* label, bool* value, int* mode, int* key, const char* tooltip, Fn&& more) {
		ImGui::PushID(label);

		row_lit = *value;
		auto row = BeginRow(label, tooltip);
		bool changed = ToggleSwitch("##toggle", value, RowSlot(row, TOGGLE_SIZE));
		auto dots = RowSlot(row, DotsSize());

		int current = std::clamp(*mode, 0, static_cast<int>(ACTIVATION_MODES.size()) - 1);
		auto hint = current == 2 ? std::string(ACTIVATION_MODES[current]) : std::format("{}: {}", ACTIVATION_MODES[current], KeyName(*key));
		MoreDots("##bind_popup", dots, hint.c_str());

		next_more_width = S(240.f);
		if (BeginMore("##bind_popup", dots)) {
			DropdownRow("Activation", mode, ACTIVATION_MODES, ACTIVATION_DESCRIPTIONS, nullptr, S(110.f));
			if (*mode != 2)
				KeybindRow("Key", key);
			more();
			EndMore();
		}

		EndRow(row);
		ImGui::PopID();
		return changed;
	}

	bool ToggleBind(const char* label, bool* value, int* mode, int* key, const char* tooltip = nullptr) {
		return ToggleBindMore(label, value, mode, key, tooltip, [] {});
	}

	// Like DropdownRow, any number of items picked: a click flips one & the list stays open
	bool MultiDropdownRow(const char* label, const std::vector<const char*>& items, const std::vector<bool*>& values,
		const char* tooltip = nullptr, float width = S(150.f)) {
		ImGui::PushID(label);

		auto row = BeginRow(label, tooltip);
		auto d = ImGui::GetWindowDrawList();

		const float height = S(26.f);
		const float item_height = S(28.f);
		const float padding = S(6.f);
		const float box = S(12.f);

		auto pos = RowSlot(row, ImVec2(width, height));
		ImGui::SetCursorScreenPos(pos);

		if (ImGui::InvisibleButton("##field", ImVec2(width, height)))
			ImGui::OpenPopup("##list");

		bool open = ImGui::IsPopupOpen("##list");
		float h = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || open ? 1.f : 0.f, 16.f);

		d->AddRectFilled(pos, pos + ImVec2(width, height), LerpColor(col::track, col::track_hover, h * 0.5f), S(3.f));
		d->AddRect(pos, pos + ImVec2(width, height), open ? C(col::border_hover) : C(col::border), S(3.f), 0, 1.f);

		// The picked ones by name, a count when they do not fit
		std::string preview;
		int picked = 0;
		for (size_t i = 0; i < items.size(); i++) {
			if (!*values[i])
				continue;
			preview += (picked++ ? ", " : "") + std::string(items[i]);
		}
		if (picked == 0)
			preview = "None";
		else if (ImGui::CalcTextSize(preview.c_str()).x > width - S(34.f))
			preview = std::to_string(picked) + " selected";

		auto text_size = ImGui::CalcTextSize(preview.c_str());
		d->PushClipRect(pos, pos + ImVec2(width - S(24.f), height), true);
		d->AddText(pos + ImVec2(S(10.f), (height - text_size.y) * 0.5f), C(picked ? col::text : col::text_dim), preview.c_str());
		d->PopClipRect();

		// Chevron, points up while open
		auto center = pos + ImVec2(width - S(13.f), height * 0.5f);
		float s = S(3.5f), flip = open ? -1.f : 1.f;
		d->AddTriangleFilled(center + ImVec2(-s, -s * 0.5f * flip), center + ImVec2(s, -s * 0.5f * flip), center + ImVec2(0.f, s * 0.6f * flip),
			LerpColor(col::text_dim, open ? col::accent : col::text, h));

		float list_height = items.size() * item_height + padding * 2.f;
		float below = ImGui::GetIO().DisplaySize.y - (pos.y + height + S(4.f)) - S(8.f);
		bool up = list_height > below && pos.y - S(12.f) > below;

		ImGui::SetNextWindowPos(up ? ImVec2(pos.x, pos.y - S(4.f) - list_height) : ImVec2(pos.x, pos.y + height + S(4.f)));
		ImGui::SetNextWindowSize(ImVec2(width, list_height));

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.f, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(8.f));

		bool changed = false;
		if (ImGui::BeginPopup("##list", ImGuiWindowFlags_NoMove)) {
			auto list = ImGui::GetWindowDrawList();
			float inner = ImGui::GetContentRegionAvail().x;

			for (int i = 0; i < static_cast<int>(items.size()); i++) {
				ImGui::PushID(i);
				auto min = ImGui::GetCursorScreenPos();

				if (ImGui::InvisibleButton("##item", ImVec2(inner, item_height))) {
					*values[i] = !*values[i];
					changed = true;
				}

				bool selected = *values[i];
				float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 18.f);
				float check = Animate(ImGui::GetItemID() + 1, selected ? 1.f : 0.f, 18.f);

				if (hover > 0.01f)
					list->AddRectFilled(min, min + ImVec2(inner, item_height), C(col::hover, hover), S(6.f));

				// Check box, filled with the accent when picked
				auto box_min = min + ImVec2(S(8.f), (item_height - box) * 0.5f);
				list->AddRectFilled(box_min, box_min + ImVec2(box, box), LerpColor(col::track_hover, col::accent, check), S(3.f));
				if (check > 0.01f) {
					list->AddPolyline(std::array<ImVec2, 3>{
						box_min + ImVec2(box * 0.22f, box * 0.52f),
						box_min + ImVec2(box * 0.42f, box * 0.72f),
						box_min + ImVec2(box * 0.78f, box * 0.30f) }.data(), 3, C(col::on_accent, check), 0, S(1.6f));
				}

				auto size = ImGui::CalcTextSize(items[i]);
				list->AddText(min + ImVec2(S(10.f) + box + S(8.f), (item_height - size.y) * 0.5f),
					selected ? C(col::text) : LerpColor(col::text_dim, col::text, hover), items[i]);

				ImGui::PopID();
			}

			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(3);

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
			ShowTooltip("%s", tooltip);
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
			ShowTooltip("%s", tooltip);

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
		return ImageCache::Small(url);
	}

	// Picture of a weapon without a skin, for the ones the icon font has no glyph of. Already small, no SmallImage
	std::string BaseImage(int index) {
		if (index == weapon_mp5sd)
			return "https://raw.githubusercontent.com/ByMykel/counter-strike-image-tracker/main/static/panorama/images/econ/weapons/base_weapons/weapon_mp5sd_png.png";

		return "";
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
	bool ItemCard(const char* id, ImVec2 size, const std::string& image, const char* icon, const char* title, const char* subtitle, ImU32 rarity, bool selected, bool marked = false, bool figure = false, float aspect = 4.f / 3.f) {
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
			// Fit, keeping the shape of the pictures (4:3 of the items)
			float w = picture_max.x - picture_min.x, h = picture_max.y - picture_min.y;
			float fit_w = std::min(w, h * aspect) * (1.f + 0.04f * hover);
			auto fit = ImVec2(fit_w, fit_w / aspect);

			d->AddImage(texture, center - fit * 0.5f, center + fit * 0.5f, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE));
		}
		else if (icon) {
			const float icon_size = S(36.f);
			auto icon_extent = font_regular->CalcTextSizeA(icon_size, FLT_MAX, 0.f, icon);
			d->AddText(font_regular, icon_size, center - icon_extent * 0.5f, LerpColor(col::text_faint, col::text_dim, hover), icon);
		}
		else if (figure) {
			// A player without a picture: head & shoulders
			float h = (picture_max.y - picture_min.y) * (1.f + 0.04f * hover);
			auto color = LerpColor(col::text_faint, col::text_dim, hover);
			d->AddCircleFilled(center - ImVec2(0.f, h * 0.17f), h * 0.15f, color, 32);
			d->AddRectFilled(center + ImVec2(-h * 0.27f, h * 0.05f), center + ImVec2(h * 0.27f, h * 0.40f), color, h * 0.18f, ImDrawFlags_RoundCornersTop);
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

	Grid BeginGrid(float height = CARD_HEIGHT, float min_width = CARD_MIN_WIDTH) {
		Grid grid{};
		grid.start = ImGui::GetCursorScreenPos();

		float available = ImGui::GetContentRegionAvail().x;
		grid.columns = std::max(1, static_cast<int>((available + CARD_GAP) / (min_width + CARD_GAP)));
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

	// The rest of the page, scrolled on its own: what is above it (back, search) stays
	void BeginScrollArea(const char* id) {
		ImGui::BeginChild(id, ImVec2(0.f, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
	}

	void EndScrollArea() {
		ImGui::EndChild();
	}

	// Large cards, this many a line whether the scrollbar shows or not, their height from the 4:3 pictures
	Grid BeginFixedGrid(int columns) {
		float width = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ScrollbarSize;
		float card_width = (width - CARD_GAP * (columns - 1)) / columns;
		return BeginGrid((card_width - S(20.f)) * 3.f / 4.f + S(60.f), card_width - 1.f);
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

		d->AddRectFilled(pos, pos + ImVec2(total, height), C(col::button), S(8.f));
		d->AddRect(pos, pos + ImVec2(total, height), C(col::border), S(8.f));

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
		auto slide_min = pos + ImVec2(slide_x, inset), slide_max = pos + ImVec2(slide_x + slide_w, height - inset);
		SoftGlow(d, slide_min, slide_max, col::accent, S(6.f), 0.7f);
		d->AddRectFilled(slide_min, slide_max, C(col::accent), S(6.f));

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
			auto color = index == *value ? C(OnAccent()) : LerpColor(col::text_dim, col::text, hover * 0.6f);
			d->AddText(pos + ImVec2(x + padding, (height - text_size.y) * 0.5f), color, item);

			ImGui::PopID();
			x += w;
			index++;
		}

		ImGui::PopID();
		return changed;
	}

	float TabSwitchWidth(std::initializer_list<const char*> items) {
		float total = S(3.f) * 2.f;
		for (auto item : items)
			total += ImGui::CalcTextSize(item).x + S(16.f) * 2.f;
		return total;
	}

	// Pages of a category as pills, the lit one slides between them. The index clicked, -1 when none
	int PagePills(const char* id, const std::vector<const char*>& items, int active, ImVec2 pos, float height) {
		ImGui::PushID(id);

		auto d = ImGui::GetWindowDrawList();
		const float padding = S(15.f);
		const float gap = S(4.f);
		const float rounding = height * 0.5f;

		float x = 0.f, selected_x = 0.f, selected_w = 0.f;
		for (int i = 0; i < static_cast<int>(items.size()); i++) {
			float w = ImGui::CalcTextSize(items[i]).x + padding * 2.f;
			if (i == active) {
				selected_x = x;
				selected_w = w;
			}
			x += w + gap;
		}

		float slide_x = Animate(ImGui::GetID("##x"), selected_x, 16.f);
		float slide_w = Animate(ImGui::GetID("##w"), selected_w, 16.f);
		auto lit_min = pos + ImVec2(slide_x, 0.f), lit_max = pos + ImVec2(slide_x + slide_w, height);

		SoftGlow(d, lit_min, lit_max, col::accent, rounding, 0.9f);
		d->AddRectFilled(lit_min, lit_max, C(col::accent), rounding);

		int clicked = -1;
		x = 0.f;

		for (int i = 0; i < static_cast<int>(items.size()); i++) {
			auto text_size = ImGui::CalcTextSize(items[i]);
			float w = text_size.x + padding * 2.f;
			auto min = pos + ImVec2(x, 0.f);

			ImGui::PushID(i);
			ImGui::SetCursorScreenPos(min);
			if (ImGui::InvisibleButton("##pill", ImVec2(w, height)) && i != active)
				clicked = i;

			float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() && i != active ? 1.f : 0.f, 16.f);
			float lit = Animate(ImGui::GetItemID() + 1, i == active ? 1.f : 0.f, 14.f);

			if (hover > 0.01f)
				d->AddRectFilled(min, min + ImVec2(w, height), C(col::hover, hover * 2.f), rounding);

			auto color = LerpColor(LerpColor(col::text_dim, col::text, hover), OnAccent(), lit);
			d->AddText(min + ImVec2(padding, (height - text_size.y) * 0.5f), color, items[i]);

			ImGui::PopID();
			x += w + gap;
		}

		ImGui::PopID();
		return clicked;
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

		row_lit = *value;
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
			ShowTooltip(placed ? "Drag to move, right click to remove" : "Drag next to the box, or click to add it");

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
			ShowTooltip("Drag to turn the camera");

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

	// The pictures of the agents taken from the game (AgentPreview), loaded again when one was taken. The
	// shape: all white, tinted for the glow, made once it is turned on
	struct AgentPicture {
		ImTextureID texture = 0;
		ImTextureID shape = 0;
		bool shape_tried = false;
		float width = 0.f, height = 0.f;
		std::string bytes;
	};

	AgentPicture* GetAgentPicture(bool terrorist, bool shape) {
		static AgentPicture pictures[2];
		static int generation = -1;

		if (generation != AgentPreview::GetGeneration()) {
			generation = AgentPreview::GetGeneration();
			for (int team = 0; team < 2; team++) {
				auto& picture = pictures[team];
				for (auto texture : { picture.texture, picture.shape })
					if (texture)
						reinterpret_cast<ID3D11ShaderResourceView*>(texture)->Release();
				picture = {};

				std::ifstream file(AgentPreview::PicturePath(team == 0), std::ios::binary);
				if (!file)
					continue;
				picture.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
				picture.texture = ImageCache::FromMemory(picture.bytes.data(), picture.bytes.size());

				// Its size from the header of the PNG: width & height big endian at 16
				if (picture.texture && picture.bytes.size() > 24) {
					auto big = [&](size_t at) {
						auto b = reinterpret_cast<const uint8_t*>(picture.bytes.data()) + at;
						return static_cast<float>((b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]);
					};
					picture.width = big(16);
					picture.height = big(20);
				}
			}
		}

		auto& picture = pictures[terrorist ? 0 : 1];
		if (!picture.texture || picture.width <= 0.f || picture.height <= 0.f)
			return nullptr;
		if (shape && !picture.shape_tried) {
			picture.shape_tried = true;
			picture.shape = ImageCache::FromMemory(picture.bytes.data(), picture.bytes.size(), nullptr, 0, true);
		}
		return &picture;
	}

	// What is drawn after it is added over what is behind (like the hologram of the game), until
	// ImDrawCallback_ResetRenderState
	void AdditiveBlend(const ImDrawList*, const ImDrawCmd*) {
		auto state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
		static ID3D11BlendState* additive = nullptr;
		if (!state)
			return;
		if (!additive) {
			D3D11_BLEND_DESC desc{};
			desc.RenderTarget[0].BlendEnable = TRUE;
			desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			desc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
			desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
			desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (FAILED(state->Device->CreateBlendState(&desc, &additive)))
				return;
		}
		const float factor[4] = { 0.f, 0.f, 0.f, 0.f };
		state->DeviceContext->OMSetBlendState(additive, factor, 0xFFFFFFFF);
	}

	// A texture in a rect, its corners tinted each in their color: top left, top right, bottom right, bottom left
	void AddImageCorners(ImDrawList* d, ImTextureID texture, ImVec2 min, ImVec2 max, ImU32 top_left, ImU32 top_right, ImU32 bottom_right, ImU32 bottom_left) {
		if (!texture)
			return;
		d->PushTexture(ImTextureRef(texture));
		d->PrimReserve(6, 4);
		auto index = static_cast<ImDrawIdx>(d->_VtxCurrentIdx);
		d->PrimWriteIdx(index); d->PrimWriteIdx(static_cast<ImDrawIdx>(index + 1)); d->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2));
		d->PrimWriteIdx(index); d->PrimWriteIdx(static_cast<ImDrawIdx>(index + 2)); d->PrimWriteIdx(static_cast<ImDrawIdx>(index + 3));
		d->PrimWriteVtx(min, ImVec2(0.f, 0.f), top_left);
		d->PrimWriteVtx(ImVec2(max.x, min.y), ImVec2(1.f, 0.f), top_right);
		d->PrimWriteVtx(max, ImVec2(1.f, 1.f), bottom_right);
		d->PrimWriteVtx(ImVec2(min.x, max.y), ImVec2(0.f, 1.f), bottom_left);
		d->PopTexture();
	}

	// The picture of the agent: head to feet fills the rect, feet on its bottom. Gives the rect of the picture.
	// material: of the material chams where it is seen (-1 none) in color, drawn about like the game draws it
	ImRect DrawAgentPicture(ImDrawList* d, const AgentPicture& picture, ImVec2 min, ImVec2 max, ImU32 glow, int material, const color_t& color) {
		namespace mc = cfg::visuals::material_chams;

		float scale = (max.y - min.y) / picture.height;
		float width = picture.width * scale;
		ImVec2 image_min(floorf((min.x + max.x) * 0.5f - width * 0.5f), min.y);
		ImVec2 image_max(image_min.x + width, max.y);
		const ImVec2 uv_min(0.f, 0.f), uv_max(1.f, 1.f);

		auto image = [&](ImTextureID texture, ImU32 tint) {
			if (texture)
				d->AddImage(ImTextureRef(texture), image_min, image_max, uv_min, uv_max, tint);
		};
		auto in_color = [&](float alpha) { return ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, color.a * alpha)); };

		if (glow && picture.shape)
			DrawGlowAround(glow, [&](ImVec2 offset, ImU32 shade) {
				d->AddImage(ImTextureRef(picture.shape), image_min + offset, image_max + offset, uv_min, uv_max, shade);
			});

		switch (material) {
		case mc::MATERIAL_FLAT:
			// Unlit: the shape in the color, a trace of the model under it
			image(picture.texture, C(IM_COL32_WHITE, 1.f - color.a));
			image(picture.shape, in_color(1.f));
			break;
		case mc::MATERIAL_GLOW:
			// The model, its edges lit in the color
			image(picture.texture, C(IM_COL32_WHITE));
			DrawGlowAround(in_color(0.8f), [&](ImVec2 offset, ImU32 shade) {
				d->AddImage(ImTextureRef(picture.shape), image_min + offset, image_max + offset, uv_min, uv_max, shade);
			});
			image(picture.shape, in_color(0.25f));
			break;
		case mc::MATERIAL_HOLOGRAM:
			// Unlit, no texture: the color added over what is behind, so it shows through. The faces behind are drawn
			// too, about twice the color all over
			d->AddCallback(AdditiveBlend, nullptr);
			image(picture.shape, in_color(0.8f));
			d->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
			break;
		case mc::MATERIAL_METALLIC: {
			// White metal tinted in the color, its own glow keeps it the color: the body in the color, shaded by its
			// form, the light of the map shining on it from above
			image(picture.shape, in_color(1.f));
			image(picture.texture, in_color(0.5f));
			auto shine = [&](float a) { return ImGui::GetColorU32(ImVec4(1.f, 1.f, 1.f, a * color.a)); };
			d->AddCallback(AdditiveBlend, nullptr);
			AddImageCorners(d, picture.shape, image_min, image_max, shine(0.35f), shine(0.18f), shine(0.f), shine(0.06f));
			image(picture.shape, in_color(0.12f));
			d->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
			break;
		}
		default:
			image(picture.texture, C(IM_COL32_WHITE));
			break;
		}
		return ImRect(image_min, image_max);
	}

	// The joints of the agents in their pictures, 0 - 1 of the picture from its top left: the pose of the game is the same
	// every time it is taken. The arm & leg on the left of the picture first
	enum AgentJoint {
		JOINT_HEAD, JOINT_NECK, JOINT_CHEST, JOINT_SPINE, JOINT_PELVIS,
		JOINT_SHOULDER_L, JOINT_ELBOW_L, JOINT_HAND_L, JOINT_SHOULDER_R, JOINT_ELBOW_R, JOINT_HAND_R,
		JOINT_HIP_L, JOINT_KNEE_L, JOINT_FOOT_L, JOINT_HIP_R, JOINT_KNEE_R, JOINT_FOOT_R,
		JOINT_COUNT
	};

	// Measured on agent_t_v2.png & agent_ct_v3.png (2026-10-10): pictures of a new version need them measured again
	constexpr ImVec2 AGENT_JOINTS[2][JOINT_COUNT] = {
		// Number K
		{ { 0.41f, 0.065f }, { 0.42f, 0.14f }, { 0.43f, 0.22f }, { 0.44f, 0.32f }, { 0.45f, 0.45f },
		  { 0.18f, 0.18f }, { 0.08f, 0.32f }, { 0.34f, 0.34f }, { 0.70f, 0.17f }, { 0.86f, 0.35f }, { 0.87f, 0.52f },
		  { 0.36f, 0.49f }, { 0.30f, 0.70f }, { 0.34f, 0.95f }, { 0.57f, 0.49f }, { 0.71f, 0.70f }, { 0.82f, 0.94f } },
		// SAS
		{ { 0.34f, 0.08f }, { 0.38f, 0.15f }, { 0.42f, 0.24f }, { 0.44f, 0.33f }, { 0.45f, 0.46f },
		  { 0.15f, 0.19f }, { 0.17f, 0.32f }, { 0.61f, 0.35f }, { 0.66f, 0.18f }, { 0.84f, 0.31f }, { 0.70f, 0.37f },
		  { 0.32f, 0.51f }, { 0.30f, 0.69f }, { 0.30f, 0.95f }, { 0.60f, 0.51f }, { 0.70f, 0.69f }, { 0.79f, 0.95f } },
	};

	constexpr int AGENT_BONES[][2] = {
		{ JOINT_HEAD, JOINT_NECK }, { JOINT_NECK, JOINT_CHEST }, { JOINT_CHEST, JOINT_SPINE }, { JOINT_SPINE, JOINT_PELVIS },
		{ JOINT_NECK, JOINT_SHOULDER_L }, { JOINT_SHOULDER_L, JOINT_ELBOW_L }, { JOINT_ELBOW_L, JOINT_HAND_L },
		{ JOINT_NECK, JOINT_SHOULDER_R }, { JOINT_SHOULDER_R, JOINT_ELBOW_R }, { JOINT_ELBOW_R, JOINT_HAND_R },
		{ JOINT_PELVIS, JOINT_HIP_L }, { JOINT_HIP_L, JOINT_KNEE_L }, { JOINT_KNEE_L, JOINT_FOOT_L },
		{ JOINT_PELVIS, JOINT_HIP_R }, { JOINT_HIP_R, JOINT_KNEE_R }, { JOINT_KNEE_R, JOINT_FOOT_R },
	};

	ImVec2 AgentJointAt(bool terrorist, int joint, const ImRect& image) {
		auto at = AGENT_JOINTS[terrorist ? 0 : 1][joint];
		return ImVec2(image.Min.x + at.x * image.GetWidth(), image.Min.y + at.y * image.GetHeight());
	}

	// The skeleton on the picture, lines like the ESP draws them
	void DrawAgentSkeleton(ImDrawList* d, bool terrorist, const ImRect& image, const color_t& color) {
		for (const auto& bone : AGENT_BONES)
			d->AddLine(AgentJointAt(terrorist, bone[0], image), AgentJointAt(terrorist, bone[1], image), ImColor(color), 1.5f);
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

	// Category of the sidebar, lit with the accent & a soft glow when picked
	bool SidebarItem(const char* icon, const char* label, bool active, ImVec2 pos, ImVec2 size) {
		ImGui::PushID(label);

		ImGui::SetCursorScreenPos(pos);
		bool pressed = ImGui::InvisibleButton("##tab", size);
		bool hovered = ImGui::IsItemHovered();

		float t = Animate(ImGui::GetItemID(), active ? 1.f : 0.f, 12.f);
		float h = Animate(ImGui::GetItemID() + 1, hovered && !active ? 1.f : 0.f, 16.f);

		auto d = ImGui::GetWindowDrawList();
		const float rounding = S(8.f);

		if (t > 0.01f) {
			SoftGlow(d, pos, pos + size, col::accent, rounding, t * 0.9f);
			d->AddRectFilled(pos, pos + size, C(col::accent, t), rounding);
		}
		if (h > 0.01f)
			d->AddRectFilled(pos, pos + size, C(col::hover, h * 2.f), rounding);

		auto text_y = pos.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;

		// Icons fit a box in front of the label, wide weapon icons are made smaller
		const float icon_box = S(18.f);
		auto font = ImGui::GetFont();
		float font_size = ImGui::GetFontSize() * 0.9f;
		auto icon_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, icon);
		if (icon_size.x > icon_box) {
			font_size *= icon_box / icon_size.x;
			icon_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.f, icon);
		}

		auto color = LerpColor(LerpColor(col::text_dim, col::text, h), OnAccent(), t);
		auto icon_pos = ImVec2(pos.x + S(14.f) + (icon_box - icon_size.x) * 0.5f, pos.y + (size.y - icon_size.y) * 0.5f);
		d->AddText(font, font_size, ImVec2(floorf(icon_pos.x), floorf(icon_pos.y)), color, icon);
		d->AddText(ImVec2(pos.x + S(44.f), text_y), color, label);

		ImGui::PopID();
		return pressed;
	}
}

static void RefreshTheme();

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
	if (ProcessPreset(Config::Preset::CONFIG, config_presets))
		Window::SetAffinity(Window::hwnd, cfg::settings::streamproof ? WindowAffinity::Invisible : WindowAffinity::Disabled);
	ProcessPreset(Config::Preset::SKINS, skin_presets);

	col::accent = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg::settings::accent.r, cfg::settings::accent.g, cfg::settings::accent.b, 1.f));
	col::backdrop = std::clamp(cfg::settings::menu_opacity, 0.3f, 1.f);
	RefreshTheme();

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
	ImGui::PushFont(font_regular, S(15.f));

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

static void ProfilePanel();
static void ModBrowserPanel();
static bool HasPickedMod();

// Previews of the tabs in their own panel next to the menu, so the options fit without scrolling. The profile page
// of the misc tab shows the player picked there
void Menu::RenderPreviewPanel(float alpha) {
	bool profile = active_tab == Tab::MISC && misc_page == 1;
	bool mod = active_tab == Tab::SKINS && skin_page == SkinPage::MODEL_BROWSER && HasPickedMod();
	bool skin = active_tab == Tab::SKINS && (skin_page == SkinPage::SKINS || skin_page == SkinPage::GLOVES ||
		skin_page == SkinPage::KNIVES || skin_page == SkinPage::AGENTS || skin_page == SkinPage::MUSIC_KITS);

	if (active_tab != Tab::PLAYERS && active_tab != Tab::BOMB && active_tab != Tab::PROJECTILES && active_tab != Tab::ITEMS && !profile && !mod && !skin)
		return;

	auto& io = ImGui::GetIO();
	const float width = S(380.f);
	const float gap = S(12.f);

	// Right of the menu, on its left when the screen ends first
	ImVec2 at(pos.x + size.x + gap, pos.y);
	if (at.x + width > io.DisplaySize.x && pos.x - gap - width >= 0.f)
		at.x = pos.x - gap - width;

	float tab = EaseOutCubic(tab_progress);
	if (profile)
		tab *= EaseOutCubic(misc_page_progress);	// Comes in with the page too
	if (mod || skin)
		tab *= EaseOutCubic(skin_page_progress);

	ImGui::SetNextWindowPos(at, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(width, size.y), ImGuiCond_Always);

	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(CONTENT_PADDING, CONTENT_PADDING));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);

	auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings;

	// Closing, it is only fading out
	if (!Renderer::IsOpen())
		flags |= ImGuiWindowFlags_NoInputs;

	bool visible = ImGui::Begin("##preview_panel", nullptr, flags);
	ImGui::PopStyleVar(2);

	if (visible) {
		auto d = ImGui::GetWindowDrawList();
		auto min = ImGui::GetWindowPos();
		auto max = min + ImGui::GetWindowSize();

		Outline(ImGui::GetBackgroundDrawList(), min, max, WINDOW_ROUNDING);
		d->AddRectFilled(min, max, C(col::window, col::backdrop), WINDOW_ROUNDING);
		d->AddRect(min, max, C(col::border), WINDOW_ROUNDING, 0, 1.f);

		// Slides & fades in with the tab
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.f - tab) * TAB_SLIDE);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha * tab);

		switch (active_tab) {
		case Tab::PLAYERS:      RenderPlayersPreview();     break;
		case Tab::BOMB:         RenderBombPreview();        break;
		case Tab::PROJECTILES:  RenderProjectilesPreview(); break;
		case Tab::ITEMS:        RenderItemsPreview();       break;
		case Tab::MISC:         ProfilePanel();             break;
		case Tab::SKINS:        RenderSkinPanel();          break;
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
	Outline(bg, pos, max, WINDOW_ROUNDING);

	// Sidebar & content, the game a little visible through them. Light from above on the top edge
	d->AddRectFilled(pos, ImVec2(pos.x + SIDEBAR_WIDTH, max.y), C(col::sidebar, col::backdrop), WINDOW_ROUNDING, ImDrawFlags_RoundCornersLeft);
	d->AddRectFilled(ImVec2(pos.x + SIDEBAR_WIDTH, pos.y), max, C(col::window, col::backdrop), WINDOW_ROUNDING, ImDrawFlags_RoundCornersRight);

	d->AddLine(ImVec2(pos.x + SIDEBAR_WIDTH, pos.y), ImVec2(pos.x + SIDEBAR_WIDTH, max.y), C(col::border));
	d->AddLine(ImVec2(pos.x + WINDOW_ROUNDING, pos.y + 0.5f), ImVec2(max.x - WINDOW_ROUNDING, pos.y + 0.5f), C(col::highlight));
	d->AddRect(pos, max, C(col::border), WINDOW_ROUNDING, 0, 1.f);
}

void Menu::RenderSidebar() {
	auto d = ImGui::GetWindowDrawList();

	// Brand: the name big & bold in the middle, a 1 px accent shadow under it
	{
		const float brand_size = S(24.f);
		auto brand_width = font_bold->CalcTextSizeA(brand_size, FLT_MAX, 0.f, BRAND_TITLE).x;
		auto brand_pos = ImVec2(floorf(pos.x + (SIDEBAR_WIDTH - brand_width) * 0.5f), pos.y + S(22.f));

		d->AddText(font_bold, brand_size, brand_pos + ImVec2(1.f, 0.f), C(col::accent, 0.6f), BRAND_TITLE);
		d->AddText(font_bold, brand_size, brand_pos, C(col::text), BRAND_TITLE);

		// A short line of the accent under it, fading out at both ends
		float line_y = brand_pos.y + brand_size + S(12.f);
		float center = pos.x + SIDEBAR_WIDTH * 0.5f;
		float half = S(34.f);
		d->AddRectFilledMultiColor(ImVec2(center - half, line_y), ImVec2(center, line_y + S(2.f)),
			C(col::accent, 0.f), C(col::accent), C(col::accent), C(col::accent, 0.f));
		d->AddRectFilledMultiColor(ImVec2(center, line_y), ImVec2(center + half, line_y + S(2.f)),
			C(col::accent), C(col::accent, 0.f), C(col::accent, 0.f), C(col::accent));

	}

	// Bottom: the author's GitHub picture, name & the version under a line. Clicked it opens the credits & the accent
	{
		const float avatar = S(30.f);
		auto line_y = pos.y + size.y - S(50.f);
		d->AddLine(ImVec2(pos.x, line_y), ImVec2(pos.x + SIDEBAR_WIDTH, line_y), C(col::border));

		auto area_min = ImVec2(pos.x + S(8.f), line_y + S(4.f));
		auto area_max = ImVec2(pos.x + SIDEBAR_WIDTH - S(8.f), pos.y + size.y - S(4.f));

		ImGui::SetCursorScreenPos(area_min);
		if (ImGui::InvisibleButton("##author", area_max - area_min))
			ImGui::OpenPopup("##author_popup");

		bool popup_open = ImGui::IsPopupOpen("##author_popup");
		float h = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() || popup_open ? 1.f : 0.f, 16.f);
		if (h > 0.01f)
			d->AddRectFilled(area_min, area_max, C(col::hover, h * 1.6f), S(5.f));

		auto avatar_min = ImVec2(pos.x + S(15.f), pos.y + size.y - S(40.f));
		auto avatar_center = avatar_min + ImVec2(avatar, avatar) * 0.5f;
		auto texture = ImageCache::Get(AUTHOR_AVATAR);

		if (texture)
			d->AddImageRounded(texture, avatar_min, avatar_min + ImVec2(avatar, avatar), ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE), avatar * 0.5f);
		else
			d->AddCircleFilled(avatar_center, avatar * 0.5f, C(col::track_hover), 24);

		auto text_x = avatar_min.x + avatar + S(8.f);
		d->AddText(font_regular, S(14.f), ImVec2(text_x, avatar_min.y), LerpColor(col::text_label, col::text, std::max(h, 0.8f)), AUTHOR);

		constexpr auto version_label = "Version: ";
		auto version = std::to_string(Updater::GetVersion());
		auto label_width = font_regular->CalcTextSizeA(S(13.f), FLT_MAX, 0.f, version_label).x;
		d->AddText(font_regular, S(13.f), ImVec2(text_x, avatar_min.y + S(15.f)), C(col::text_dim), version_label);
		d->AddText(font_regular, S(13.f), ImVec2(text_x + label_width, avatar_min.y + S(15.f)), C(col::accent), version.c_str());

		// Small arrow on the right, up while open
		auto arrow = ImVec2(area_max.x - S(12.f), (area_min.y + area_max.y) * 0.5f);
		float dir = popup_open ? -1.f : 1.f;
		d->AddTriangleFilled(
			arrow + ImVec2(-S(3.5f), -S(2.f) * dir),
			arrow + ImVec2(S(3.5f), -S(2.f) * dir),
			arrow + ImVec2(0.f, S(2.f) * dir),
			LerpColor(col::text_faint, col::text, h));

		// The popup grows up from the bottom left, next to the sidebar
		const float popup_width = S(250.f);
		ImGui::SetNextWindowPos(ImVec2(pos.x + S(8.f), line_y - S(6.f)), ImGuiCond_Always, ImVec2(0.f, 1.f));
		ImGui::SetNextWindowSize(ImVec2(popup_width, 0.f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14.f), S(14.f)));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, S(6.f));

		if (ImGui::BeginPopup("##author_popup", ImGuiWindowFlags_NoMove)) {
			auto pd = ImGui::GetWindowDrawList();
			auto start = ImGui::GetCursorScreenPos();
			float inner = ImGui::GetContentRegionAvail().x;

			// Author, the name opens the GitHub page
			const float big = S(40.f);
			if (texture)
				pd->AddImageRounded(texture, start, start + ImVec2(big, big), ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE), big * 0.5f);
			else
				pd->AddCircleFilled(start + ImVec2(big, big) * 0.5f, big * 0.5f, C(col::track_hover), 24);

			auto name_pos = start + ImVec2(big + S(10.f), S(3.f));
			pd->AddText(font_bold, S(16.f), name_pos, C(col::text), AUTHOR);

			constexpr auto link = "github.com/Keydak";
			auto link_pos = name_pos + ImVec2(0.f, S(19.f));
			auto link_size = font_regular->CalcTextSizeA(S(13.f), FLT_MAX, 0.f, link);
			ImGui::SetCursorScreenPos(link_pos);
			if (ImGui::InvisibleButton("##github", link_size))
				ShellExecuteW(nullptr, L"open", AUTHOR_URL, nullptr, nullptr, SW_SHOWNORMAL);

			bool link_hovered = ImGui::IsItemHovered();
			if (link_hovered)
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			pd->AddText(font_regular, S(13.f), link_pos, link_hovered ? C(col::accent) : C(col::text_dim), link);
			if (link_hovered)
				pd->AddLine(link_pos + ImVec2(0.f, link_size.y), link_pos + link_size, C(col::accent));

			ImGui::SetCursorScreenPos(start + ImVec2(0.f, big + S(12.f)));

			auto section = [&](const char* title) {
				auto at = ImGui::GetCursorScreenPos();
				pd->AddLine(at, at + ImVec2(inner, 0.f), C(col::border));
				pd->AddText(font_regular, S(12.f), at + ImVec2(0.f, S(9.f)), C(col::text_faint), title);
				ImGui::Dummy(ImVec2(inner, S(30.f)));
			};

			// Credits
			section("CREDITS");
			for (const auto& credit : CREDITS) {
				auto at = ImGui::GetCursorScreenPos();
				pd->AddText(font_regular, S(13.f), at, C(col::text), credit.name);
				auto what_size = font_regular->CalcTextSizeA(S(12.f), FLT_MAX, 0.f, credit.what);
				pd->AddText(font_regular, S(12.f), ImVec2(at.x + inner - what_size.x, at.y + S(1.f)), C(col::text_dim), credit.what);
				ImGui::Dummy(ImVec2(inner, S(19.f)));
			}

			ImGui::Dummy(ImVec2(inner, S(4.f)));

			// Accent: the quick ones, then any color
			section("ACCENT");
			{
				const float dot = S(20.f);
				const float gap = S(8.f);
				auto at = ImGui::GetCursorScreenPos();
				auto current = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg::settings::accent.r, cfg::settings::accent.g, cfg::settings::accent.b, 1.f));

				for (int i = 0; i < IM_ARRAYSIZE(ACCENTS); i++) {
					auto dot_min = at + ImVec2(i * (dot + gap), 0.f);
					auto center = dot_min + ImVec2(dot, dot) * 0.5f;

					ImGui::PushID(i);
					ImGui::SetCursorScreenPos(dot_min);
					if (ImGui::InvisibleButton("##accent", ImVec2(dot, dot))) {
						auto value = ImGui::ColorConvertU32ToFloat4(ACCENTS[i]);
						cfg::settings::accent.r = value.x;
						cfg::settings::accent.g = value.y;
						cfg::settings::accent.b = value.z;
						cfg::settings::accent.a = 1.f;
					}
					float dh = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.f : 0.f, 16.f);
					ImGui::PopID();

					pd->AddCircleFilled(center, dot * 0.5f - S(1.f) + dh, C(ACCENTS[i]), 24);
					if (current == ACCENTS[i])
						pd->AddCircle(center, dot * 0.5f + S(3.f), C(col::text, 0.85f), 24, 1.5f);
				}

				// Any other color
				ImGui::PushID("##accent_custom");
				ColorSwatch("##custom", &cfg::settings::accent,
					at + ImVec2(IM_ARRAYSIZE(ACCENTS) * (dot + gap) + (dot - SWATCH_SIZE) * 0.5f, (dot - SWATCH_SIZE) * 0.5f), "Any other color");
				ImGui::PopID();

				ImGui::SetCursorScreenPos(at + ImVec2(0.f, dot + S(2.f)));
				ImGui::Dummy(ImVec2(inner, 0.f));
			}

			// How much the game shows through the menu
			ImGui::Dummy(ImVec2(inner, S(8.f)));
			section("BACKGROUND");
			{
				int percent = static_cast<int>(std::round(cfg::settings::menu_opacity * 100.f));
				if (SliderInt("Opacity", &percent, 30, 100, "%d%%"))
					cfg::settings::menu_opacity = percent / 100.f;
			}

			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(2);
	}

	// Categories, the pages of one are pills in the header. Visuals & settings open on the page last seen
	bool visuals = active_tab == Tab::PLAYERS || active_tab == Tab::ITEMS || active_tab == Tab::PROJECTILES || active_tab == Tab::BOMB;
	bool settings = active_tab == Tab::SETTINGS || active_tab == Tab::CONFIGS;
	if (visuals)
		last_visuals_tab = active_tab;
	if (settings)
		last_settings_tab = active_tab;

	struct Entry {
		const char* label;
		const char* icon;
		Tab tab;
		bool active;
	};

	const Entry entries[] = {
		{ "Visuals", Icons::PEOPLE, last_visuals_tab, visuals },
		{ "Skins", WeaponIcons::AK47, Tab::SKINS, active_tab == Tab::SKINS },
		{ "Movement", Icons::PERSON, Tab::MOVEMENT, active_tab == Tab::MOVEMENT },
		{ "Misc", Icons::GLOBE, Tab::MISC, active_tab == Tab::MISC },
		{ "Settings", Icons::SETTINGS, last_settings_tab, settings },
	};

	const float margin = S(12.f);
	const ImVec2 item_size(SIDEBAR_WIDTH - margin * 2.f, S(38.f));
	float y = pos.y + S(80.f);

	for (const auto& entry : entries) {
		if (SidebarItem(entry.icon, entry.label, entry.active, ImVec2(pos.x + margin, y), item_size) && !entry.active) {
			active_tab = entry.tab;
			tab_progress = 0.f;
		}

		y += item_size.y + S(6.f);
	}
}

void Menu::RenderHeader() {
	auto d = ImGui::GetWindowDrawList();

	float tab = EaseOutCubic(tab_progress);
	float x = pos.x + SIDEBAR_WIDTH + CONTENT_PADDING;
	const float pill_height = S(30.f);
	float pill_y = pos.y + (HEADER_HEIGHT - pill_height) * 0.5f;

	bool visuals = active_tab == Tab::PLAYERS || active_tab == Tab::ITEMS || active_tab == Tab::PROJECTILES || active_tab == Tab::BOMB;
	bool settings = active_tab == Tab::SETTINGS || active_tab == Tab::CONFIGS;

	// The pages of the category, its name when it has one page
	if (visuals) {
		static const std::vector<const char*> pages = { "Players", "Items", "Projectiles", "Bomb" };
		const Tab tabs[] = { Tab::PLAYERS, Tab::ITEMS, Tab::PROJECTILES, Tab::BOMB };

		int current = 0;
		for (int i = 0; i < IM_ARRAYSIZE(tabs); i++)
			if (tabs[i] == active_tab)
				current = i;

		int clicked = PagePills("##visual_pages", pages, current, ImVec2(x, pill_y), pill_height);
		if (clicked >= 0) {
			active_tab = tabs[clicked];
			tab_progress = 0.f;
		}
	}
	else if (active_tab == Tab::MISC) {
		static const std::vector<const char*> pages = { "Camera", "Profile", "Interface", "Effects" };

		int clicked = PagePills("##misc_pages", pages, misc_page, ImVec2(x, pill_y), pill_height);
		if (clicked >= 0) {
			misc_page = clicked;
			misc_page_progress = 0.f;
		}
	}
	else if (settings) {
		static const std::vector<const char*> pages = { "General", "Configs" };

		int clicked = PagePills("##settings_pages", pages, active_tab == Tab::CONFIGS ? 1 : 0, ImVec2(x, pill_y), pill_height);
		if (clicked >= 0) {
			active_tab = clicked == 1 ? Tab::CONFIGS : Tab::SETTINGS;
			tab_progress = 0.f;
		}
	}
	else {
		// Slides in with the content
		const char* title = active_tab == Tab::SKINS ? "Skins" : "Movement";
		float slide = (1.f - tab) * 10.f;
		d->AddText(font_bold, S(19.f), ImVec2(x + slide, pos.y + (HEADER_HEIGHT - S(19.f)) * 0.5f), C(col::text, tab), title);
	}

	// Players: team or enemy, then the switch of that group at the right edge
	if (active_tab == Tab::PLAYERS) {
		auto& group = esp_group == 0 ? cfg::esp::team : cfg::esp::enemy;
		float right = pos.x + size.x - CONTENT_PADDING;

		auto switch_pos = ImVec2(right - TOGGLE_SIZE.x, pos.y + (HEADER_HEIGHT - TOGGLE_SIZE.y) * 0.5f);
		ToggleSwitch("##group_enabled", &group.enabled, switch_pos);

		constexpr auto label = "ESP";
		auto label_size = ImGui::CalcTextSize(label);
		float label_x = switch_pos.x - S(10.f) - label_size.x;
		d->AddText(ImVec2(label_x, pos.y + (HEADER_HEIGHT - label_size.y) * 0.5f), C(group.enabled ? col::text : col::text_dim), label);

		float group_width = TabSwitchWidth({ "Team", "Enemy" });
		TabSwitch("##esp_group", &esp_group, { "Team", "Enemy" }, ImVec2(label_x - S(16.f) - group_width, pill_y), pill_height);
	}
}

// The flags not placed on the preview: a button opens them by kind, clicked they go where they usually are, dragged next
// to the box of the preview where they are dropped. A placed flag dragged on it is taken off
static void AddFlagRow(cfg::esp::group_t& group, ImFont* title_font) {
	auto d = ImGui::GetWindowDrawList();
	auto payload = ImGui::GetDragDropPayload();
	bool dragging = payload && payload->IsDataType(FLAG_PAYLOAD);
	auto mouse = ImGui::GetIO().MousePos;
	float width = ImGui::GetContentRegionAvail().x;

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

	auto row_min = ImGui::GetCursorScreenPos();
	auto row_max = row_min + ImVec2(width, CHIP_HEIGHT + S(8.f));
	std::optional<int> added, removed;

	if (dragging) {
		// Where a placed flag goes back
		bool hovered = ImRect(row_min, row_max).Contains(mouse);
		d->AddRectFilled(row_min, row_max, C(hovered ? col::track_hover : col::track), S(6.f));
		d->AddRect(row_min, row_max, C(col::accent, hovered ? 0.8f : 0.35f), S(6.f));
		const char* text = "Drop here to take it off";
		auto text_size = ImGui::CalcTextSize(text);
		d->AddText(ImVec2(floorf(row_min.x + (width - text_size.x) * 0.5f), floorf(row_min.y + (row_max.y - row_min.y - text_size.y) * 0.5f)), C(col::text_dim), text);
	}
	else {
		auto label = unused.empty() ? std::string("Every flag is placed") : std::format("+  Add Flag  ({})", unused.size());
		ImGui::BeginDisabled(unused.empty());
		if (PillButton(label.c_str(), row_min, row_max - row_min, false, "The flags not on the preview yet: click one to add it, or drag it next to the box of the preview"))
			ImGui::OpenPopup("##add_flag");
		ImGui::EndDisabled();
	}

	if (ImGui::BeginDragDropTargetCustom(ImRect(row_min, row_max), ImGui::GetID("##flag_row"))) {
		if (auto accepted = ImGui::AcceptDragDropPayload(FLAG_PAYLOAD))
			removed = *static_cast<const int*>(accepted->Data);
		ImGui::EndDragDropTarget();
	}

	// Under the row, as wide. Stays open while a flag is dragged out of it
	ImGui::SetNextWindowPos(ImVec2(row_min.x, row_max.y + S(6.f)), ImGuiCond_Appearing);
	ImGui::SetNextWindowSize(ImVec2(width, 0.f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(10.f), S(10.f)));
	if (ImGui::BeginPopup("##add_flag", ImGuiWindowFlags_NoMove)) {
		struct Kind {
			const char* name;
			std::vector<int> flags;
		};
		namespace e = cfg::esp;
		const Kind kinds[] = {
			{ "INFO", { e::FLAG_NAME, e::FLAG_HEALTH_BAR, e::FLAG_ARMOR_BAR, e::FLAG_MONEY, e::FLAG_PING, e::FLAG_WEAPON,
				e::FLAG_WEAPON_NAME, e::FLAG_AMMO, e::FLAG_DISTANCE } },
			{ "STATES", { e::FLAG_RELOADING_TEXT, e::FLAG_SCOPED_TEXT, e::FLAG_FLASHED_TEXT, e::FLAG_DEFUSING, e::FLAG_C4, e::FLAG_KIT } },
			{ "ICONS", { e::FLAG_RELOADING, e::FLAG_SCOPED, e::FLAG_FLASHED } },
		};

		auto popup_draw = ImGui::GetWindowDrawList();
		float inner = ImGui::GetContentRegionAvail().x;
		bool any = false;
		for (const auto& kind : kinds) {
			std::vector<int> shown;
			for (int flag : kind.flags)
				if (std::find(unused.begin(), unused.end(), flag) != unused.end())
					shown.push_back(flag);
			if (shown.empty())
				continue;

			if (any)
				ImGui::Dummy(ImVec2(0.f, S(8.f)));
			any = true;

			auto title_at = ImGui::GetCursorScreenPos();
			popup_draw->AddText(title_font, S(11.f), title_at, C(col::text_faint), kind.name);
			ImGui::Dummy(ImVec2(inner, S(16.f)));

			auto chips_min = ImGui::GetCursorScreenPos();
			auto positions = LayoutChips(shown, chips_min, chips_min + ImVec2(inner, 0.f), false);
			float chips_height = CHIP_HEIGHT;
			for (const auto& p : positions)
				chips_height = std::max(chips_height, p.y + CHIP_HEIGHT - chips_min.y);

			for (size_t i = 0; i < shown.size(); i++)
				if (FlagChip(shown[i], positions[i], false) == 1)
					added = shown[i];

			ImGui::SetCursorScreenPos(chips_min);
			ImGui::Dummy(ImVec2(inner, chips_height));
		}

		if (added || unused.empty())
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();

	ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_max.y));
	ImGui::Dummy(ImVec2(width, S(6.f)));

	if (removed)
		RemoveFlag(group, *removed);
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
}

void Menu::RenderPlayersPreview() {
	auto& group = esp_group == 0 ? cfg::esp::team : cfg::esp::enemy;

	// The layout of the flags around a preview of the player
	ImGui::BeginDisabled(!group.enabled);
	{
		BeginPreview("LAYOUT");

		auto d = ImGui::GetWindowDrawList();
		auto preview_min = ImGui::GetCursorScreenPos();
		auto preview_size = ImVec2(ImGui::GetContentRegionAvail().x, S(470.f));
		auto preview_max = preview_min + preview_size;

		d->AddRectFilled(preview_min, preview_max, C(col::window), S(8.f));

		// Box in the middle, big like the agent in it, room around it for the flags
		auto box_size = ImVec2(floorf(preview_size.x * 0.52f), floorf(preview_size.y * 0.84f));
		auto box_min = ImVec2(floorf(preview_min.x + (preview_size.x - box_size.x) * 0.5f), floorf(preview_min.y + preview_size.y * 0.08f));
		auto box_max = box_min + box_size;

		auto payload = ImGui::GetDragDropPayload();
		bool dragging = payload && payload->IsDataType(FLAG_PAYLOAD);
		auto mouse = ImGui::GetIO().MousePos;

		d->PushClipRect(preview_min, preview_max, true);

		// Always the same: enemies are T (Number K), the team is CT
		bool terrorist = esp_group != 0;

		// The picture of the agent taken from the game when the program started
		auto picture = GetAgentPicture(terrorist, false);

		ImGui::SetCursorScreenPos(box_min);
		ImGui::InvisibleButton("##model", box_size);

		// The picture of the agent, the plain model without it (no -insecure yet to take it), a figure without both
		auto model_min = box_min + ImVec2(0.f, box_size.y * 0.04f), model_max = box_max - ImVec2(0.f, box_size.y * 0.015f);
		auto glow = esp_group == 0
			? PreviewGlow(cfg::visuals::glow::team, cfg::visuals::glow::team_color)
			: PreviewGlow(cfg::visuals::glow::enemies, cfg::visuals::glow::enemy_color);

		// No material chams on the preview (user's choice: they did not look like the game), the agent as it is
		constexpr int material = -1;
		const color_t material_color{};

		std::optional<ImRect> image;
		if (picture) {
			GetAgentPicture(terrorist, glow || material >= 0);	// Its shape for the glow & the materials, made once needed
			image = DrawAgentPicture(d, *picture, model_min, model_max, glow, material, material_color);
		}
		else if (auto model = GetPreviewModel(terrorist ? "t" : "ct"))
			DrawModel(d, *model, model_min, model_max, 0.f, material >= 0
				? ImGui::ColorConvertFloat4ToU32(ImVec4(material_color.r, material_color.g, material_color.b, 1.f))
				: IM_COL32(205, 205, 212, 255), glow);
		else {
			auto figure_inset = ImVec2(box_size.x * 0.14f, box_size.y * 0.05f);
			DrawFigure(d, box_min + figure_inset, box_max - figure_inset, C(col::text_faint, 0.9f));
		}

		// What the ESP draws on a player, with the same code
		auto [preview_local, preview_player] = PreviewPlayers();

		if (group.box)
			Esp::DrawBox(d, box_min, box_max, group.box_visible, group.box_style);
		else if (dragging)
			d->AddRect(box_min, box_max, C(col::border_hover), 0.f, 0, 1.f);

		// Skeleton on the agent, the head tracker around its head about the size it has in the game
		if (image && group.skeleton)
			DrawAgentSkeleton(d, terrorist, *image, group.skeleton_visible);
		if (group.head_tracker) {
			auto head_at = image
				? AgentJointAt(terrorist, JOINT_HEAD, *image)
				: ImVec2((model_min.x + model_max.x) * 0.5f, model_min.y + (model_max.y - model_min.y) * 0.06f);
			float head_radius = (model_max.y - model_min.y) * 0.045f;
			Esp::DrawTracker(d, head_at, head_radius * 6.f, group.tracker_visible);
		}

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
				ShowTooltip("%s\nDrag to move, right click to remove", FlagName(area.flag));
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

		EndPreview();
	}
	ImGui::EndDisabled();
}

// Rows of one group of the material chams: where it is seen, behind walls (null: never behind one), the material
static void MaterialChamsRows(cfg::visuals::material_chams::group_t& chams, const char* visible_note, const char* hidden_note,
	const char* visible_label = "Visible") {
	constexpr auto unavailable = "Only available when the game is launched with -insecure";
	static const std::vector<const char*> materials = { "Flat", "Glow", "Hologram", "Metallic" };
	static const char* material_descriptions[] = {
		"One color, no light or texture",
		"Only the edges of the model lit in the color",
		"See through, its color added over what is behind (black adds nothing: invisible)",
		"Shiny metal in the color, lit by the map",
	};
	const char* missing = Engine::IsInsecure()
		? "The drawing code of the game was not found in this game version"
		: unavailable;
	bool available = MaterialChams::IsAvailable();

	ImGui::BeginDisabled(!available);
	Toggle(visible_label, &chams.visible.enabled, available ? visible_note : missing, nullptr, &chams.visible.color);
	if (hidden_note) {
		bool behind_walls = MaterialChams::HasBehindWalls(chams.material);
		ImGui::BeginDisabled(!behind_walls);
		Toggle("Behind Walls", &chams.hidden.enabled, !available
			? missing
			: behind_walls
			? hidden_note
			: "Only with Flat & Glow", nullptr, &chams.hidden.color);
		ImGui::EndDisabled();
	}

	// Always there, so the panel keeps its size
	ImGui::BeginDisabled(!chams.visible.enabled && !chams.hidden.enabled);
	DropdownRow("Material", &chams.material, materials, material_descriptions,
		material_descriptions[std::clamp(chams.material, 0, cfg::visuals::material_chams::MATERIAL_COUNT - 1)]);
	ImGui::EndDisabled();
	ImGui::EndDisabled();

	if (auto status = MaterialChams::GetStatus(); !status.empty() && (chams.visible.enabled || chams.hidden.enabled))
		TextBlock(("Not working yet: " + status).c_str());
}

void Menu::RenderPlayersTab() {
	auto& group = esp_group == 0 ? cfg::esp::team : cfg::esp::enemy;
	const bool writes = Visuals::IsAvailable();
	constexpr auto unavailable = "Only available when the game is launched with -insecure";

	// Two columns, team or enemy & the switch of the group are in the header. The preview is in the panel next to the menu
	BeginColumn(0);
	ImGui::BeginDisabled(!group.enabled);
	{
		BeginPanel("ESP");
		static const std::vector<const char*> box_styles = { "Full", "Corners", "Rounded", "Filled" };
		static const char* box_style_descriptions[] = {
			"All four sides",
			"Only its corners",
			"All four sides, round corners",
			"All four sides, the inside lightly filled",
		};
		ToggleMore("Box", &group.box, nullptr, [&] {
			DropdownRow("Style", &group.box_style, box_styles, box_style_descriptions, "How the box is drawn");
		}, &group.box_visible, "Visible", &group.box_invisible, "Behind a wall");
		ToggleColors("Skeleton", &group.skeleton, nullptr, &group.skeleton_visible, &group.skeleton_invisible);
		ToggleColors("Head Tracker", &group.head_tracker, nullptr, &group.tracker_visible, &group.tracker_invisible);
		ToggleColors("Tracers", &group.tracers, nullptr, &group.tracer_visible, &group.tracer_invisible);
		Toggle("Visible Only", &group.visible_only);
		MoreRow("Bar Numbers", "The health & armor as numbers on their bars", [&] {
			Toggle("Health Number", &group.health_number);
			Toggle("Armor Number", &group.armor_number);
		});

		if (esp_group == 0 && Cache::Current()->game.deathmatch)
			TextBlock("Deathmatch: everyone is an enemy, the Enemy ESP is used for all players");
		EndPanel();

		// Size & color of each kind of flag, the sizes behind the dots
		BeginPanel("FLAGS");
		AddFlagRow(group, font_bold);
		MoreRow("Text", "Name, weapon, ping, money...", [&] {
			SliderFloat("Size", &group.text_size, 8.f, 24.f, "%.0f px");
		}, { { &group.text, "Color" } });
		MoreRow("Icons", "Weapon, kit, C4 & the other icons", [&] {
			SliderFloat("Size", &group.icon_size, 8.f, 28.f, "%.0f px");
		}, { { &group.icon, "Flashed, reloading, scoped & kit icons" } });
		MoreRow("States", "FLASHED, RELOADING, SCOPED & DEFUSING", [&] {
			SliderFloat("Size", &group.state_size, 8.f, 24.f, "%.0f px");
		}, { { &group.flashed, "Flashed" }, { &group.reloading, "Reloading" }, { &group.scoped, "Scoped" }, { &group.defusing, "Defusing" } });
		EndPanel();
	}
	ImGui::EndDisabled();
	EndColumn();

	BeginColumn(1);
	ImGui::BeginDisabled(!group.enabled);
	{
		// Written into the game
		BeginPanel("GAME");
		ImGui::BeginDisabled(!writes);
		Toggle("Outline Glow", esp_group == 0 ? &cfg::visuals::glow::team : &cfg::visuals::glow::enemies,
			writes ? nullptr : unavailable,
			nullptr, esp_group == 0 ? &cfg::visuals::glow::team_color : &cfg::visuals::glow::enemy_color);

		ImGui::EndDisabled();
		EndPanel();

		// Our own materials drawn by the game, where the player is seen & behind walls
		BeginPanel("MATERIAL CHAMS");
		MaterialChamsRows(esp_group == 0 ? cfg::visuals::material_chams::team : cfg::visuals::material_chams::enemy,
			"The player in our material where they can be seen",
			"The player in our material through walls & smokes. A player partly behind a wall is split where the wall is");
		EndPanel();
	}
	ImGui::EndDisabled();
	EndColumn();
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
		Toggle("Bomb ESP", &cfg::esp::bomb, nullptr, nullptr, &cfg::esp::colors::bomb);

		ImGui::BeginDisabled(!Visuals::IsAvailable());
		Toggle("Outline Glow", &cfg::visuals::glow::bomb, Visuals::IsAvailable()
			? "Outline of the game around the planted bomb, seen through walls.\nThe dropped bomb: in the items tab"
			: "Only available when the game is launched with -insecure", nullptr, &cfg::visuals::glow::bomb_color);
		ImGui::EndDisabled();
		EndPanel();

		BeginPanel("MATERIAL CHAMS");
		MaterialChamsRows(cfg::visuals::material_chams::bomb,
			"The planted bomb in our material where it can be seen.\nThe dropped bomb: in the items tab",
			"The planted bomb in our material through walls & smokes");
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("BOMB WINDOW");
		Toggle("Bomb Location", &cfg::world::bomb::location);
		Toggle("Bomb Timer", &cfg::world::bomb::timer);
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
	namespace gr = cfg::esp::grenades;
	namespace gc = cfg::esp::colors::grenades;
	const bool writes = Visuals::IsAvailable();

	// What is drawn, the preview is in the panel next to the menu
	BeginColumn(0);
	{
		BeginPanel("GRENADES");
		ToggleMore("Grenade ESP", &gr::enabled, nullptr, [&] {
			SliderFloat("Text Size", &gr::text_size, 8.f, 28.f, "%.0f px");
		});

		ImGui::BeginDisabled(!gr::enabled);
		ToggleMore("Trails", &gr::trails, nullptr, [&] {
			Toggle("Grenade Colors", &gr::trail_type_color, "Color each trail like its grenade instead of using the trail color");
		}, &gc::trail, "Trail color");
		Toggle("Throw Preview", &gr::prediction, "Path, bounces & landing spot of the grenade in your hand\nUses the map collision, built from the game files the first time a map is played");
		Toggle("Landing Prediction", &gr::landing, "Where grenades in the air are going to land or explode, thrown by anyone\nUses the map collision too");
		ImGui::EndDisabled();

		ImGui::BeginDisabled(!writes);
		ToggleMore("Outline Glow", &cfg::visuals::glow::thrown, writes
			? "Outline of the game around grenades in the air, seen through walls.\nGrenades on the ground: in the items tab"
			: "Only available when the game is launched with -insecure", [&] {
			namespace thrown = cfg::visuals::glow::thrown_colors;
			ColorRow("Smoke", &thrown::smoke);
			ColorRow("Molotov", &thrown::molotov, "Also the incendiary");
			ColorRow("Flash", &thrown::flash);
			ColorRow("HE", &thrown::he);
			ColorRow("Decoy", &thrown::decoy);
		});
		ImGui::EndDisabled();
		EndPanel();

		BeginPanel("COLORS");
		ImGui::BeginDisabled(!gr::enabled);
		ColorRow("Smoke", &gc::smoke);
		ColorRow("Molotov", &gc::molotov, "Also used for the fire area");
		ColorRow("Flash", &gc::flash);
		ColorRow("HE", &gc::he);
		ColorRow("Decoy", &gc::decoy);
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("LOOK");
		ImGui::BeginDisabled(!gr::enabled);
		Toggle("Smoke Area", &gr::smoke_area, "Outline & fill of a popped smoke on the ground.\nIts icon & timer stay");
		Toggle("Fire Area", &gr::fire_area, "Outline & fill of a molotov or incendiary fire on the ground.\nIts icon & timer stay");
		Toggle("Glow", &gr::glow, "Popped smokes & burning fires glow on the ground");
		Toggle("Icons", &gr::icons);
		Toggle("Names", &gr::names);
		ToggleMore("Timers", &gr::timers, nullptr, [&] {
			Toggle("Timer Bars", &gr::timer_bars);
		});
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
	const bool writes = Visuals::IsAvailable();
	constexpr auto unavailable = "Only available when the game is launched with -insecure";

	// A kind of item: on & its color, what is shown of it behind the dots
	auto kind_row = [&](const char* label, it::category_t& category, bool has_ammo) {
		ToggleMore(label, &category.enabled, nullptr, [&] {
			Toggle("Icon", &category.icon);
			Toggle("Name", &category.name, "Always shown when the icon is off");
			if (has_ammo)
				Toggle("Ammo", &category.ammo);
			Toggle("Distance", &category.distance);
		}, &category.color, "Color");
	};

	// What is drawn, the preview is in the panel next to the menu
	BeginColumn(0);
	{
		BeginPanel("ITEMS");
		ToggleMore("Item ESP", &it::enabled, "Weapons, grenades, the bomb & defuse kits nobody holds", [&] {
			SliderFloat("Max Distance", &it::max_distance, 5.f, 200.f, "%.0f m");
			SliderFloat("Text Size", &it::text_size, 8.f, 24.f, "%.0f px");
			SliderFloat("Icon Size", &it::icon_size, 8.f, 28.f, "%.0f px");
		});

		ImGui::BeginDisabled(!it::enabled);
		kind_row("Weapons", it::weapons, true);
		kind_row("Grenades", it::utility, false);
		kind_row("Dropped Bomb", it::bomb, false);
		kind_row("Defuse Kits", it::kits, false);
		ImGui::EndDisabled();
		EndPanel();
	}
	EndColumn();

	BeginColumn(1);
	{
		BeginPanel("OUTLINE GLOW");
		ImGui::BeginDisabled(!writes);
		Toggle("Weapons", &cfg::visuals::glow::items, writes
			? "Outline of the game around weapons & defuse kits on the ground, seen through walls"
			: unavailable, nullptr, &cfg::visuals::glow::item_color);
		Toggle("Grenades", &cfg::visuals::glow::utility, writes
			? "Outline of the game around grenades on the ground, seen through walls.\nThrown grenades: in the projectiles tab"
			: unavailable, nullptr, &cfg::visuals::glow::utility_color);
		Toggle("Dropped Bomb", &cfg::visuals::glow::dropped_bomb, writes
			? "Outline of the game around the dropped bomb, seen through walls.\nThe planted bomb: in the bomb tab"
			: unavailable, nullptr, &cfg::visuals::glow::dropped_bomb_color);
		ImGui::EndDisabled();
		EndPanel();

		BeginPanel("MATERIAL CHAMS");
		MaterialChamsRows(cfg::visuals::material_chams::items,
			"Weapons, grenades, the dropped bomb & defuse kits on the ground in our material where they can be seen",
			"The same through walls & smokes");
		EndPanel();
	}
	EndColumn();
}

// Player models of the user (csgo/characters & csgo/agents of the game) are cards of the agent list. Above it, what
// goes with them: the state of the one picked, moving misplaced ones, the folder
// True when the GameBanana browser is asked for
static bool CustomModelTools(cfg::skins::loadout_t& loadout, const std::vector<CustomModel>& models) {
	// One row, no notes: the agent cards right below it
	if (!CustomModels::IsAvailable())
		return false;

	// Models put in another folder than the one they were made for: moved there on a click
	static std::string move_error;
	for (const auto& model : models) {
		if (model.place.empty() || !model.fits_there)
			continue;

		auto label = std::format("Move {} to its folder##move_{}", model.name, model.resource);
		auto tooltip = std::format("Moves the folder of {} (with its materials) to csgo/{}", model.name, model.place);
		if (PillButton(label.c_str(), ImGui::GetCursorScreenPos(), ImVec2(ImGui::GetContentRegionAvail().x, S(28.f)), false, tooltip.c_str())) {
			move_error.clear();
			if (CustomModels::MoveToPlace(model, move_error) && loadout.custom_model == model.resource)
				loadout.custom_model = model.made_for;      // Still the one picked, at its new place
		}
		ImGui::Dummy(ImVec2(0.f, S(4.f)));
	}

	// What the game did with the one picked, or why a move failed: a line next to the buttons
	auto picked = std::find_if(models.begin(), models.end(), [&](const CustomModel& model) { return model.resource == loadout.custom_model; });
	std::string status = move_error;
	if (status.empty() && !loadout.custom_model.empty())
		status = picked != models.end() ? CustomModels::LoadStatus(loadout.custom_model) : std::string("Not in the folder anymore");

	auto pos = ImGui::GetCursorScreenPos();
	auto size = ImVec2(std::min(S(110.f), (ImGui::GetContentRegionAvail().x - S(12.f)) / 3.f), S(28.f));
	const float row_x = pos.x;
	float status_height = 0.f;

	bool browse = PillButton("Download##custom_browse", pos, size, true, "Player models of GameBanana, put in the game with one click");
	pos.x += size.x + S(6.f);

	if (PillButton("Refresh##custom_refresh", pos, size, false, "Look at the folders again, and try a model the game could not load once more"))
		CustomModels::Rescan();

	// The folder of the model picked, else where new ones can go
	if (PillButton("Open Folder##custom_folder", pos + ImVec2(size.x + S(6.f), 0.f), size, false,
		picked != models.end() ? "Opens the folder of the model picked" : "Opens the csgo folder of the game, models go in characters/ or agents/")) {
		auto folder = picked != models.end() ? picked->file.parent_path() : CustomModels::Folder().parent_path().parent_path();
		if (!folder.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(folder, ec);
			ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
	}

	if (!status.empty()) {
		auto at = pos + ImVec2((size.x + S(6.f)) * 2.f + S(4.f), 0.f);
		float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
		// No room next to the buttons: under them
		if (right - at.x < S(100.f)) {
			at = ImVec2(row_x, pos.y + size.y + S(4.f));
			status_height = size.y + S(4.f);
		}
		// Cut with "..." where it does not fit, whole in the tooltip
		std::string shown = status;
		bool cut = false;
		while (!shown.empty() && ImGui::CalcTextSize((shown + (cut ? "..." : "")).c_str()).x > right - at.x) {
			shown.pop_back();
			while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80)
				shown.pop_back();   // Not half a character
			if (!shown.empty() && static_cast<unsigned char>(shown.back()) >= 0xC0)
				shown.pop_back();
			cut = true;
		}
		while (cut && !shown.empty() && shown.back() == ' ')
			shown.pop_back();
		if (cut)
			shown += "...";

		auto text = ImGui::CalcTextSize(shown.c_str());
		ImGui::GetWindowDrawList()->AddText(at + ImVec2(0.f, (size.y - text.y) * 0.5f), C(col::text_dim), shown.c_str());

		if (cut && ImGui::IsMouseHoveringRect(at, ImVec2(right, at.y + size.y)))
			ShowTooltip("%s", status.c_str());
	}

	ImGui::SetCursorScreenPos(ImVec2(row_x, pos.y));
	ImGui::Dummy(ImVec2(0.f, size.y + status_height + S(6.f)));
	return browse;
}

// A picture of a mod, 16:9 over the given rect, a little closer on hover
static void ModPicture(ImDrawList* d, const std::string& image, ImVec2 min, ImVec2 max, float hover, float rounding, ImDrawFlags corners) {
	auto texture = image.empty() ? ImTextureID{} : ImageCache::Get(image);
	if (!texture) {
		d->AddRectFilled(min, max, C(col::track), rounding, corners);
		return;
	}

	float zoom = 0.03f * hover;
	d->AddImageRounded(texture, min, max, ImVec2(zoom, zoom), ImVec2(1.f - zoom, 1.f - zoom), C(IM_COL32_WHITE), rounding, corners);
}

// A GameBanana mod: its picture over the whole width, a badge of what it got to, its name & who made it
static bool ModCard(const char* id, ImVec2 size, const ModBrowser::Mod& mod, const char* badge, ImU32 badge_color, float progress, bool selected) {
	ImGui::PushID(id);

	auto pos = ImGui::GetCursorScreenPos();
	bool pressed = ImGui::InvisibleButton("##mod", size);
	auto item = ImGui::GetItemID();
	float hover = Animate(item, ImGui::IsItemHovered() ? 1.f : 0.f, 14.f);
	float active = Animate(item + 1, selected ? 1.f : 0.f, 12.f);

	auto d = ImGui::GetWindowDrawList();
	auto min = pos - ImVec2(0.f, S(2.f) * hover);
	auto max = min + size;
	const float rounding = S(10.f);

	d->AddRectFilled(min, max, LerpColor(col::panel, col::selected, std::max(hover * 0.5f, active)), rounding);

	auto picture_max = ImVec2(max.x, min.y + size.x * 9.f / 16.f);
	ModPicture(d, mod.image, min, picture_max, hover, rounding, ImDrawFlags_RoundCornersTop);

	// Download progress along the bottom of the picture
	if (progress >= 0.f) {
		auto bar = ImVec2(min.x, picture_max.y - S(4.f));
		d->AddRectFilled(bar, picture_max, C(col::track));
		d->AddRectFilled(bar, ImVec2(min.x + size.x * std::clamp(progress, 0.f, 1.f), picture_max.y), C(col::accent));
	}

	// What it got to, on the picture
	if (badge && *badge) {
		const float text_size = S(11.f);
		auto text = font_bold->CalcTextSizeA(text_size, FLT_MAX, 0.f, badge);
		auto badge_min = min + ImVec2(S(8.f), S(8.f));
		auto badge_max = badge_min + ImVec2(text.x + S(22.f), text.y + S(8.f));
		d->AddRectFilled(badge_min, badge_max, IM_COL32(0, 0, 0, 185), (badge_max.y - badge_min.y) * 0.5f);
		d->AddCircleFilled(ImVec2(badge_min.x + S(9.f), (badge_min.y + badge_max.y) * 0.5f), S(3.f), badge_color, 12);
		d->AddText(font_bold, text_size, ImVec2(badge_min.x + S(16.f), badge_min.y + S(4.f)), IM_COL32(255, 255, 255, 235), badge);
	}

	// Name & who made it
	d->PushClipRect(ImVec2(min.x + S(10.f), picture_max.y), ImVec2(max.x - S(10.f), max.y), true);
	d->AddText(font_bold, S(14.f), ImVec2(min.x + S(12.f), picture_max.y + S(8.f)), C(col::text), mod.name.c_str());
	auto by = mod.author.empty() ? std::string() : "by " + mod.author;
	d->AddText(font_regular, S(12.f), ImVec2(min.x + S(12.f), picture_max.y + S(27.f)), C(col::text_dim), by.c_str());
	d->PopClipRect();

	d->AddRect(min, max, LerpColor(LerpColor(col::border, col::border_hover, hover), col::accent, active), rounding, 0, 1.f + 0.5f * active);

	ImGui::PopID();
	return pressed;
}

// The GameBanana mod picked on the browser page, in a column next to the cards: its picture, what it is, what the
// download got to, what can be done with it. DONE once it is downloaded or removed, CLOSE when closed, the panel goes
// away then
enum class ModPanelResult { NONE, DONE, CLOSE };

static int picked_mod = 0;     // On the browser page, 0 for none

static ModPanelResult ModPanel(const ModBrowser::Mod& mod) {
	auto job = ModBrowser::GetJob(mod.id);
	bool installed = ModBrowser::IsInstalled(mod.id);
	bool busy = ModBrowser::IsBusy();
	bool working = job.state == ModBrowser::Job::State::FETCHING || job.state == ModBrowser::Job::State::DOWNLOADING ||
		job.state == ModBrowser::Job::State::UNPACKING;

	static std::string remove_error;
	static int remove_error_mod = 0;

	// Downloaded: done with it
	if (job.state == ModBrowser::Job::State::DONE) {
		ModBrowser::ClearJob(mod.id);
		return ModPanelResult::DONE;
	}

	auto result = ModPanelResult::NONE;
	auto d = ImGui::GetWindowDrawList();
	auto start = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;
	float y = start.y;

	// The picture over the whole width
	float picture_h = width * 9.f / 16.f;
	ModPicture(d, mod.image, start, start + ImVec2(width, picture_h), 0.f, S(8.f), ImDrawFlags_RoundCornersAll);
	d->AddRect(start, start + ImVec2(width, picture_h), C(col::border), S(8.f));
	y += picture_h + S(12.f);

	// Name, who made it
	auto name_size = font_bold->CalcTextSizeA(S(17.f), FLT_MAX, width, mod.name.c_str());
	d->AddText(font_bold, S(17.f), ImVec2(start.x, y), C(col::text), mod.name.c_str(), nullptr, width);
	y += name_size.y + S(6.f);

	auto by = std::format("by {}", mod.author.empty() ? "?" : mod.author);
	auto stats = std::format("{} likes   {} views", mod.likes, mod.views);
	d->PushClipRect(ImVec2(start.x, y), ImVec2(start.x + width, y + S(40.f)), true);
	d->AddText(font_regular, S(12.f), ImVec2(start.x, y), C(col::text_dim), by.c_str());
	d->AddText(font_regular, S(12.f), ImVec2(start.x, y + S(17.f)), C(col::text_faint), stats.c_str());
	d->PopClipRect();
	y += S(40.f);

	std::string info;
	if (remove_error_mod == mod.id && !remove_error.empty()) {
		info = remove_error;
	}
	else if (installed) {
		std::string names;
		for (const auto& model : ModBrowser::ModelsOf(mod.id))
			names += (names.empty() ? "" : ", ") + model.substr(model.find_last_of('/') + 1);
		info = std::format("Installed: {}. Pick it in the agent list", names);
		if (!mod.own_hands)
			info += ". It has no first person hands of its own, the arms of your agent are shown with it";
	}
	else if (job.state == ModBrowser::Job::State::FAILED) {
		info = "Failed: " + job.message;
	}
	else if (!job.message.empty()) {
		info = job.message;
	}
	else if (mod.verdict == ModBrowser::Mod::Verdict::UNUSABLE) {
		info = "Can't be used: " + mod.note;
	}
	else if (mod.verdict == ModBrowser::Mod::Verdict::USABLE && !mod.own_hands) {
		info = "It has no first person hands of its own, the arms of your agent are shown with it";
	}
	else if (mod.verdict == ModBrowser::Mod::Verdict::UNCHECKED) {
		info = mod.note;
	}

	if (!info.empty()) {
		auto size = font_regular->CalcTextSizeA(S(13.f), FLT_MAX, width, info.c_str());
		d->AddText(font_regular, S(13.f), ImVec2(start.x, y), C(col::text_dim), info.c_str(), nullptr, width);
		y += size.y + S(12.f);
	}

	// Download progress
	if (job.state == ModBrowser::Job::State::DOWNLOADING || job.state == ModBrowser::Job::State::UNPACKING) {
		d->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + width, y + S(6.f)), C(col::track), S(3.f));
		d->AddRectFilled(ImVec2(start.x, y), ImVec2(start.x + width * std::clamp(job.progress, 0.f, 1.f), y + S(6.f)), C(col::accent), S(3.f));
		y += S(16.f);
	}

	// Buttons over the whole width, one under the other
	auto button = ImVec2(width, S(32.f));
	auto pos = ImVec2(start.x, y);
	auto next = [&]() { pos.y += button.y + S(6.f); };

	if (working) {
		if (PillButton("Cancel##mod_cancel", pos, button))
			ModBrowser::Cancel();
		next();
	}
	else if (job.state == ModBrowser::Job::State::CHOOSE && !installed) {
		// One button per file of the mod
		for (const auto& file : job.files) {
			auto label = std::format("{} ({:.1f} MB)##mod_file_{}", file.name, file.size / 1048576.0, file.id);
			ImGui::BeginDisabled(busy);
			if (PillButton(label.c_str(), pos, button, false, file.description.empty() ? nullptr : file.description.c_str()))
				ModBrowser::Install(mod, file.id);
			ImGui::EndDisabled();
			next();
		}
	}
	else if (!installed) {
		ImGui::BeginDisabled(busy);
		if (PillButton("Download##mod_download", pos, button, true, busy ? "Another model is downloading" : "Downloads it and puts it in the game folder"))
			ModBrowser::Install(mod);
		ImGui::EndDisabled();
		next();
	}
	else {
		if (PillButton("Remove##mod_remove", pos, button, false, "Deletes the files it put in the game folder")) {
			std::vector<std::string> removed;
			remove_error.clear();
			remove_error_mod = mod.id;
			if (ModBrowser::Remove(mod.id, removed, remove_error))
				result = ModPanelResult::DONE;

			// Not picked anymore once its files are gone
			for (auto& loadout : cfg::skins::loadouts)
				if (std::find(removed.begin(), removed.end(), loadout.custom_model) != removed.end())
					loadout.custom_model.clear();
		}
		next();
	}

	if (!mod.url.empty()) {
		if (PillButton("Open Page##mod_page", pos, button, false, mod.url.c_str())) {
			std::wstring url(mod.url.begin(), mod.url.end());
			ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		next();
	}

	if (PillButton("Close##mod_close", pos, button))
		result = ModPanelResult::CLOSE;
	next();

	ImGui::SetCursorScreenPos(start);
	ImGui::Dummy(ImVec2(width, pos.y - start.y));
	return result;
}

static bool HasPickedMod() {
	return picked_mod != 0;
}

// The window next to the menu on the browser page, like the previews: the picked mod
static void ModBrowserPanel() {
	auto mods = ModBrowser::List();
	auto mod = std::find_if(mods.begin(), mods.end(), [](const ModBrowser::Mod& m) { return m.id == picked_mod; });
	if (mod == mods.end())
		return;

	BeginPanel("MODEL");
	if (ModPanel(*mod) != ModPanelResult::NONE)
		picked_mod = 0;     // Downloaded, removed or closed: nothing picked anymore
	EndPanel();
}

// "agents/models/x/naruto.vmdl" -> "naruto"
static std::string CustomModelName(const std::string& resource) {
	auto name = resource.substr(resource.find_last_of('/') + 1);
	return name.ends_with(".vmdl") ? name.substr(0, name.size() - 5) : name;
}

// The knife under the mouse on the knives page, shown in the window next to the menu
static int knife_hovered = 0;

// The picture of what is picked over the whole width of the window next to the menu, the glow of its rarity behind
static void SkinPreview(const std::string& image, const char* icon, ImU32 rarity) {
	auto d = ImGui::GetWindowDrawList();
	auto pos = ImGui::GetCursorScreenPos();
	auto size = ImVec2(ImGui::GetContentRegionAvail().x, S(210.f));
	auto center = pos + size * 0.5f;

	d->AddRectFilled(pos, pos + size, C(col::window), S(8.f));
	d->AddCircleFilled(center, S(90.f), C(rarity, 0.12f), 48);

	auto texture = image.empty() ? ImTextureID{} : ImageCache::Get(image);
	if (texture) {
		float fit_w = std::min(size.x - S(20.f), (size.y - S(20.f)) * 4.f / 3.f);
		auto fit = ImVec2(fit_w, fit_w * 3.f / 4.f);
		d->AddImage(texture, center - fit * 0.5f, center + fit * 0.5f, ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE));
	}
	else if (icon) {
		auto extent = font_regular->CalcTextSizeA(S(64.f), FLT_MAX, 0.f, icon);
		d->AddText(font_regular, S(64.f), center - extent * 0.5f, C(col::text_faint), icon);
	}

	ImGui::Dummy(size);
}

// Name & a second line under the picture, a stripe of the rarity when it has one
static void SkinCaption(const std::string& title, const std::string& subtitle, std::optional<ImU32> rarity = std::nullopt) {
	ImGui::Dummy(ImVec2(0.f, S(4.f)));
	auto d = ImGui::GetWindowDrawList();
	auto at = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;

	d->PushClipRect(at, at + ImVec2(width, S(46.f)), true);
	d->AddText(font_bold, S(15.f), at, C(col::text), title.c_str());
	d->AddText(font_regular, S(12.f), at + ImVec2(0.f, S(20.f)), C(col::text_dim), subtitle.c_str());
	d->PopClipRect();
	if (rarity)
		d->AddRectFilled(at + ImVec2(0.f, S(38.f)), at + ImVec2(S(40.f), S(40.f)), C(*rarity), 1.f);

	ImGui::Dummy(ImVec2(0.f, S(46.f)));
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
	auto local = Cache::Current()->local;
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
			ShowTooltip("%s", available
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
	ImGui::BeginChild("##skins_page", ImVec2(region.x, region.y - toolbar_height), ImGuiChildFlags_None,
		ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	// When a change shows up in the game
	{
		constexpr auto note = "Changes show after the next round or a respawn. Set in the lobby, they are on from the start of the match";
		auto at = ImGui::GetCursorScreenPos();
		auto d = ImGui::GetWindowDrawList();
		const float line = S(16.f);

		auto center = at + ImVec2(S(7.f), line * 0.5f);
		d->AddCircle(center, S(6.f), C(col::text_faint), 16, 1.2f);
		auto mark = font_bold->CalcTextSizeA(S(10.f), FLT_MAX, 0.f, "i");
		d->AddText(font_bold, S(10.f), center - mark * 0.5f, C(col::text_dim), "i");

		auto text = font_regular->CalcTextSizeA(S(13.f), FLT_MAX, 0.f, note);
		d->AddText(font_regular, S(13.f), at + ImVec2(S(20.f), (line - text.y) * 0.5f), C(col::text_dim), note);

		ImGui::Dummy(ImVec2(0.f, line + S(8.f)));
	}

	std::optional<SkinPage> go_to;

	switch (skin_page) {
	case SkinPage::ITEMS: {
		BeginScrollArea("##items_scroll");
		if (!loaded)
			TextBlock(Skins::HasFailed()
				? "Could not download the skin list. Hold a weapon in game and set its paint kit id by hand"
				: "Downloading skin list...");

		// Agent, gloves, knife & music kit in one row, like the loadout of the game
		SectionTitle("CHARACTER");
		{
			auto grid = BeginGrid();
			NextCard(grid);

			// A custom model is the agent of the team
			auto custom_name = loadout.custom_model.empty() ? std::string() : CustomModelName(loadout.custom_model);
			auto agent = custom_name.empty() && loadout.agent ? Skins::FindAgent(loadout.agent) : nullptr;
			if (ItemCard("##agent", grid.card,
				agent ? SmallImage(agent->image) : "", nullptr,
				!custom_name.empty() ? custom_name.c_str() : agent ? agent->name.c_str() : "Agent",
				!custom_name.empty() ? "Custom" : agent ? agent->group.c_str() : "Default",
				!custom_name.empty() ? col::accent : agent ? RarityColor(agent->rarity_color) : col::border_hover,
				false, false, !agent))
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
			if (ItemCard("##in_hand", grid.card, BaseImage(in_hand), Weapon::IconFor(in_hand), label.c_str(), "", col::border_hover, false, true)) {
				go_to = SkinPage::SKINS;
				selected_item = in_hand;
			}

			EndGrid(grid);
		}

		// Weapons of the team, one category at a time so it barely scrolls
		static int category = 0;
		auto in_category = [&](const ItemInfo& item) {
			switch (category) {
			case 0:		return item.category == "Pistols";
			case 1:		return item.category == "SMGs" || item.category == "Heavy";
			default:	return item.category == "Rifles";
			}
		};

		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		auto tabs = ImGui::GetCursorScreenPos();
		TabSwitch("##category", &category, { "Pistols", "Mid-Tier", "Rifles" }, tabs);
		ImGui::SetCursorScreenPos(tabs + ImVec2(0.f, S(32.f) + S(12.f)));

		auto grid = BeginGrid();
		for (const auto& item : all_items) {
			if (!(skin_team == cfg::skins::TERRORIST ? item.terrorist : item.counter_terrorist) || !in_category(item))
				continue;

			NextCard(grid);

			auto skin = assigned_skin(item);
			auto id = std::to_string(item.definition_index);

			if (ItemCard(id.c_str(), grid.card,
				skin ? SmallImage(skin->image) : BaseImage(item.definition_index), Weapon::IconFor(item.definition_index),
				item.name.c_str(),
				skin ? skin->name.c_str() : "Default",
				skin ? RarityColor(skin->rarity_color) : col::border_hover,
				false, item.definition_index == in_hand)) {
				go_to = SkinPage::SKINS;
				selected_item = item.definition_index;
			}
		}
		EndGrid(grid);
		EndScrollArea();

		break;
	}

	case SkinPage::GLOVES: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		// The one equipped & the third person switch in the window next to the menu
		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		BeginScrollArea("##gloves_scroll");

		auto grid = BeginFixedGrid(4);
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
		EndScrollArea();
		break;
	}

	case SkinPage::KNIVES: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		// The knife under the mouse (else the one equipped) big in the window next to the menu
		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		SearchBox("##knife_search", skin_search, sizeof(skin_search));
		BeginScrollArea("##knives_scroll");

		int hovered_now = 0;

		auto grid = BeginFixedGrid(4);
		if (!*skin_search) {
			NextCard(grid);
			if (ItemCard("##default", grid.card, "", Weapon::IconFor(skin_team == cfg::skins::TERRORIST ? 59 : 42), "Default", "From the game", col::border_hover, loadout.knife == 0))
				loadout.knife = 0;
		}

		for (const auto& item : all_items) {
			if (item.category != "Knives" || !ContainsInsensitive(item.name, skin_search))
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

			if (ImGui::IsItemHovered())
				hovered_now = item.definition_index;
		}

		knife_hovered = hovered_now;

		EndGrid(grid);
		EndScrollArea();
		break;
	}

	case SkinPage::AGENTS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		// One pick for the team: the game's model, a custom model or an agent. The one picked & the tools of the
		// custom models in the window next to the menu
		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		SearchBox("##agent_search", skin_search, sizeof(skin_search));
		BeginScrollArea("##agents_scroll");

		auto models = CustomModels::IsAvailable() ? CustomModels::List() : std::vector<CustomModel>{};
		bool custom = !loadout.custom_model.empty();
		auto grid = BeginFixedGrid(4);
		if (!*skin_search) {
			NextCard(grid);
			if (ItemCard("##default", grid.card, "", nullptr, "Default", "From the game", col::border_hover, !custom && loadout.agent == 0, false, true)) {
				loadout.agent = 0;
				loadout.custom_model.clear();
			}
		}

		for (const auto& model : models) {
			if (!ContainsInsensitive(model.name, skin_search) && !ContainsInsensitive(std::string("Custom"), skin_search))
				continue;

			NextCard(grid);

			bool bad = model.status == CustomModel::Status::BAD;
			bool warning = model.status == CustomModel::Status::WARNING;
			auto id = "custom_" + model.resource;

			ImGui::BeginDisabled(bad);
			if (ItemCard(id.c_str(), grid.card, "", nullptr, model.name.c_str(),
				bad ? "Custom, can't be used" : warning ? "Custom, check" : "Custom",
				bad ? col::border_hover : warning ? IM_COL32(230, 160, 60, 255) : col::accent,
				loadout.custom_model == model.resource, false, true)) {
				loadout.custom_model = model.resource;
				loadout.agent = 0;
			}
			ImGui::EndDisabled();
		}

		bool terrorist = skin_team == cfg::skins::TERRORIST;

		for (const auto& agent : Skins::GetAgents()) {
			if (agent.terrorist != terrorist)
				continue;

			if (!ContainsInsensitive(agent.name, skin_search) && !ContainsInsensitive(agent.group, skin_search))
				continue;

			NextCard(grid);

			auto id = std::to_string(agent.definition_index);
			if (ItemCard(id.c_str(), grid.card, SmallImage(agent.image), nullptr, agent.name.c_str(), agent.group.c_str(),
				RarityColor(agent.rarity_color), !custom && loadout.agent == agent.definition_index)) {
				loadout.agent = agent.definition_index;
				loadout.custom_model.clear();
			}
		}

		EndGrid(grid);
		EndScrollArea();
		break;
	}

	case SkinPage::MODEL_BROWSER: {
		if (BackButton("Agents", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::AGENTS;

		ImGui::Dummy(ImVec2(0.f, S(4.f)));

		auto mods = ModBrowser::List();
		auto state = ModBrowser::GetListState();

		if (state == ModBrowser::ListState::LOADING && mods.empty()) {
			TextBlock("Loading the list...");
			break;
		}

		if (state == ModBrowser::ListState::FAILED) {
			TextBlock("Could not get the list from GameBanana");
			if (PillButton("Try Again##mods_retry", ImGui::GetCursorScreenPos(), ImVec2(S(110.f), S(28.f))))
				ModBrowser::Reload();
			break;
		}

		// Shown: the usable ones, the ones checked on download & what was downloaded (to remove it)
		static int mod_filter = 0;
		std::vector<char> installed_flags;
		int installed_count = 0, shown_count = 0;
		for (const auto& mod : mods) {
			installed_flags.push_back(ModBrowser::IsInstalled(mod.id));
			installed_count += installed_flags.back();
			shown_count += installed_flags.back() || mod.verdict == ModBrowser::Mod::Verdict::USABLE ||
				mod.verdict == ModBrowser::Mod::Verdict::UNCHECKED;
		}

		auto all_label = std::format("All ({})", shown_count);
		auto installed_label = std::format("Downloaded ({})", installed_count);
		auto missing_label = std::format("Not Downloaded ({})", shown_count - installed_count);
		DropdownRow("Show", &mod_filter, { all_label.c_str(), installed_label.c_str(), missing_label.c_str() });

		// The first check of the list (later ones come from the cache at once)
		{
			int checked = 0, total = 0, usable = 0;
			ModBrowser::CheckProgress(checked, total, usable);
			if (total > 0 && checked < total) {
				float done = static_cast<float>(checked) / total;
				auto label = std::format("Checking the models {:.0f}%", done * 100.f);

				ImGui::Dummy(ImVec2(0.f, S(4.f)));
				auto at = ImGui::GetCursorScreenPos();
				float width = ImGui::GetContentRegionAvail().x;
				auto d = ImGui::GetWindowDrawList();
				d->AddText(at, C(col::text_dim), label.c_str());
				at.y += ImGui::GetTextLineHeight() + S(4.f);
				d->AddRectFilled(at, at + ImVec2(width, S(6.f)), C(col::track), S(3.f));
				d->AddRectFilled(at, at + ImVec2(width * done, S(6.f)), C(col::accent), S(3.f));
				ImGui::Dummy(ImVec2(0.f, ImGui::GetTextLineHeight() + S(14.f)));
			}
		}

		SearchBox("##mod_search", skin_search, sizeof(skin_search));
		BeginScrollArea("##mods_scroll");

		// Large cards, the picture is what tells them apart
		auto grid = BeginFixedGrid(3);
		grid.card.y = grid.card.x * 9.f / 16.f + S(50.f);

		for (size_t index = 0; index < mods.size(); index++) {
			const auto& mod = mods[index];
			bool installed = installed_flags[index];

			if ((mod_filter == 1 && !installed) || (mod_filter == 2 && installed))
				continue;

			// Downloaded ones always show, to remove them. Ones only checked once downloaded show too
			bool usable = mod.verdict == ModBrowser::Mod::Verdict::USABLE;
			bool unchecked = mod.verdict == ModBrowser::Mod::Verdict::UNCHECKED;
			if (!usable && !unchecked && !installed)
				continue;
			if (!ContainsInsensitive(mod.name, skin_search) && !ContainsInsensitive(mod.author, skin_search))
				continue;

			NextCard(grid);

			// A download that finished while another one was picked: nothing more to show of it
			auto job = ModBrowser::GetJob(mod.id);
			if (job.state == ModBrowser::Job::State::DONE && mod.id != picked_mod)
				ModBrowser::ClearJob(mod.id);

			std::string badge;
			ImU32 badge_color = C(col::text_dim);
			float progress = -1.f;

			switch (job.state) {
			case ModBrowser::Job::State::FETCHING:    badge = "Getting files"; badge_color = C(col::accent); break;
			case ModBrowser::Job::State::DOWNLOADING: badge = std::format("Downloading {:.0f}%", job.progress * 100.f); badge_color = C(col::accent); progress = job.progress; break;
			case ModBrowser::Job::State::UNPACKING:   badge = "Unpacking"; badge_color = C(col::accent); progress = 1.f; break;
			case ModBrowser::Job::State::CHOOSE:      badge = "Pick a file"; badge_color = C(col::accent); break;
			case ModBrowser::Job::State::FAILED:      badge = "Failed"; badge_color = IM_COL32(230, 80, 80, 255); break;
			default:
				if (installed) {
					badge = mod.own_hands ? "Installed" : "Installed, no own hands";
					badge_color = C(col::online);
				}
				else if (mod.verdict == ModBrowser::Mod::Verdict::USABLE && !mod.own_hands) {
					badge = "No own hands";
					badge_color = IM_COL32(230, 160, 60, 255);
				}
				else if (mod.verdict == ModBrowser::Mod::Verdict::PENDING) {
					badge = "Checking";
				}
				else if (mod.verdict == ModBrowser::Mod::Verdict::UNUSABLE) {
					badge = "Can't be used";
					badge_color = IM_COL32(230, 80, 80, 255);
				}
				else if (unchecked) {
					badge = "Checked on download";
					badge_color = IM_COL32(230, 160, 60, 255);
				}
				break;
			}

			// Picking the picked one again closes it
			auto id = "mod_" + std::to_string(mod.id);
			if (ModCard(id.c_str(), grid.card, mod, badge.c_str(), badge_color, progress, picked_mod == mod.id))
				picked_mod = picked_mod == mod.id ? 0 : mod.id;
		}

		EndGrid(grid);
		EndScrollArea();
		break;
	}

	case SkinPage::MUSIC_KITS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		// The one picked & what it is for in the window next to the menu
		ImGui::Dummy(ImVec2(0.f, S(4.f)));
		SearchBox("##music_search", skin_search, sizeof(skin_search));
		BeginScrollArea("##music_scroll");

		auto grid = BeginFixedGrid(4);
		if (!*skin_search) {
			NextCard(grid);
			if (ItemCard("##default", grid.card, "", nullptr, "Default", "From the game", col::border_hover, cfg::skins::music_kit == 0))
				cfg::skins::music_kit = 0;
		}

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
		EndScrollArea();
		break;
	}

	case SkinPage::PRESETS: {
		if (BackButton("Loadout", ImGui::GetCursorScreenPos()))
			go_to = SkinPage::ITEMS;

		ImGui::Dummy(ImVec2(0.f, S(10.f)));
		BeginScrollArea("##presets_scroll");

		// Half the width, the list does not need more
		ImGui::BeginChild("##skin_presets", ImVec2(std::min(ImGui::GetContentRegionAvail().x, S(460.f)), 0.f),
			ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
		BeginPanel("SKIN LOADOUTS");
		TextBlock("Both teams, gloves, knives, agents & the music kit. Configs are exported apart, in Settings");
		PresetList(Config::Preset::SKINS, skin_presets);
		EndPanel();
		ImGui::EndChild();

		EndScrollArea();
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

		// The search stays on top, only the cards scroll under it
		if (info) {
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() - S(4.f));
			SearchBox("##skin_search", skin_search, sizeof(skin_search));
		}

		// Skins, their wear & seed in the window next to the menu
		BeginScrollArea("##skin_grid");
		{
			bool equipped = is_glove ? loadout.glove == selected_item : is_knife ? loadout.knife == selected_item : true;

			auto grid = BeginFixedGrid(4);
			NextCard(grid);

			// Knives: the plain knife, still equipped
			auto default_image = is_knife && info && !info->image.empty() ? SmallImage(info->image) : BaseImage(selected_item);
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
		EndScrollArea();

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

// The window next to the menu on the skins pages: what is picked there big, with what goes with it (the wear, seed &
// paint kit of a skin, the third person switch of gloves, the tools of the custom models...)
void Menu::RenderSkinPanel() {
	if (skin_page == SkinPage::MODEL_BROWSER) {
		ModBrowserPanel();
		return;
	}

	std::optional<SkinPage> go_to;
	{
		std::lock_guard<std::mutex> lock(cfg::skins::mutex);
		auto& loadout = cfg::skins::loadouts[skin_team];
		auto assigned_skin = [&](const ItemInfo& item) -> const SkinInfo* {
			auto it = loadout.items.find(item.definition_index);
			return it != loadout.items.end() && it->second.paint_kit ? item.FindSkin(it->second.paint_kit) : nullptr;
		};
		const int default_knife = skin_team == cfg::skins::TERRORIST ? 59 : 42;

		switch (skin_page) {
		case SkinPage::GLOVES: {
			auto glove = loadout.glove ? Skins::FindItem(loadout.glove) : nullptr;
			auto skin = glove ? assigned_skin(*glove) : nullptr;

			BeginPanel("GLOVES");
			SkinPreview(skin ? SmallImage(skin->image) : "", nullptr, skin ? RarityColor(skin->rarity_color) : col::border_hover);
			if (skin)
				SkinCaption(glove->name, skin->name, RarityColor(skin->rarity_color));
			else
				SkinCaption(glove ? glove->name : "Default", glove ? "No skin picked" : "From the player model");
			Toggle("Hide In Third Person", &cfg::skins::glove_hide_third_person,
				"Keeps the new gloves in first person only\nFor player models with gloves built in, they would show both in third person");
			EndPanel();
			break;
		}

		case SkinPage::KNIVES: {
			int shown_index = knife_hovered ? knife_hovered : loadout.knife;
			auto shown = shown_index ? Skins::FindItem(shown_index) : nullptr;
			auto skin = shown && shown_index == loadout.knife ? assigned_skin(*shown) : nullptr;
			auto image = skin ? skin->image : (shown ? shown->image : "");

			BeginPanel("KNIFE");
			SkinPreview(image.empty() ? "" : SmallImage(image), Weapon::IconFor(shown_index ? shown_index : default_knife),
				skin ? RarityColor(skin->rarity_color) : col::border_hover);
			auto name = shown ? shown->name : std::string("Default knife");
			if (skin)
				name += " | " + skin->name;
			SkinCaption(name, knife_hovered && knife_hovered != loadout.knife ? "Click to equip" : "Equipped");
			EndPanel();
			break;
		}

		case SkinPage::AGENTS: {
			bool custom = !loadout.custom_model.empty();
			auto agent = !custom && loadout.agent ? Skins::FindAgent(loadout.agent) : nullptr;

			BeginPanel("AGENT");
			SkinPreview(agent ? SmallImage(agent->image) : "", nullptr,
				custom ? col::accent : agent ? RarityColor(agent->rarity_color) : col::border_hover);
			if (custom)
				SkinCaption(CustomModelName(loadout.custom_model), "Custom model", col::accent);
			else if (agent)
				SkinCaption(agent->name, agent->group, RarityColor(agent->rarity_color));
			else
				SkinCaption("Default", "From the game");
			EndPanel();

			if (CustomModels::IsAvailable()) {
				auto models = CustomModels::List();
				BeginPanel("CUSTOM MODELS");
				if (CustomModelTools(loadout, models))
					go_to = SkinPage::MODEL_BROWSER;
				EndPanel();
			}
			break;
		}

		case SkinPage::MUSIC_KITS: {
			auto kit = cfg::skins::music_kit ? Skins::FindMusicKit(cfg::skins::music_kit) : nullptr;

			BeginPanel("MUSIC KIT");
			SkinPreview(kit ? kit->image : "", nullptr, kit ? RarityColor(kit->rarity_color) : col::border_hover);
			if (kit) {
				// "Artist, Name" on two lines
				auto comma = kit->name.find(", ");
				auto title = comma != std::string::npos ? kit->name.substr(comma + 2) : kit->name;
				auto artist = comma != std::string::npos ? kit->name.substr(0, comma) : std::string();
				SkinCaption(title, artist, RarityColor(kit->rarity_color));
			}
			else
				SkinCaption(cfg::skins::music_kit ? "Custom" : "Default", "From the game");
			TextBlock("For both teams, plays in the menu, at the start & end of rounds and as your MVP anthem");
			EndPanel();
			break;
		}

		case SkinPage::SKINS:
			SkinPanel();
			break;

		default:
			break;
		}
	}

	if (go_to)
		OpenSkinPage(*go_to);
}

// The picked skin of the item of the skins page big, its wear, seed & paint kit
void Menu::SkinPanel() {
	auto& loadout = cfg::skins::loadouts[skin_team];      // Locked by RenderSkinPanel
	auto info = Skins::FindItem(selected_item);
	bool is_glove = Skins::IsGlove(selected_item);
	bool is_knife = Skins::IsKnife(selected_item);
	auto& entry = loadout.items[selected_item];
	auto skin = info ? info->FindSkin(entry.paint_kit) : nullptr;

	BeginPanel("SKIN");
	{
		auto base_image = is_knife && info && !info->image.empty() ? SmallImage(info->image) : BaseImage(selected_item);
		SkinPreview(skin ? SmallImage(skin->image) : base_image, is_glove ? nullptr : Weapon::IconFor(selected_item),
			skin ? RarityColor(skin->rarity_color) : col::border_hover);

		auto item_name = info ? info->name : std::format("Item #{}", selected_item);
		if (skin)
			SkinCaption(skin->name, item_name, RarityColor(skin->rarity_color));
		else
			SkinCaption(is_knife ? "Vanilla" : "Default", item_name);

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

	// Dont keep empty entries around in the config
	if (!entry.paint_kit) {
		loadout.items.erase(selected_item);
		if (is_glove && loadout.glove == selected_item)
			loadout.glove = 0;
	}
}

void Menu::RenderMovementTab() {
	constexpr auto unavailable = "Only available when the game is launched with -insecure";
	const bool available = Movement::IsAvailable();

	BeginColumn(0);
	{
		BeginPanel("JUMP");
		ImGui::BeginDisabled(!available);
		Toggle("Bunny Hop", &cfg::misc::bhop, available ? nullptr : unavailable);
		// Subtick needs the input layout of this game build
		ImGui::BeginDisabled(!Subtick::IsAvailable());
		ToggleBind("Air Strafe", &cfg::misc::auto_strafe, &cfg::misc::air_strafe_mode, &cfg::misc::air_strafe_key, Subtick::IsAvailable()
			? nullptr
			: "Only with -insecure, on the game build it was made for");

		ToggleBind("Jump Bug", &cfg::misc::jump_bug, &cfg::misc::jump_bug_mode, &cfg::misc::jump_bug_key, Subtick::IsAvailable()
			? "Jumps right before touching the ground: no fall damage & a higher jump. Hold SPACE with it while falling"
			: "Only with -insecure, on the game build it was made for");
		ImGui::EndDisabled();

		// Rows the keybind list leaves out
		MultiDropdownRow("Hide In Keybinds", { "Bunny Hop", "Air Strafe", "Jump Bug" },
			{ &cfg::world::keybinds::hide_bhop, &cfg::world::keybinds::hide_air_strafe, &cfg::world::keybinds::hide_jump_bug },
			"Left out of the keybind list, they keep working", S(120.f));
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

// The people of the match, as the scoreboard knows them. Read twice a second, the menu is drawn every frame
struct MatchPlayer {
	uint64_t steam_id = 0;
	std::string name;
	int index = 0;
	int team = 0;
	bool bot = false;
	bool local = false;

	int rank_type = 0;
	int ranking = 0;
	int wins = 0;
};

static const std::vector<MatchPlayer>& MatchPlayers() {
	static std::vector<MatchPlayer> players;
	static std::chrono::steady_clock::time_point next{};

	auto now = std::chrono::steady_clock::now();
	if (now < next)
		return players;
	next = now + std::chrono::milliseconds(500);

	players.clear();
	auto p = Engine::GetProcess();
	auto snapshot = Cache::Current();
	if (!p || !snapshot->globals.in_match)
		return players;

	for (const auto& player : snapshot->players) {
		auto controller = player.GetControllerAddress();
		if (!controller || player.index < 0)
			continue;

		MatchPlayer entry;
		entry.steam_id = player.steam_id;
		entry.index = player.index;
		entry.team = player.team;
		entry.bot = player.bot;
		entry.local = player.localplayer;

		// The whole name, the cache keeps 32 bytes of it
		char name[128]{};
		p->read_raw(controller + offsets::controller::m_iszPlayerName, name, sizeof(name) - 1);
		entry.name = std::string(name, strnlen(name, sizeof(name)));

		entry.ranking = p->read<int32_t>(controller + offsets::controller::m_iCompetitiveRanking);
		entry.wins = p->read<int32_t>(controller + offsets::controller::m_iCompetitiveWins);
		entry.rank_type = p->read<int8_t>(controller + offsets::controller::m_iCompetitiveRankType);

		players.push_back(std::move(entry));
	}

	// Teams together, us first in ours
	std::sort(players.begin(), players.end(), [](const MatchPlayer& a, const MatchPlayer& b) {
		if (a.team != b.team)
			return a.team > b.team;
		if (a.local != b.local)
			return a.local;
		return a.index < b.index;
	});
	return players;
}

// What the scoreboard shows: the Premier rating in the color of its tier, else the skill group
static std::pair<std::string, ImU32> RankText(const MatchPlayer& player) {
	static const char* groups[] = {
		"Silver I", "Silver II", "Silver III", "Silver IV", "Silver Elite", "Silver Elite Master",
		"Gold Nova I", "Gold Nova II", "Gold Nova III", "Gold Nova Master",
		"Master Guardian I", "Master Guardian II", "Master Guardian Elite", "Distinguished Master Guardian",
		"Legendary Eagle", "Legendary Eagle Master", "Supreme Master First Class", "Global Elite",
	};

	if (player.bot)
		return { "Bot", col::text_dim };
	if (player.ranking <= 0)
		return { "Unranked", col::text_dim };

	if (player.rank_type == 11) {
		int rating = player.ranking;
		ImU32 color = rating >= 30000 ? IM_COL32(254, 215, 0, 255)
			: rating >= 25000 ? IM_COL32(235, 75, 75, 255)
			: rating >= 20000 ? IM_COL32(211, 44, 230, 255)
			: rating >= 15000 ? IM_COL32(136, 71, 255, 255)
			: rating >= 10000 ? IM_COL32(75, 105, 255, 255)
			: rating >= 5000 ? IM_COL32(94, 152, 217, 255)
			: IM_COL32(176, 195, 217, 255);
		return { std::format("{},{:03}", rating / 1000, rating % 1000), color };
	}

	if (player.ranking >= 1 && player.ranking <= 18) {
		auto text = std::string(groups[player.ranking - 1]);
		if (player.rank_type == 7)
			text += " (Wingman)";
		return { text, col::text };
	}

	return { std::to_string(player.ranking), col::text };
}

static ImU32 TeamColor(int team) {
	return team == 2 ? IM_COL32(234, 190, 84, 255) : team == 3 ? IM_COL32(120, 160, 230, 255) : col::text_dim;
}

// Their picture in a circle with a ring of their side, the first letter of the name without one
static void DrawAvatar(ImDrawList* d, const MatchPlayer& player, ImVec2 center, float size) {
	if (auto texture = player.bot ? ImTextureID{} : Avatars::Get(player.steam_id))
		d->AddImageRounded(texture, center - ImVec2(size, size) * 0.5f, center + ImVec2(size, size) * 0.5f,
			ImVec2(0, 0), ImVec2(1, 1), C(IM_COL32_WHITE), size * 0.5f);
	else {
		d->AddCircleFilled(center, size * 0.5f, C(col::track), 32);
		char letter[2] = { player.name.empty() ? '?' : static_cast<char>(toupper(static_cast<unsigned char>(player.name[0]))), 0 };
		auto letter_size = font_bold->CalcTextSizeA(size * 0.45f, FLT_MAX, 0.f, letter);
		d->AddText(font_bold, size * 0.45f, center - letter_size * 0.5f, C(col::text_dim), letter);
	}
	d->AddCircle(center, size * 0.5f + S(1.5f), C(TeamColor(player.team)), 32, S(1.5f));
}

// A name cut to fit a buffer, not in the middle of a letter
static void CopyName(char* buffer, size_t size, const std::string& name) {
	size_t length = std::min(name.size(), size - 1);
	while (length > 0 && length < name.size() && (static_cast<unsigned char>(name[length]) & 0xC0) == 0x80)
		length--;

	std::memcpy(buffer, name.data(), length);
	buffer[length] = 0;
}

static int profile_selected = -1;	// Entity index of the player shown, -1 for ourselves

static const MatchPlayer* SelectedPlayer(const std::vector<MatchPlayer>& players) {
	for (const auto& player : players)
		if (profile_selected >= 0 ? player.index == profile_selected : player.local)
			return &player;
	return players.empty() ? nullptr : &players.front();
}

// Everyone in the match, by side: picture, name & rank. Picking one shows their profile next to it
static void MatchPlayersPanel() {
	BeginPanel("PLAYERS");

	const auto& players = MatchPlayers();
	if (players.empty())
		TextBlock("Join a match to see the players in it");

	auto selected = SelectedPlayer(players);
	auto d = ImGui::GetWindowDrawList();
	const float row_height = S(34.f);
	const float avatar = S(24.f);

	int last_team = -1;
	for (const auto& player : players) {
		ImGui::PushID(player.index);

		// The side, above its players
		if (player.team != last_team) {
			last_team = player.team;
			SectionTitle(player.team == 2 ? "TERRORISTS" : player.team == 3 ? "COUNTER-TERRORISTS" : "SPECTATORS");
		}

		auto pos = ImGui::GetCursorScreenPos();
		auto size = ImVec2(ImGui::GetContentRegionAvail().x, row_height);
		bool is_selected = selected == &player;

		if (ImGui::InvisibleButton("##player", size))
			profile_selected = player.local ? -1 : player.index;

		float hover = Animate(ImGui::GetItemID(), ImGui::IsItemHovered() && !is_selected ? 1.f : 0.f, 16.f);
		float active = Animate(ImGui::GetItemID() + 1, is_selected ? 1.f : 0.f, 14.f);

		auto row_min = pos - ImVec2(S(6.f), 0.f), row_max = pos + size + ImVec2(S(6.f), 0.f);
		if (active > 0.01f) {
			d->AddRectFilled(row_min, row_max, C(col::selected, active), S(6.f));
			d->AddRectFilled(ImVec2(row_min.x, row_min.y + S(10.f)), ImVec2(row_min.x + S(2.f), row_max.y - S(10.f)), C(col::accent, active), 1.f);
		}
		if (hover > 0.01f)
			d->AddRectFilled(row_min, row_max, C(col::hover, hover), S(6.f));

		DrawAvatar(d, player, ImVec2(pos.x + S(4.f) + avatar * 0.5f, pos.y + row_height * 0.5f), avatar);

		// Rank at the right, the name up to it
		auto [rank, rank_color] = RankText(player);
		auto rank_size = ImGui::CalcTextSize(rank.c_str());
		float text_y = pos.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;
		d->AddText(ImVec2(pos.x + size.x - rank_size.x - S(4.f), text_y), C(rank_color), rank.c_str());

		float text_x = pos.x + S(4.f) + avatar + S(10.f);
		d->PushClipRect(ImVec2(text_x, pos.y), ImVec2(pos.x + size.x - rank_size.x - S(14.f), pos.y + row_height), true);
		auto name = player.name.empty() ? std::string("?") : player.name;
		d->AddText(ImVec2(text_x, text_y), LerpColor(col::text_label, col::text, std::max(std::max(hover, active), player.local ? 1.f : 0.f)), name.c_str());
		if (player.local)
			d->AddText(ImVec2(text_x + ImGui::CalcTextSize(name.c_str()).x + S(6.f), text_y), C(col::accent, 0.8f), "(you)");
		d->PopClipRect();

		ImGui::PopID();
	}

	EndPanel();
}

// Label at the left, value at the right, a thin line under it
static void InfoLine(const char* label, const std::string& value, ImU32 color = col::text) {
	if (value.empty())
		return;

	auto d = ImGui::GetWindowDrawList();
	auto pos = ImGui::GetCursorScreenPos();
	float width = ImGui::GetContentRegionAvail().x;
	float height = S(26.f);
	float text_y = pos.y + (height - ImGui::GetTextLineHeight()) * 0.5f;

	d->AddText(ImVec2(pos.x, text_y), C(col::text_dim), label);

	auto label_width = ImGui::CalcTextSize(label).x;
	auto value_size = ImGui::CalcTextSize(value.c_str());
	float value_x = std::max(pos.x + label_width + S(16.f), pos.x + width - value_size.x);
	d->PushClipRect(ImVec2(pos.x + label_width + S(16.f), pos.y), ImVec2(pos.x + width, pos.y + height), true);
	d->AddText(ImVec2(value_x, text_y), C(color), value.c_str());
	d->PopClipRect();

	d->AddLine(ImVec2(pos.x, pos.y + height - 0.5f), ImVec2(pos.x + width, pos.y + height - 0.5f), C(col::border));
	ImGui::Dummy(ImVec2(width, height));
}

// The player picked in the list, in the panel next to the menu: their rank & their Steam profile
static void ProfilePanel() {
	BeginPanel("PROFILE");

	const auto& players = MatchPlayers();
	auto player = SelectedPlayer(players);
	if (!player) {
		TextBlock("Join a match, then pick a player to see their profile");
		EndPanel();
		return;
	}

	auto d = ImGui::GetWindowDrawList();
	auto profile = player->bot ? SteamProfile{} : Avatars::GetProfile(player->steam_id);
	float width = ImGui::GetContentRegionAvail().x;

	// Header: picture, name, what Steam says they do. Their rank at the right
	{
		auto pos = ImGui::GetCursorScreenPos();
		const float avatar = S(46.f);
		const float height = avatar + S(8.f);
		DrawAvatar(d, *player, ImVec2(pos.x + avatar * 0.5f + S(2.f), pos.y + height * 0.5f), avatar);

		auto [rank, rank_color] = RankText(*player);
		auto rank_size = font_bold->CalcTextSizeA(S(16.f), FLT_MAX, 0.f, rank.c_str());
		d->AddText(font_bold, S(16.f), ImVec2(pos.x + width - rank_size.x, pos.y + height * 0.5f - S(19.f)), C(rank_color), rank.c_str());

		std::string rank_under = player->rank_type == 11 ? "Premier" : player->rank_type == 7 ? "Wingman" : player->ranking > 0 ? "Competitive" : "";
		if (!player->bot && player->wins > 0)
			rank_under += std::format("{}{} wins", rank_under.empty() ? "" : "  ", player->wins);
		auto under_size = ImGui::CalcTextSize(rank_under.c_str());
		d->AddText(ImVec2(pos.x + width - under_size.x, pos.y + height * 0.5f + S(3.f)), C(col::text_dim), rank_under.c_str());

		float text_x = pos.x + avatar + S(14.f);
		float text_right = pos.x + width - std::max(rank_size.x, under_size.x) - S(12.f);
		auto name = player->name.empty() ? std::string("?") : player->name;
		d->PushClipRect(ImVec2(text_x, pos.y), ImVec2(text_right, pos.y + height), true);
		d->AddText(font_bold, S(16.f), ImVec2(text_x, pos.y + height * 0.5f - S(19.f)), C(col::text), name.c_str());

		std::string state = player->bot ? "Bot"
			: !profile.loaded ? "Loading Steam profile..."
			: !profile.state.empty() ? profile.state
			: "Steam profile not found";
		d->AddText(ImVec2(text_x, pos.y + height * 0.5f + S(3.f)), C(col::text_dim), state.c_str());
		d->PopClipRect();

		ImGui::Dummy(ImVec2(width, height + S(8.f)));
	}

	SectionTitle("STEAM");
	if (player->bot)
		TextBlock("Bots have no Steam profile");
	else {
		InfoLine("Steam Name", profile.persona);
		InfoLine("Real Name", profile.real_name);
		InfoLine("Location", profile.location);
		InfoLine("Member Since", profile.member_since);
		if (profile.cs2_listed) {
			InfoLine("CS2 Hours", profile.cs2_hours.empty() ? std::string() : profile.cs2_hours + " h");
			InfoLine("Last 2 Weeks", profile.cs2_recent_hours.empty() ? std::string() : profile.cs2_recent_hours + " h");
		}
		else if (profile.loaded && profile.privacy == "public")
			InfoLine("CS2 Hours", "Hidden", col::text_dim);	// Game details private, or not played in two weeks
		if (profile.loaded) {
			std::string privacy = profile.privacy == "public" ? "Public" : profile.privacy == "friendsonly" ? "Friends only"
				: profile.privacy.empty() ? "" : "Private";
			InfoLine("Profile", privacy);
			InfoLine("VAC", profile.vac_banned ? "Banned" : "Clean", profile.vac_banned ? IM_COL32(235, 75, 75, 255) : IM_COL32(126, 204, 142, 255));
			if (!profile.trade_ban.empty())
				InfoLine("Trade Ban", profile.trade_ban, profile.trade_ban == "None" ? col::text : IM_COL32(235, 75, 75, 255));
			InfoLine("Limited Account", profile.limited ? "Yes" : "No");
		}
		InfoLine("SteamID64", std::to_string(player->steam_id), col::text_dim);
	}

	// Their profile in the browser (Steam, csst.at), their ID, their name for ours. Two rows of two
	ImGui::Dummy(ImVec2(0, S(10.f)));
	{
		auto pos = ImGui::GetCursorScreenPos();
		const float gap = S(6.f);
		auto button = ImVec2(floorf((width - gap) / 2.f), S(28.f));
		auto second = ImVec2(button.x + gap, 0.f);
		auto below = ImVec2(0.f, button.y + gap);

		auto open = [](const std::wstring& url) { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };

		ImGui::BeginDisabled(player->bot);
		if (PillButton("Steam Profile##open", pos, button))
			open(std::format(L"https://steamcommunity.com/profiles/{}", player->steam_id));
		if (PillButton("csst.at##csst", pos + second, button, false, "Their CS2 stats on csst.at, in the browser"))
			open(std::format(L"https://csst.at/profile/{}", player->steam_id));
		if (PillButton("Copy ID##copy", pos + below, button, false, "Copies their SteamID64"))
			ImGui::SetClipboardText(std::to_string(player->steam_id).c_str());
		ImGui::EndDisabled();

		bool can_steal = ClanTag::IsNameAvailable() && !player->local;
		ImGui::BeginDisabled(!can_steal);
		if (PillButton("Steal Name##steal", pos + below + second, button, true,
			ClanTag::IsNameAvailable() ? "Their name as yours (Change Name)" : "Only available when the game is launched with -insecure")) {
			CopyName(cfg::misc::name_text, sizeof(cfg::misc::name_text), player->name);
			cfg::misc::name_change = true;
		}
		ImGui::EndDisabled();

		ImGui::SetCursorScreenPos(pos);
		ImGui::Dummy(ImVec2(width, button.y * 2.f + gap));
	}

	EndPanel();
}

// A sound of the sound folder: on & off, behind the dots which file (heard when picked) & its volume (heard when let go)
static void SoundRows(const char* label, bool* enabled, std::string& file, int* volume, const char* tooltip) {
	bool changed = ToggleMore(label, enabled, tooltip, [&] {
		auto files = Sounds::List();

		if (files.empty()) {
			auto text = std::format("No sounds in the {} folder yet, download them or put .vsnd_c, .wav or .mp3 files in it",
				Sounds::Folder().string());
			TextBlock(text.c_str());
			return;
		}

		auto found = std::find(files.begin(), files.end(), file);
		if (found == files.end()) {
			file = files.front();
			found = files.begin();
		}

		// Shown without the file extension
		std::vector<std::string> shown;
		for (const auto& name : files) {
			auto dot = name.find_last_of('.');
			shown.push_back(dot == std::string::npos || dot == 0 ? name : name.substr(0, dot));
		}

		std::vector<const char*> names;
		for (const auto& name : shown)
			names.push_back(name.c_str());

		int index = static_cast<int>(found - files.begin());
		if (DropdownRow("Sound", &index, names, nullptr, nullptr, S(150.f))) {
			file = files[index];
			Sounds::Play(file, *volume);
		}

		SliderInt("Volume", volume, 0, 100, "%d%%");
		if (slider_released)
			Sounds::Play(file, *volume);
	});

	if (changed && *enabled)
		Sounds::Preload(file);
}

void Menu::RenderMiscTab() {
	constexpr auto unavailable = "Only available when the game is launched with -insecure";

	misc_page_progress = std::min(1.f, misc_page_progress + ImGui::GetIO().DeltaTime * TAB_SWITCH_SPEED);
	float page = EaseOutCubic(misc_page_progress);

	// The pages are pills in the header, two columns of panels here sliding in after a switch
	auto columns = column_layout;
	column_layout.y += (1.f - page) * TAB_SLIDE;
	ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * page);

	switch (misc_page) {
	case 0: {
		const char* camera_missing = View::IsAvailable()
			? "The camera code was not found in this game version"
			: unavailable;

		BeginColumn(0);
		{
			BeginPanel("VIEW");
			ImGui::BeginDisabled(!View::IsAvailable());
			ToggleMore("FOV Override", &cfg::view::fov_enabled, View::IsAvailable()
				? "Camera field of view, scoped weapons keep their zoom"
				: unavailable, [&] {
				SliderInt("FOV", &cfg::view::fov, 60, 140, "%d");
			});
			ImGui::EndDisabled();

			ImGui::BeginDisabled(!View::IsThirdPersonAvailable());
			ToggleBindMore("Third Person", &cfg::view::third_person, &cfg::view::third_person_mode, &cfg::view::third_person_key,
				View::IsThirdPersonAvailable()
					? nullptr
					: View::IsAvailable()
						? "The third person code was not found in this game version"
						: unavailable, [&] {
				Toggle("Off While Scoped", &cfg::view::third_person_scoped_off);
			});
			ImGui::EndDisabled();

			ImGui::BeginDisabled(!Freecam::IsAvailable());
			ToggleMore("View Punch", &cfg::view::punch_enabled, Freecam::IsAvailable()
				? "How much the camera kicks when you shoot or get hit, 100% is the game\n"
				  "Only what you see: where the bullets go stays the same"
				: camera_missing, [&] {
				SliderFloat("Amount", &cfg::view::punch_scale, 0.f, 100.f, "%.0f%%", "0% keeps the camera still");
			});
			ImGui::EndDisabled();

			// The console variables of the game, so the same limits
			ImGui::BeginDisabled(!View::IsAvailable());
			ToggleMore("Viewmodel", &cfg::view::viewmodel_enabled, View::IsAvailable()
				? "Position & field of view of the weapon in your hands, like viewmodel_fov & viewmodel_offset_x/y/z in the console\nYour own values come back when turned off"
				: unavailable, [&] {
				SliderFloat("FOV", &cfg::view::viewmodel_fov, 40.f, 120.f, "%.0f", "The game allows 60 - 68");
				SliderFloat("Offset X", &cfg::view::viewmodel_x, -20.f, 20.f, "%.1f", "Left & right, the game allows -2 - 2.5");
				SliderFloat("Offset Y", &cfg::view::viewmodel_y, -20.f, 20.f, "%.1f", "Forward & back, the game allows -2 - 2");
				SliderFloat("Offset Z", &cfg::view::viewmodel_z, -20.f, 20.f, "%.1f", "Up & down, the game allows -2 - 2");

				if (!View::IsViewmodelAvailable())
					TextBlock("Looking for the viewmodel settings of the game...");
			});
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			BeginPanel("CAMERA");
			ImGui::BeginDisabled(!Freecam::IsAvailable());
			ToggleMore("Free Cam", &cfg::view::freecam, Freecam::IsAvailable()
				? "The camera flies on its own while your player stands still, only while alive\n"
				  "The mouse looks around, W A S D fly where you look, space up, left ctrl down, left shift faster\n"
				  "Your player gets none of it: no turning, walking, shooting or scoping"
				: camera_missing, [&] {
				KeybindRow("Key", &cfg::view::freecam_key);
				SliderFloat("Speed", &cfg::view::freecam_speed, 100.f, 3000.f, "%.0f", "Units per second, three times faster with shift");
				SliderFloat("Sensitivity", &cfg::view::freecam_sensitivity, 0.1f, 5.f, "%.2fx", "Turning with the mouse, times your sensitivity in the game");
			});
			ImGui::EndDisabled();

			ImGui::BeginDisabled(!Freecam::IsDeadAvailable());
			ToggleMore("Casual Spectate", &cfg::view::dead_spectate, Freecam::IsDeadAvailable()
				? "Once you die, watch anyone like in casual, enemies too, no key needed\n"
				  "Left & right click switch the player, space goes first person, third person, free cam"
				: Freecam::IsAvailable()
					? "The spectator camera was not found in this game version"
					: camera_missing, [&] {
				SliderFloat("Distance", &cfg::view::spectate_distance, 40.f, 300.f, "%.0f", "Behind them in third person, W & S change it");
			});
			ImGui::EndDisabled();
			EndPanel();

			// Our agent & hands, our weapon in our material: always seen, one switch each
			BeginPanel("LOCAL CHAMS");
			{
				static int part = 0;
				SegmentRow("Part", &part, { "Agent & Hands", "Weapon" },
					"Agent & Hands: your player in third person & what it holds, your arms & gloves in first person\n"
					"Weapon: the weapon in your hands in first person");
				namespace chams = cfg::visuals::material_chams;
				MaterialChamsRows(part == 0 ? chams::local : chams::weapon, part == 0
					? "Your player in third person & your arms in first person in our material"
					: "The weapon in your hands in our material", nullptr, "Chams");
			}
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
			next_more_width = S(360.f);
			ToggleMore("Clan Tag", &cfg::misc::clantag, ClanTag::IsAvailable()
				? "Animated tag, sent to the server: everyone there sees it.\n"
				  "There it changes at most once a second"
				: unavailable, [&] {
				SegmentRow("Text", &cfg::misc::clantag_source, { "Custom", "Players" },
					"Custom: the text below\nPlayers: the names of everyone in the match take turns");

				if (cfg::misc::clantag_source == 0)
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
			});
			ImGui::EndDisabled();
			EndPanel();

			BeginPanel("NAME");
			ImGui::BeginDisabled(!ClanTag::IsNameAvailable());
			Toggle("Change Name", &cfg::misc::name_change, ClanTag::IsNameAvailable()
				? "Your whole name in the scoreboard & kill feed, sent to the server: everyone there sees it.\n"
				  "Empty keeps your real name, turned off your real name goes back"
				: unavailable);
			if (cfg::misc::name_change)
				TextField("##name_text", "Name...", cfg::misc::name_text, sizeof(cfg::misc::name_text));
			ImGui::EndDisabled();
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		MatchPlayersPanel();
		EndColumn();
		break;
	}

	default: {
		// The radar of the game needs memory writes
		if (!GameRadar::IsAvailable())
			cfg::world::radar::mode = cfg::world::radar::MODE_OVERLAY;

		BeginColumn(0);
		{
			BeginPanel("OVERLAYS");
			ToggleMore("Radar", &cfg::world::radar::enabled, nullptr, [&] {
				ImGui::BeginDisabled(!GameRadar::IsAvailable());
				SegmentRow("Type", &cfg::world::radar::mode, { "External", "Internal" }, GameRadar::IsAvailable()
					? "External: our own radar window\nInternal: enemies show up on the radar of the game, even when nobody sees them"
					: "Internal is only available when the game is launched with -insecure");
				ImGui::EndDisabled();

				// Only our own window turns & has a range
				ImGui::BeginDisabled(cfg::world::radar::mode != cfg::world::radar::MODE_OVERLAY);
				Toggle("Disable Rotation", &cfg::world::radar::no_rotate);
				SliderFloat("Range", &cfg::world::radar::range, 100.f, 8000.f, "%.0f u");
				ImGui::EndDisabled();
			});

			ToggleMore("Spectator List", &cfg::world::spectators::enabled, nullptr, [&] {
				Toggle("Detailed", &cfg::world::spectators::detailed);
				Toggle("Only Self", &cfg::world::spectators::self_only);
			});

			ToggleMore("Vote List", &cfg::world::votes::enabled,
				"Shows the vote going on (kick, surrender, timeout...) of your team or the enemy with its yes & no.\n"
				"Only you see it. Who called it, on whom & who voted what needs Voter Names", [&] {
				Toggle("Voter Names", &cfg::world::votes::names,
					"Experimental: who voted yes or no with their profile picture, heard from inside the game.\n"
					"Needs -insecure. Turn it off if the game crashes");
			});

			Toggle("Keybind List", &cfg::world::keybinds::enabled,
				"Shows the features turned on by a key while they are on: third person, free cam, bunny hop, air strafe, jump bug.\n"
				"With the menu open it shows them all & can be dragged");

		#ifdef _DEBUG // Part of the velocity graph for developers
			ToggleMore("Velocity Graph", &cfg::world::velocity::enabled, nullptr, [&] {
				SliderInt("Sample Rate", &cfg::world::velocity::sample_rate, 1, 100, "%d");
				SliderFloat("Sample Length", &cfg::world::velocity::sample_length, 1.f, 20.f, "%.1f s");
			});
		#else
			Toggle("Velocity Graph", &cfg::world::velocity::enabled);
		#endif
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			BeginPanel("CROSSHAIR");
			ToggleMore("Sniper Crosshair", &cfg::world::crosshair::enabled, "Draws a crosshair while holding an unscoped sniper", [&] {
				SegmentRow("Style", &cfg::world::crosshair::style, { "Classic", "CS2" },
					"Classic: small white cross\nCS2: your crosshair from the game settings (size, gap, color, outline, dot)");
				if (cfg::world::crosshair::style == cfg::world::crosshair::STYLE_GAME && !GameCrosshair::Get().found)
					TextBlock("No CS2 settings found in the Steam userdata, using the default crosshair");
			});
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
		namespace hm = cfg::world::hitmarker;
		const bool writes = Visuals::IsAvailable();

		BeginColumn(0);
		{
			BeginPanel("REMOVALS");
			ImGui::BeginDisabled(!writes);
			ToggleMore("No Flash", &vis::no_flash, writes ? nullptr : unavailable, [&] {
				SliderFloat("Flash Strength", &vis::flash_alpha, 0.f, 255.f, "%.0f", "0 is no white at all, 255 is the game");
			});
			Toggle("No Smoke", &vis::no_smoke, writes
				? "Smokes are not drawn. The smoke ESP still shows where they are"
				: unavailable);
			ImGui::EndDisabled();
			EndPanel();

			BeginPanel("HITMARKER");
			Toggle("Crosshair", &hm::crosshair);
			ToggleMore("World", &hm::world, "An X on the player you hit, at their bone nearest to your crosshair", [&] {
				Toggle("Damage", &hm::damage);
			});
			MoreRow("Look", "How long & how big the hitmarkers are, their colors", [&] {
				SliderFloat("Duration", &hm::duration, 0.2f, 2.f, "%.1f s");
				SliderFloat("Size", &hm::size, 3.f, 16.f, "%.0f");
			}, { { &hm::color, "Color" }, { &hm::kill_color, "Kill color" } });
			EndPanel();
		}
		EndColumn();

		BeginColumn(1);
		{
			BeginPanel("HIT & KILL SOUND");
			SoundRows("Hit Sound", &cfg::misc::hitsound, cfg::misc::hitsound_file, &cfg::misc::hitsound_volume, nullptr);
			SoundRows("Kill Sound", &cfg::misc::killsound, cfg::misc::killsound_file, &cfg::misc::killsound_volume,
				"Played instead of the hit sound when you kill");

			{
				auto row = BeginRow("Download Sounds");
				const ImVec2 button(S(96.f), S(24.f));
				auto at = RowSlot(row, button);
				bool busy = Sounds::IsDownloading();

				ImGui::BeginDisabled(busy);
				if (PillButton(busy ? "Downloading" : "Download", at, button, true))
					Sounds::Download();
				ImGui::EndDisabled();
				EndRow(row);
			}

			auto status = Sounds::GetStatus();
			if (!status.empty())
				TextBlock(status.c_str());
			EndPanel();

			// An effect of the game where an enemy we killed was, the body gone
			BeginPanel("KILL EFFECT");
			{
				namespace ke = cfg::world::kill_effect;
				bool available = KillEffect::IsAvailable();
				const char* why = Engine::IsInsecure()
					? "The particle code of the game was not found in this game version"
					: "Only available when the game is launched with -insecure";
				static const char* effect_descriptions[] = {
					"The body burns to ashes",
					"A blast where it was",
					"The body bursts into blood",
					"The body stays, shocked all over by the zeus",
					"Big lightning strikes where the body was, drawn over the game",
					"A black hole opens & pulls the body in, drawn over the game",
					"The body turns to glass & shatters, drawn over the game",
					"The body breaks up into glitching pixels, drawn over the game",
					"A beam of light from above, the body rises in it as light, drawn over the game",
					"The body freezes to ice, cracks & crumbles, drawn over the game",
					"A blade of light cuts the body in two, drawn over the game",
					"The body falls apart into blocks that bounce on the ground, drawn over the game",
					"A fireball falls from the sky & hits the body, drawn over the game",
					"A whirlwind takes the body up, drawn over the game",
					"The body is switched off like an old television, drawn over the game",
					"The body falls into a star that explodes, drawn over the game",
					"The body floats away instead of falling",
				};

				ImGui::BeginDisabled(!available);
				Toggle("Enabled", &ke::enabled, available
					? "An enemy you kill: its body is gone at once & an effect of the game plays in its place"
					: why);
				ImGui::BeginDisabled(!ke::enabled);
				const auto& names = KillEffect::GetNames();
				ke::effect = std::clamp(ke::effect, 0, static_cast<int>(names.size()) - 1);
				DropdownRow("Effect", &ke::effect, names, effect_descriptions, effect_descriptions[ke::effect]);
				if (KillEffect::Floats(ke::effect))
					SliderFloat("Gravity", &ke::gravity, -1.f, 1.f, "%.2fx",
						"Times the normal gravity: below 0 the body rises, 0 it hangs where it died. Every body floats while this is picked, not only your kills");
				ImGui::EndDisabled();
				ImGui::EndDisabled();
			}
			EndPanel();

			// An effect of the game on our own player
			BeginPanel("SELF EFFECT");
			{
				namespace se = cfg::world::self_effect;
				bool available = KillEffect::IsAvailable();
				const char* why = Engine::IsInsecure()
					? "The particle code of the game was not found in this game version"
					: "Only available when the game is launched with -insecure";
				static const char* self_descriptions[] = {
					"Zeus sparks running over your body",
					"Ashes coming off your body",
					"Small lightning jumping over & out of your body, drawn over the game",
					"Three little black holes going round you, drawn over the game",
					"A gold halo over your head & sparkles rising, drawn over the game",
					"Ice crystals going round you & snow falling, drawn over the game",
					"Two neon rings turning around you, drawn over the game",
					"Fireflies wandering around you, drawn over the game",
					"Sakura petals falling & turning around you, drawn over the game",
					"Six blades of light going round you, drawn over the game",
					"Green code raining down around you, drawn over the game",
					"A dome of energy around you, waves going over it, drawn over the game",
					"A ribbon of light behind you as you move, drawn over the game",
				};

				ImGui::BeginDisabled(!available);
				Toggle("Enabled", &se::enabled, available
					? "An effect of the game on your own player, played again & again.\nOnly you see it"
					: why);
				ImGui::BeginDisabled(!se::enabled);
				const auto& names = KillEffect::GetSelfNames();
				se::effect = std::clamp(se::effect, 0, static_cast<int>(names.size()) - 1);
				DropdownRow("Effect", &se::effect, names, self_descriptions, self_descriptions[se::effect]);
				if (KillEffect::SelfUsesInterval(se::effect))
					SliderFloat("Interval", &se::interval, 0.2f, 5.f, "%.1f s", "Time between two plays: lower is denser");
				Toggle("Third Person Only", &se::third_person_only,
					"In first person the effect is all around the camera & covers the view");
				ImGui::EndDisabled();
				ImGui::EndDisabled();
			}
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
		Toggle("ESP", &cfg::enabled);

		if (Toggle("Streamproof", &cfg::settings::streamproof, "Hides the overlay from screen capture and streaming software"))
		{
			Window::SetAffinity(
				Window::hwnd,
				cfg::settings::streamproof ? WindowAffinity::Invisible : WindowAffinity::Disabled
			);
		}

		Toggle("Notifications", &cfg::settings::notifications, "Map building, notices of the program & the ESP off message");




		// Steps of 5%, taken once the mouse is let go
		int percent = static_cast<int>(std::round(cfg::settings::ui_scale * 20.f)) * 5;
		if (SliderInt("Menu Scale", &percent, 80, 160, "%d%%"))
			cfg::settings::ui_scale = std::round(percent / 5.f) * 5.f / 100.f;
		EndPanel();

	#ifdef _DEBUG
		BeginPanel("DEV");
		if (Toggle("Console", &cfg::dev::console) && !cfg::dev::console)
			LogHelper::Free();

		SliderInt("Cache Refresh", &cfg::dev::cache_refresh_rate, 0, 100, "%d ms");
		Toggle("Force Show Flags", &cfg::dev::force_show_flags);
		EndPanel();

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
		Toggle("Free CPU", &cfg::settings::free_cpu, "Let the CPU sleep to free resources\nOff as a last resort: more CPU use, less latency");
		Toggle("Watermark", &cfg::settings::watermark);

	#ifdef _DEBUG
		ImGui::BeginDisabled(!cfg::settings::watermark);
		Toggle("Frame Times", &cfg::settings::frame_times, "Shows in the watermark where the time of a frame goes, in milliseconds");
		ImGui::EndDisabled();
	#endif
		EndPanel();
	}
	EndColumn();
}

// Colors of ImGui itself (popups, inputs, the overlay windows) from the palette
static void ApplyStyleColors() {
	ImGuiStyle& style = ImGui::GetStyle();

	auto c = [](ImU32 color) { return ImGui::ColorConvertU32ToFloat4(color); };

	style.Colors[ImGuiCol_Text] = c(col::text);
	style.Colors[ImGuiCol_TextDisabled] = c(col::text_dim);
	style.Colors[ImGuiCol_TextLink] = c(col::accent);
	style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.28f, 0.55f, 1.00f, 0.35f);

	style.Colors[ImGuiCol_WindowBg] = c(col::window);
	style.Colors[ImGuiCol_ChildBg] = c(col::panel);
	style.Colors[ImGuiCol_PopupBg] = c(col::panel_top);
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

	style.Colors[ImGuiCol_Button] = c(col::button);
	style.Colors[ImGuiCol_ButtonHovered] = c(col::button_hover);
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
}

// The theme, applied once
static void RefreshTheme() {
	static bool applied = false;
	if (applied)
		return;

	applied = true;
	col::ApplyTheme();
	ApplyStyleColors();
}

void Menu::SetupStyles() {
	RefreshTheme();
	ImGuiStyle& style = ImGui::GetStyle();

	style.WindowBorderSize = 1.f;
	style.ChildBorderSize = 1.f;
	style.PopupBorderSize = 1.f;
	style.FrameBorderSize = 0.f;

	style.WindowRounding = WINDOW_ROUNDING;
	style.ChildRounding = PANEL_ROUNDING;
	style.FrameRounding = 3.f;
	style.PopupRounding = 8.f;
	style.GrabRounding = 3.f;
	style.ScrollbarRounding = 3.f;
	style.ScrollbarSize = 4.f;

	style.WindowPadding = ImVec2(10.f, 10.f);
	style.FramePadding = ImVec2(8.f, 4.f);
	style.ItemSpacing = ImVec2(8.f, 6.f);

	// Fonts: Inter is in the program, letters it does not have (other alphabets in names) come from Segoe UI
	auto& io = ImGui::GetIO();
	io.Fonts->Clear();

	auto font_path = [](const char* preferred, const char* fallback) {
		return std::filesystem::exists(preferred) ? preferred : fallback;
	};

	ImFontConfig text_cfg{};
	text_cfg.FontDataOwnedByAtlas = false;

	ImFontConfig fallback_cfg{};
	fallback_cfg.MergeMode = true;

	font_regular = io.Fonts->AddFontFromMemoryTTF(inter_medium_font, inter_medium_font_len, 16.0f, &text_cfg);

	ImFontConfig merge_icon_cfg{};
	merge_icon_cfg.FontDataOwnedByAtlas = false;
	merge_icon_cfg.MergeMode = true;
	merge_icon_cfg.GlyphOffset = Vec2_t(0, 3.f);

	// the icons will use the size specified when getting added so it ignores the base size
	static const ImWchar icon_ranges[] = { 0xE100, 0xE108, 0 };
	io.Fonts->AddFontFromMemoryTTF(icons_font, icons_font_len, 18.f, &merge_icon_cfg, icon_ranges);

	// Weapon silhouettes, used as tab icon
	ImFontConfig merge_weapon_cfg{};
	merge_weapon_cfg.FontDataOwnedByAtlas = false;
	merge_weapon_cfg.MergeMode = true;
	merge_weapon_cfg.GlyphOffset = Vec2_t(0, 1.f);

	static const ImWchar weapon_ranges[] = { 0xE000, 0xE046, 0 };
	io.Fonts->AddFontFromMemoryTTF(weapon_icon_font, weapon_icon_font_len, 13.f, &merge_weapon_cfg, weapon_ranges);

	io.Fonts->AddFontFromFileTTF(font_path("C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf"), 16.0f, &fallback_cfg);

	// Titles
	font_bold = io.Fonts->AddFontFromMemoryTTF(inter_bold_font, inter_bold_font_len, 16.0f, &text_cfg);
	io.Fonts->AddFontFromFileTTF(
		font_path("C:\\Windows\\Fonts\\seguisb.ttf",
			font_path("C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf")),
		16.0f,
		&fallback_cfg
	);
}

// In a match, while the pictures of the items load: small, at the bottom left, the game not covered
static void RenderLoadingCorner(bool show, ImFont* bold, ImFont* regular) {
	static float fade = 0.f;
	auto& io = ImGui::GetIO();
	fade = std::clamp(fade + io.DeltaTime * (show ? 4.f : -4.f), 0.f, 1.f);
	if (fade <= 0.f)
		return;

	auto d = ImGui::GetForegroundDrawList();
	float alpha = EaseOutCubic(fade);
	auto with_alpha = [&](ImU32 color, float a = 1.f) { return ImGui::GetColorU32(color, a * alpha); };

	const float padding = S(12.f);
	auto size = ImVec2(S(240.f), S(54.f));
	auto min = ImVec2(S(16.f) - (1.f - alpha) * S(10.f), io.DisplaySize.y - S(16.f) - size.y);
	auto max = min + size;

	SoftShadow(d, min, max, WINDOW_ROUNDING);
	d->AddRectFilled(min, max, with_alpha(col::window), WINDOW_ROUNDING);
	d->AddRect(min, max, with_alpha(col::border), WINDOW_ROUNDING, 0, 1.f);

	d->AddText(bold, S(13.f), min + ImVec2(padding, padding - S(1.f)), with_alpha(col::text), "Loading item pictures");
	auto progress = ImageCache::GetPrefetchProgress();
	if (progress.rfind("Loading ", 0) == 0)
		progress = progress.substr(8);
	auto progress_width = regular->CalcTextSizeA(S(12.f), FLT_MAX, 0.f, progress.c_str()).x;
	d->AddText(regular, S(12.f), ImVec2(max.x - padding - progress_width, min.y + padding), with_alpha(col::text_dim), progress.c_str());

	float percent = ImageCache::GetPrefetchPercent();
	auto bar_min = ImVec2(min.x + padding, max.y - padding - S(4.f)), bar_max = ImVec2(max.x - padding, max.y - padding);
	d->AddRectFilled(bar_min, bar_max, with_alpha(col::track), S(2.f));
	if (percent > 0.001f)
		d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * percent, bar_max.y), with_alpha(col::accent), S(2.f));
}

// While the program gets ready: a box in the middle, the percent in a ring, the name, Loading & a bar. The agents of the
// preview are taken from the game under its top part (the overlay is left out of the capture), not said
void Menu::RenderLoadingCard() {
	static float fade = 0.f;
	static float shown = 0.f;
	auto& io = ImGui::GetIO();
	bool agents_ready = AgentPreview::IsReady();
	bool pictures_ready = ImageCache::IsPrefetched();

	// In a match the game is not covered: the pictures of the items load in a corner, the agents are taken once in the
	// main menu
	auto snapshot = Cache::Current();
	bool in_match = snapshot && snapshot->globals.in_match;
	RenderLoadingCorner(in_match && !pictures_ready, font_bold, font_regular);

	bool ready = (agents_ready && pictures_ready) || in_match;
	if (ready && fade <= 0.f)
		return;
	fade = std::clamp(fade + io.DeltaTime * (ready && (shown >= 0.999f || in_match) ? -4.f : 4.f), 0.f, 1.f);
	if (fade <= 0.f)
		return;

	// The percent goes up smoothly to where it is: the pictures of the items, with the agents when they had to be
	// taken when it started
	static const bool agents_part = !agents_ready;
	float target = agents_part
		? (AgentPreview::GetPercent() + ImageCache::GetPrefetchPercent()) * 0.5f
		: ImageCache::GetPrefetchPercent();
	if (agents_ready && pictures_ready)
		target = 1.f;
	shown = target < shown ? target : shown + (target - shown) * std::min(1.f, io.DeltaTime * (ready ? 10.f : 4.f));
	if (target - shown < 0.001f)
		shown = target;

	auto d = ImGui::GetForegroundDrawList();
	float alpha = EaseOutCubic(fade);
	auto with_alpha = [&](ImU32 color, float a = 1.f) { return ImGui::GetColorU32(color, a * alpha); };
	float time = static_cast<float>(ImGui::GetTime());

	// The top part covers the agent of the game: tall like it, the picture stays sharp in the preview
	const float padding = S(16.f);
	float spot_height = floorf(std::clamp(io.DisplaySize.y * 0.42f, 300.f, 440.f));
	auto spot_size = ImVec2(floorf(spot_height * 0.66f), spot_height);
	auto size = ImVec2(spot_size.x + padding * 2.f, spot_size.y + padding * 2.f + S(64.f));
	auto min = ImVec2(floorf((io.DisplaySize.x - size.x) * 0.5f), floorf((io.DisplaySize.y - size.y) * 0.5f + (1.f - alpha) * S(12.f)));
	auto max = min + size;
	auto spot_min = min + ImVec2(padding, padding);
	auto spot_max = spot_min + spot_size;

	SoftShadow(d, min, max, WINDOW_ROUNDING);
	d->AddRectFilled(min, max, with_alpha(col::window), WINDOW_ROUNDING);
	d->AddRect(min, max, with_alpha(col::border), WINDOW_ROUNDING, 0, 1.f);

	// The percent in the middle of a ring filled as far, a short arc turning around it
	auto center = ImVec2(floorf((spot_min.x + spot_max.x) * 0.5f), floorf((spot_min.y + spot_max.y) * 0.5f));
	float radius = floorf(spot_size.x * 0.3f);
	const float pi = std::numbers::pi_v<float>;
	d->AddCircle(center, radius, with_alpha(col::track), 64, S(4.f));
	if (shown > 0.001f) {
		d->PathArcTo(center, radius, -pi * 0.5f, -pi * 0.5f + pi * 2.f * shown, 64);
		d->PathStroke(with_alpha(col::accent), 0, S(4.f));
	}
	d->PathArcTo(center, radius + S(9.f), time * 2.5f, time * 2.5f + pi * 0.35f, 16);
	d->PathStroke(with_alpha(col::accent, 0.35f), 0, S(2.f));

	auto percent = std::format("{}%", static_cast<int>(shown * 100.f + 0.0001f));
	auto percent_size = font_bold->CalcTextSizeA(S(30.f), FLT_MAX, 0.f, percent.c_str());
	d->AddText(font_bold, S(30.f), ImVec2(floorf(center.x - percent_size.x * 0.5f), floorf(center.y - percent_size.y * 0.5f)), with_alpha(col::text), percent.c_str());

	// The name, Loading & what it waits for, a bar filled as far
	float y = spot_max.y + S(14.f);
	d->AddText(font_bold, S(16.f), ImVec2(min.x + padding, y), with_alpha(col::text), BRAND_TITLE);

	std::string loading = ready ? "Ready" : "Loading" + std::string(static_cast<int>(time * 2.5f) % 4, '.');
	auto progress = AgentPreview::GetProgress();
	auto pictures = ImageCache::GetPrefetchProgress();
	if (!agents_ready && progress.rfind("Waiting", 0) == 0)
		loading = progress;
	else if (!ready && agents_ready && pictures.rfind("Loading ", 0) == 0)
		loading = pictures;
	d->AddText(font_regular, S(13.f), ImVec2(min.x + padding, y + S(22.f)), with_alpha(col::text_dim), loading.c_str());

	float bar_y = max.y - padding - S(4.f);
	auto bar_min = ImVec2(min.x + padding, bar_y), bar_max = ImVec2(max.x - padding, bar_y + S(4.f));
	d->AddRectFilled(bar_min, bar_max, with_alpha(col::track), S(2.f));
	if (shown > 0.001f)
		d->AddRectFilled(bar_min, ImVec2(bar_min.x + (bar_max.x - bar_min.x) * shown, bar_max.y), with_alpha(col::accent), S(2.f));

	// Covered all the way: the agent goes under the top part
	if (!agents_ready && !in_match && fade >= 1.f) {
		POINT origin{ 0, 0 };
		ClientToScreen(Window::hwnd, &origin);
		AgentPreview::Request(spot_min.x, spot_min.y, spot_size.x, spot_size.y, static_cast<float>(origin.x), static_cast<float>(origin.y));
	}
}

void Menu::RenderStartupHelpImpl() {
	col::accent = ImGui::ColorConvertFloat4ToU32(ImVec4(cfg::settings::accent.r, cfg::settings::accent.g, cfg::settings::accent.b, 1.f));
	col::backdrop = std::clamp(cfg::settings::menu_opacity, 0.3f, 1.f);
	RefreshTheme();

	// The overlay is left out of screen captures while a picture of an agent is taken, the agent of the game is under
	// the loading card
	static bool excluded = false;
	if (AgentPreview::IsCapturing() != excluded) {
		excluded = AgentPreview::IsCapturing();
		Window::SetAffinity(Window::hwnd, excluded || cfg::settings::streamproof ? WindowAffinity::Invisible : WindowAffinity::Disabled);
	}

	// The pictures of the items downloaded while it gets ready
	ImageCache::StartPrefetch();

	// Loading card in the middle while the program gets ready, the menu opens after it
	RenderLoadingCard();
	auto snapshot = Cache::Current();
	bool in_match = snapshot && snapshot->globals.in_match;
	if (!in_match && (!AgentPreview::IsReady() || !ImageCache::IsPrefetched()))
		return;

	// Ready: the pictures of what is worn made textures already, the skin changer opens with them there
	static bool worn_loaded = false;
	if (!worn_loaded && ImageCache::IsPrefetched() && Skins::IsLoaded()) {
		worn_loaded = true;
		std::lock_guard lock(cfg::skins::mutex);
		for (const auto& loadout : cfg::skins::loadouts) {
			if (auto agent = Skins::FindAgent(loadout.agent))
				ImageCache::Get(SmallImage(agent->image));
			if (auto knife = Skins::FindItem(loadout.knife); knife && !knife->image.empty())
				ImageCache::Get(SmallImage(knife->image));
			for (const auto& [index, item] : loadout.items)
				if (auto info = Skins::FindItem(index))
					if (auto skin = info->FindSkin(item.paint_kit))
						ImageCache::Get(SmallImage(skin->image));
		}
		if (auto kit = Skins::FindMusicKit(cfg::skins::music_kit); kit && !kit->image.empty())
			ImageCache::Get(kit->image);
	}

	// Toast in the top left corner of the game once the program is ready. Gone after a few seconds, or right away
	// once the menu opens
	constexpr float LIFETIME = 4.5f;

	static double started = -1.0;
	static bool dismissed = false;
	static float fade = 0.f;

	auto& io = ImGui::GetIO();
	double now = ImGui::GetTime();
	if (started < 0.0)
		started = now;

	float age = static_cast<float>(now - started);
	if (Renderer::IsOpen() || age > LIFETIME)
		dismissed = true;

	fade = std::clamp(fade + io.DeltaTime * (dismissed ? -5.f : 4.f), 0.f, 1.f);
	if (fade <= 0.f)
		return;

	auto d = ImGui::GetForegroundDrawList();
	float alpha = EaseOutCubic(fade);
	auto with_alpha = [&](ImU32 color, float a = 1.f) { return ImGui::GetColorU32(color, a * alpha); };

	// What it says: the name & version, then the keys of the menu as key caps
	const float title_size = S(16.f);
	const float text_size = S(13.f);
	const float padding = S(14.f);
	const float key_pad = S(6.f);

	auto version = std::format("v{}", Updater::GetVersion());
	auto title_width = font_bold->CalcTextSizeA(title_size, FLT_MAX, 0.f, BRAND_TITLE).x;
	auto version_width = font_regular->CalcTextSizeA(text_size, FLT_MAX, 0.f, version.c_str()).x;

	struct Part {
		const char* text;
		bool key;
	};
	const Part parts[] = { { "Press", false }, { "INSERT", true }, { "or", false }, { "RIGHT SHIFT", true }, { "to open the menu", false } };

	float line_width = 0.f;
	for (const auto& part : parts) {
		float w = font_regular->CalcTextSizeA(text_size, FLT_MAX, 0.f, part.text).x;
		line_width += w + (part.key ? key_pad * 2.f : 0.f) + S(6.f);
	}
	line_width -= S(6.f);

	auto size = ImVec2(padding * 2.f + std::max(title_width + S(8.f) + version_width, line_width), padding * 2.f + title_size + S(10.f) + S(20.f));

	// Slides in from the left
	auto min = ImVec2(floorf(S(16.f) - (1.f - alpha) * S(24.f)), S(16.f));
	auto max = min + size;

	d->AddRectFilled(min, max, with_alpha(col::window, 0.96f), S(8.f));
	d->AddRect(min, max, with_alpha(col::border_hover), S(8.f), 0, 1.f);

	// Accent bar on the left & the time left as a line under it all
	d->AddRectFilled(min + ImVec2(0.f, S(10.f)), ImVec2(min.x + S(2.f), max.y - S(10.f)), with_alpha(col::accent), 1.f);
	float left = dismissed ? 0.f : std::clamp(1.f - age / LIFETIME, 0.f, 1.f);
	d->AddRectFilled(ImVec2(min.x + S(8.f), max.y - S(3.f)), ImVec2(min.x + S(8.f) + (size.x - S(16.f)) * left, max.y - S(1.5f)), with_alpha(col::accent, 0.6f), 1.f);

	float y = min.y + padding;
	d->AddText(font_bold, title_size, ImVec2(min.x + padding, y), with_alpha(col::text), BRAND_TITLE);
	d->AddText(font_regular, text_size, ImVec2(min.x + padding + title_width + S(8.f), y + (title_size - text_size) * 0.5f + S(1.f)),
		with_alpha(col::accent), version.c_str());

	y += title_size + S(10.f);
	float x = min.x + padding;
	const float key_height = S(20.f);

	for (const auto& part : parts) {
		auto w = font_regular->CalcTextSizeA(text_size, FLT_MAX, 0.f, part.text).x;
		float text_y = y + (key_height - text_size) * 0.5f;

		if (part.key) {
			auto key_min = ImVec2(x, y);
			auto key_max = ImVec2(x + w + key_pad * 2.f, y + key_height);
			d->AddRectFilled(key_min, key_max, with_alpha(col::track_hover), S(4.f));
			d->AddRect(key_min, key_max, with_alpha(col::border_hover), S(4.f));
			d->AddText(font_regular, text_size, ImVec2(x + key_pad, text_y), with_alpha(col::text), part.text);
			x = key_max.x + S(6.f);
		}
		else {
			d->AddText(font_regular, text_size, ImVec2(x, text_y), with_alpha(col::text_dim), part.text);
			x += w + S(6.f);
		}
	}
}
