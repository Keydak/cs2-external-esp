#include "Esp.hpp"

#include "gui/renderer/Renderer.hpp"
#include "assets/fonts/WeaponIcons.h"
#include "assets/fonts/Icons.h"

#include "GameCrosshair.hpp"
#include "core/features/Freecam.hpp"
#include "core/features/Visuals.hpp"
#include "core/engine/Engine.hpp"
#include "core/offsets/Offsets.hpp"

#include <numbers>
#include <unordered_map>

bool Esp::Init() {
	return GetInstance().InitImpl();
}

void Esp::Render() {
    return GetInstance().RenderImpl();
}

bool Esp::InitImpl() {
	auto& io = ImGui::GetIO();

	ImFontConfig cfg{};
	cfg.FontDataOwnedByAtlas = false;

	this->font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 12.0f, &cfg);

	this->font_merged_icons = io.Fonts->AddFontFromMemoryTTF(
		weapon_icon_font,
		weapon_icon_font_len,
		16.0f,
		&cfg
	);

	cfg.MergeMode = true;

	static const ImWchar general_ranges[] = { 0xE100, 0xE108, 0 };
	io.Fonts->AddFontFromMemoryTTF(
		icons_font,
		icons_font_len,
		16.0f,
		&cfg,
		general_ranges
	);

	return true;
}

void Esp::RenderImpl() {
	if (!cfg::enabled)
		return;

	auto current = Cache::Current();
	const auto& snapshot = *current;
	auto& game = snapshot.game;
	auto& bomb = snapshot.bomb;
	auto& local = snapshot.local;
	auto& globals = snapshot.globals;
	auto& players = snapshot.players;

	ImGui::PushFont(this->font);

	this->io = ImGui::GetIO();
	this->d = ImGui::GetBackgroundDrawList();

	// The camera of right now, read as we draw: the one of the cache stands still while it reads all the players
	// (several ms, more on a slow PC) & may be copied half written, the boxes shook behind the mouse
	this->matrix = game.view_matrix;
	if (auto p = Engine::GetProcess())
		this->matrix = p->read<view_matrix_t>(Engine::GetClient().base + offsets::viewMatrix);

	struct Drawn {
		const Player* player;
		const cfg::esp::group_t* group;
		bool visible;
		float distance;
	};

	std::vector<Drawn> drawn;

	for (auto& player : players) {
		if (!player.alive)
			continue;

		if (player.localplayer)
			continue;

		bool mate = !game.IsEnemy(local.team, player.team);
		const auto& group = mate ? cfg::esp::team : cfg::esp::enemy;

		if (!group.enabled)
			continue;

		// Are we spectating the player in first person? then dont render
		// TODO: Exception here when spectating someone
		if (
			local.observer_services.target == player.pawn_controller_addr
			&& local.observer_services.mode == ObserverMode::First
		)
			continue;

		// The same through their eyes with the free cam
		if (Freecam::IsFirstPerson() && Freecam::GetTarget() == player.GetPawnAddress())
			continue;

		bool visible = player.visible;
		if (group.visible_only && !visible)
			continue;

		auto delta = player.pos - local.pos;
		drawn.push_back({ &player, &group, visible, delta.x * delta.x + delta.y * delta.y + delta.z * delta.z });
	}

	// The far ones first so close ones cover them
	std::sort(drawn.begin(), drawn.end(), [](const Drawn& a, const Drawn& b) { return a.distance > b.distance; });

	for (const auto& entry : drawn) {
		RenderPlayerTracers(local, *entry.player, *entry.group, entry.visible);
		RenderPlayer(local, *entry.player, *entry.group, entry.visible);
	}

	// The crosshair is where our player aims, not where the camera looks
	if (Freecam::GetMode() == Freecam::Mode::Off)
		RenderCrosshair(local);
	RenderItems(snapshot.items, local.pos);
	RenderBombBox(bomb);
	RenderGrenades(snapshot.grenades);
	RenderGrenadePrediction(snapshot.grenade_path);
	ImGui::PopFont();
}

ImFont* Esp::GetIconFont() {
	return GetInstance().font_merged_icons;
}

void Esp::RenderPreview(ImDrawList* d, const view_matrix_t& matrix, const Bomb* bomb,
	const std::vector<Grenade>& grenades, const GrenadePath* path,
	const std::vector<Item>* items, Vec3_t viewer) {
	auto& i = GetInstance();

	// Drawn like the real thing, into the menu instead of the overlay
	auto previous_d = i.d;
	auto previous_matrix = i.matrix;
	auto previous_size = i.io.DisplaySize;

	i.d = d;
	i.matrix = matrix;
	i.io.DisplaySize = ImGui::GetIO().DisplaySize;

	ImGui::PushFont(i.font);

	if (bomb)
		i.RenderBombBox(*bomb);

	if (items)
		i.RenderItems(*items, viewer);

	i.RenderGrenades(grenades);

	if (path)
		i.RenderGrenadePrediction(*path);

	ImGui::PopFont();

	i.d = previous_d;
	i.matrix = previous_matrix;
	i.io.DisplaySize = previous_size;
}

void Esp::RenderPlayer(const Player& local, const Player& player, const cfg::esp::group_t& group, bool visible) {
	// Needed for flags & item sizing, so even if the box is not enabled
	// Should be calculated
	std::pair<Vec2_t, Vec2_t> bounds;
	if (!player.GetBounds(matrix, io.DisplaySize, bounds))
		return;

	// Causes hp bars across the screen when they respawn
	if (!player.alive)
		return;

	if (group.box)
		DrawBox(d, bounds.first, bounds.second, visible ? group.box_visible : group.box_invisible);

	if (group.skeleton)
		RenderPlayerBones(player, visible, group.skeleton_visible, group.skeleton_invisible);

	if (group.head_tracker)
		RenderPlayerTracker(player, bounds, visible ? group.tracker_visible : group.tracker_invisible);

	DrawFlagsImpl(d, local, player, bounds.first, bounds.second, group, cfg::dev::force_show_flags, nullptr);
}

void Esp::DrawBox(ImDrawList* d, Vec2_t min, Vec2_t max, const color_t& color) {
	// Dark outline keeps the box readable on bright walls
	d->AddRect(min - Vec2_t(1, 1), max + Vec2_t(1, 1), IM_COL32(0, 0, 0, static_cast<int>(color.a * 120)));
	d->AddRect(min, max, ImColor(color));
}

void Esp::DrawTracker(ImDrawList* d, Vec2_t head, float box_width, const color_t& color) {
	d->AddCircle(head, box_width / 6, ImColor(color), 15);
}

void Esp::DrawFlags(ImDrawList* d, const Player& local, const Player& player, Vec2_t min, Vec2_t max,
	const cfg::esp::group_t& group, bool force, std::vector<FlagArea>* areas) {
	GetInstance().DrawFlagsImpl(d, local, player, min, max, group, force, areas);
}

void Esp::RenderPlayerBones(const Player& player, bool visible, const color_t& visible_color, const color_t& invisible_color) {
	// Each bone in its own color, seen or not: a line between a seen & a hidden one changes color halfway
	auto seen = [&](int bone) { return visible && (player.visible_bones & 1u << bone) != 0; };
	auto bone_count = player.bone_list.size();
	for (const auto& bone : connections) {
		int first = bone[0], second = bone[1];

		if (bone_count <= first || bone_count <= second)
			continue;

		const auto& bone1 = player.bone_list[first];
		const auto& bone2 = player.bone_list[second];

		Vec2_t scb1;
		if (!matrix.wts(bone1.pos, io.DisplaySize, scb1))
			continue;

		Vec2_t scb2;
		if (!matrix.wts(bone2.pos, io.DisplaySize, scb2))
			continue;

		bool seen1 = seen(first), seen2 = seen(second);
		ImU32 color1 = ImColor(seen1 ? visible_color : invisible_color);
		ImU32 color2 = ImColor(seen2 ? visible_color : invisible_color);

		if (seen1 == seen2) {
			d->AddLine(scb1, scb2, color1, 1.5f);
			continue;
		}

		auto middle = Vec2_t((scb1.x + scb2.x) * 0.5f, (scb1.y + scb2.y) * 0.5f);
		d->AddLine(scb1, middle, color1, 1.5f);
		d->AddLine(middle, scb2, color2, 1.5f);
	}
}

void Esp::RenderPlayerTracker(const Player& player, std::pair<Vec2_t, Vec2_t> bounds, const color_t& color) {
	if (player.bone_list.empty())
		return;

	auto head_bone = player.bone_list[bone_index::head];

	Vec2_t head;
	if (!matrix.wts(head_bone.pos, io.DisplaySize, head))
		return;

	DrawTracker(d, head, bounds.second.x - bounds.first.x, color);
}

namespace {
	// One flag, ready to be placed around the box
	struct FlagItem {
		enum Kind { TEXT, ICON, BAR } kind = TEXT;
		std::string text;
		ImU32 color = IM_COL32_WHITE;
		float fraction = 0.f; // Bars
		std::string number;   // Bars, drawn where the fill ends

		int flag = 0, index = 0; // Which one & where in the layout
		float size = 12.f;       // Font size of texts & icons
	};

	ImU32 HealthColor(int health) {
		float t = std::clamp(health / 100.f, 0.f, 1.f);
		return ImColor(1.f - t * 0.6f, 0.35f + t * 0.65f, 0.35f, 1.f);
	}

	constexpr float BAR_THICKNESS = 3.f;
	constexpr float BAR_GAP = 3.f;
	// Texts telling what the player is doing, sized on their own
	bool IsStateFlag(int flag) {
		return flag == cfg::esp::FLAG_FLASHED_TEXT || flag == cfg::esp::FLAG_RELOADING_TEXT
			|| flag == cfg::esp::FLAG_SCOPED_TEXT || flag == cfg::esp::FLAG_DEFUSING;
	}
	constexpr float NUMBER_FONT_SIZE = 10.f;

	// Box bounds can come out flipped for players right next to or behind the camera, std::clamp asserts on those
	float ClampSafe(float value, float a, float b) {
		return std::max(std::min(a, b), std::min(value, std::max(a, b)));
	}
}

static bool BuildFlag(int flag, const Player& local, const Player& player, const cfg::esp::group_t& group, bool force, FlagItem& out) {
	ImU32 text = ImColor(group.text);
	ImU32 icon = ImColor(group.icon);

	switch (flag) {
	case cfg::esp::FLAG_NAME:
		out = { FlagItem::TEXT, std::format("{}{}", player.name, player.bot ? " (Bot)" : ""), text };
		return true;

	case cfg::esp::FLAG_HEALTH_BAR:
		out = { FlagItem::BAR, "", HealthColor(player.health), player.health / 100.f };
		if (group.health_number)
			out.number = std::to_string(player.health);
		return true;

	case cfg::esp::FLAG_ARMOR_BAR:
		if (player.armor <= 0 && !force)
			return false;
		out = { FlagItem::BAR, "", IM_COL32(120, 150, 255, 255), player.armor / 100.f };
		if (group.armor_number)
			out.number = std::to_string(player.armor);
		return true;

	case cfg::esp::FLAG_MONEY:
		out = { FlagItem::TEXT, std::format("${}", player.money), IM_COL32(140, 230, 120, 255) };
		return true;

	case cfg::esp::FLAG_PING:
		if (!player.ping && !force)
			return false;
		out = { FlagItem::TEXT, std::format("{}ms", player.ping), text };
		return true;

	case cfg::esp::FLAG_WEAPON:
		out = { FlagItem::ICON, player.weapon.icon ? player.weapon.icon : "", text };
		return !out.text.empty();

	case cfg::esp::FLAG_WEAPON_NAME:
		out = { FlagItem::TEXT, player.weapon.name, text };
		return !out.text.empty();

	case cfg::esp::FLAG_AMMO:
		if (player.ammo < 0)
			return false;
		out = { FlagItem::TEXT, std::to_string(player.ammo), text };
		return true;

	case cfg::esp::FLAG_DISTANCE: {
		auto delta = player.pos - local.pos;
		float meters = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) * 0.0254f;
		out = { FlagItem::TEXT, std::format("{:.0f}m", meters), text };
		return true;
	}

	case cfg::esp::FLAG_FLASHED:
		out = { FlagItem::ICON, Icons::BLIND, icon };
		return player.flashed || force;

	case cfg::esp::FLAG_RELOADING:
		out = { FlagItem::ICON, Icons::RELOAD, icon };
		return player.is_reloading || force;

	case cfg::esp::FLAG_DEFUSING:
		out = { FlagItem::TEXT, "DEFUSING", ImColor(group.defusing) };
		return player.defusing || force;

	case cfg::esp::FLAG_KIT:
		out = { FlagItem::ICON, WeaponIcons::CUTTERS, icon };
		return player.has_defuser || force;

	case cfg::esp::FLAG_SCOPED:
		out = { FlagItem::ICON, WeaponIcons::SCOPE, icon };
		return player.scoped || force;

	case cfg::esp::FLAG_FLASHED_TEXT:
		out = { FlagItem::TEXT, "FLASHED", ImColor(group.flashed) };
		return player.flashed || force;

	case cfg::esp::FLAG_RELOADING_TEXT:
		out = { FlagItem::TEXT, "RELOADING", ImColor(group.reloading) };
		return player.is_reloading || force;

	case cfg::esp::FLAG_SCOPED_TEXT:
		out = { FlagItem::TEXT, "SCOPED", ImColor(group.scoped) };
		return player.scoped || force;

	case cfg::esp::FLAG_C4: {
		if (!player.has_c4 && !force)
			return false;

		out = { FlagItem::ICON, WeaponIcons::C4, ImColor(1.f, 0.84f, 0.f, 1.f) };
		return true;
	}
	}

	return false;
}

void Esp::DrawFlagsImpl(ImDrawList* d, const Player& local, const Player& player, Vec2_t min, Vec2_t max,
	const cfg::esp::group_t& group, bool force, std::vector<FlagArea>* areas) {
	auto add_area = [&](const FlagItem& item, int side, Vec2_t from, Vec2_t to) {
		if (areas)
			areas->push_back({ item.flag, side, item.index, ImRect(from, to) });
	};

	auto measure = [&](const FlagItem& item) {
		auto font = item.kind == FlagItem::ICON ? this->font_merged_icons : this->font;
		return font->CalcTextSizeA(item.size, FLT_MAX, 0.f, item.text.c_str());
	};

	auto draw_text = [&](const FlagItem& item, Vec2_t pos) {
		bool is_icon = item.kind == FlagItem::ICON;
		auto font = is_icon ? this->font_merged_icons : this->font;
		float size = item.size;
		auto alpha = (item.color >> IM_COL32_A_SHIFT) & 0xFF;

		pos = Vec2_t(floorf(pos.x), floorf(pos.y));
		d->AddText(font, size, pos + Vec2_t(1, 1), IM_COL32(0, 0, 0, alpha * 3 / 4), item.text.c_str());
		d->AddText(font, size, pos, item.color, item.text.c_str());
	};

	// Filled from the bottom (vertical) or the left (horizontal)
	auto draw_bar = [&](const FlagItem& item, Vec2_t from, Vec2_t to, bool vertical) {
		float fraction = std::clamp(item.fraction, 0.f, 1.f);

		d->AddRectFilled(from - Vec2_t(1, 1), to + Vec2_t(1, 1), IM_COL32(0, 0, 0, 150));

		if (vertical)
			d->AddRectFilled(Vec2_t(from.x, to.y - (to.y - from.y) * fraction), to, item.color);
		else
			d->AddRectFilled(from, Vec2_t(from.x + (to.x - from.x) * fraction, to.y), item.color);

		if (item.number.empty())
			return;

		// On the bar where the fill ends, kept inside the length of the bar
		auto size = this->font->CalcTextSizeA(NUMBER_FONT_SIZE, FLT_MAX, 0.f, item.number.c_str());
		Vec2_t pos;

		if (vertical) {
			float y = to.y - (to.y - from.y) * fraction - size.y * 0.5f;
			pos = Vec2_t((from.x + to.x - size.x) * 0.5f, ClampSafe(y, from.y - size.y * 0.5f, to.y - size.y * 0.5f));
		}
		else {
			float x = from.x + (to.x - from.x) * fraction - size.x * 0.5f;
			pos = Vec2_t(ClampSafe(x, from.x - size.x * 0.5f, to.x - size.x * 0.5f), (from.y + to.y - size.y) * 0.5f);
		}

		pos = Vec2_t(floorf(pos.x), floorf(pos.y));
		for (const auto& offset : { Vec2_t(-1, 0), Vec2_t(1, 0), Vec2_t(0, -1), Vec2_t(0, 1) })
			d->AddText(this->font, NUMBER_FONT_SIZE, pos + offset, IM_COL32(0, 0, 0, 220), item.number.c_str());
		d->AddText(this->font, NUMBER_FONT_SIZE, pos, IM_COL32_WHITE, item.number.c_str());
	};

	for (int side = 0; side < cfg::esp::SIDE_COUNT; side++) {
		std::vector<FlagItem> bars, texts;
		const auto& layout = group.layout[side];

		for (int i = 0; i < static_cast<int>(layout.size()); i++) {
			FlagItem item;
			if (!BuildFlag(layout[i], local, player, group, force, item))
				continue;

			item.flag = layout[i];
			item.index = i;
			item.size = item.kind == FlagItem::ICON ? group.icon_size : IsStateFlag(item.flag) ? group.state_size : group.text_size;

			(item.kind == FlagItem::BAR ? bars : texts).push_back(std::move(item));
		}

		switch (side) {
		case cfg::esp::SIDE_TOP: {
			float y = min.y - BAR_GAP;

			for (const auto& bar : bars) {
				draw_bar(bar, Vec2_t(min.x, y - BAR_THICKNESS), Vec2_t(max.x, y), false);
				add_area(bar, side, Vec2_t(min.x, y - BAR_THICKNESS), Vec2_t(max.x, y));
				y -= BAR_THICKNESS + BAR_GAP;
			}

			for (const auto& text : texts) {
				auto size = measure(text);
				y -= size.y;
				Vec2_t pos((min.x + max.x - size.x) * 0.5f, y);
				draw_text(text, pos);
				add_area(text, side, pos, pos + Vec2_t(size.x, size.y));
			}
			break;
		}

		case cfg::esp::SIDE_BOTTOM: {
			float y = max.y + BAR_GAP;

			for (const auto& bar : bars) {
				draw_bar(bar, Vec2_t(min.x, y), Vec2_t(max.x, y + BAR_THICKNESS), false);
				add_area(bar, side, Vec2_t(min.x, y), Vec2_t(max.x, y + BAR_THICKNESS));
				y += BAR_THICKNESS + BAR_GAP;
			}

			for (const auto& text : texts) {
				auto size = measure(text);
				Vec2_t pos((min.x + max.x - size.x) * 0.5f, y);
				draw_text(text, pos);
				add_area(text, side, pos, pos + Vec2_t(size.x, size.y));
				y += size.y;
			}
			break;
		}

		case cfg::esp::SIDE_LEFT: {
			float x = min.x - BAR_GAP - 1.f;

			for (const auto& bar : bars) {
				draw_bar(bar, Vec2_t(x - BAR_THICKNESS, min.y), Vec2_t(x, max.y), true);
				add_area(bar, side, Vec2_t(x - BAR_THICKNESS, min.y), Vec2_t(x, max.y));
				x -= BAR_THICKNESS + BAR_GAP;
			}

			float y = min.y;
			for (const auto& text : texts) {
				auto size = measure(text);
				Vec2_t pos(x - size.x, y);
				draw_text(text, pos);
				add_area(text, side, pos, pos + Vec2_t(size.x, size.y));
				y += size.y;
			}
			break;
		}

		case cfg::esp::SIDE_RIGHT: {
			float x = max.x + BAR_GAP + 1.f;

			for (const auto& bar : bars) {
				draw_bar(bar, Vec2_t(x, min.y), Vec2_t(x + BAR_THICKNESS, max.y), true);
				add_area(bar, side, Vec2_t(x, min.y), Vec2_t(x + BAR_THICKNESS, max.y));
				x += BAR_THICKNESS + BAR_GAP;
			}

			float y = min.y;
			for (const auto& text : texts) {
				auto size = measure(text);
				Vec2_t pos(x, y);
				draw_text(text, pos);
				add_area(text, side, pos, pos + Vec2_t(size.x, size.y));
				y += size.y;
			}
			break;
		}
		}
	}
}

void Esp::RenderBombBox(Bomb bomb) {
	if (!cfg::esp::bomb)
		return;


	if (!bomb.is_planted)
		return;

	// Bomb dimensions
	float w = 10.f, l = 5.f, h = 10.f;
	Vec3_t half_size = { w / 2.f, h / 2.f, l / 2.f };

	Vec3_t corners[8] = {
		{ bomb.pos.x - half_size.x, bomb.pos.y - half_size.y, bomb.pos.z - half_size.z },
		{ bomb.pos.x + half_size.x, bomb.pos.y - half_size.y, bomb.pos.z - half_size.z },
		{ bomb.pos.x + half_size.x, bomb.pos.y - half_size.y, bomb.pos.z + half_size.z },
		{ bomb.pos.x - half_size.x, bomb.pos.y - half_size.y, bomb.pos.z + half_size.z },
		{ bomb.pos.x - half_size.x, bomb.pos.y + half_size.y, bomb.pos.z - half_size.z },
		{ bomb.pos.x + half_size.x, bomb.pos.y + half_size.y, bomb.pos.z - half_size.z },
		{ bomb.pos.x + half_size.x, bomb.pos.y + half_size.y, bomb.pos.z + half_size.z },
		{ bomb.pos.x - half_size.x, bomb.pos.y + half_size.y, bomb.pos.z + half_size.z },
	};

	Vec2_t projected[8];
	bool visible[8] = { false };
	int visible_count = 0;

	for (int i = 0; i < 8; ++i) {
		if (matrix.wts(corners[i], io.DisplaySize, projected[i])) {
			visible[i] = true;
			visible_count++;
		}
	}

	if (visible_count == 0)
		return;

	auto color = cfg::esp::colors::bomb;

	int edges[12][2] = {
		{ 0, 1 }, { 1, 2 }, { 2, 3 }, { 3, 0 }, // Bottom
		{ 4, 5 }, { 5, 6 }, { 6, 7 }, { 7, 4 }, // Top
		{ 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 }  // Verticals
	};

	for (auto& edge : edges) {
		int i = edge[0];
		int j = edge[1];
		if (visible[i] && visible[j]) {
			d->AddLine(projected[i], projected[j], ImColor(color), 1.0f);
		}
	}

	Vec2_t screen;
	if (!matrix.wts(bomb.pos + Vec3_t(0, 0, 8), io.DisplaySize, screen))
		return;

	ImGui::PushFont(this->font_merged_icons);
	d->AddText(
		this->font_merged_icons,
		16.0f,
		Vec2_t(
			screen.x - 8, // lazy
			screen.y
		),
		ImColor(255, 255, 255),
		WeaponIcons::C4
	);
	ImGui::PopFont();
}

namespace {
	ImU32 WithAlpha(ImU32 color, float alpha);
}

void Esp::RenderItems(const std::vector<Item>& items, const Vec3_t& viewer) {
	namespace it = cfg::esp::items;

	if (!it::enabled)
		return;

	for (const auto& item : items) {
		const it::category_t* category = &it::weapons;
		switch (item.kind) {
		case ItemKind::Utility:	category = &it::utility; break;
		case ItemKind::Bomb:	category = &it::bomb; break;
		case ItemKind::Kit:		category = &it::kits; break;
		default: break;
		}

		if (!category->enabled)
			continue;

		auto delta = item.pos - viewer;
		float meters = sqrtf(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) * 0.0254f;
		if (meters > it::max_distance)
			continue;

		Vec2_t screen;
		if (!matrix.wts(item.pos, io.DisplaySize, screen))
			continue;

		// Fainter the further away
		float alpha = 1.f - 0.55f * std::clamp(meters / it::max_distance, 0.f, 1.f);
		ImU32 color = WithAlpha(ImColor(category->color), alpha);
		ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(200 * alpha));

		float below = screen.y + 1.f;

		bool has_icon = category->icon && item.icon && *item.icon && strcmp(item.icon, "?") != 0;
		if (has_icon) {
			auto size = font_merged_icons->CalcTextSizeA(it::icon_size, FLT_MAX, 0.f, item.icon);
			Vec2_t pos(floorf(screen.x - size.x * 0.5f), floorf(screen.y - size.y));

			d->AddText(font_merged_icons, it::icon_size, pos + Vec2_t(1.f, 1.f), shadow, item.icon);
			d->AddText(font_merged_icons, it::icon_size, pos, color, item.icon);
		}

		// Name, ammo & distance on one line under it
		std::string label;
		auto add = [&](const std::string& part) {
			if (!label.empty())
				label += " ";
			label += part;
		};

		if (category->name || !has_icon)
			add(item.name);
		if (category->ammo && item.kind == ItemKind::Weapon && item.ammo >= 0)
			add(std::format("[{}]", item.ammo));
		if (category->distance)
			add(std::format("{:.0f}m", meters));

		if (label.empty())
			continue;

		auto size = font->CalcTextSizeA(it::text_size, FLT_MAX, 0.f, label.c_str());
		Vec2_t pos(floorf(screen.x - size.x * 0.5f), floorf(below));

		d->AddText(font, it::text_size, pos + Vec2_t(1.f, 1.f), shadow, label.c_str());
		d->AddText(font, it::text_size, pos, WithAlpha(IM_COL32_WHITE, alpha), label.c_str());
	}
}

void Esp::RenderCrosshair(Player local)
{
	if (!cfg::world::crosshair::enabled)
		return;

	if (local.scoped)
		return;

	auto weapon = local.weapon;

	if (weapon.item_index == -1)
		return;

	static std::vector<WeaponIds> valid_weapons = { weapon_ssg08, weapon_awp, weapon_g3sg1, weapon_scar20 };

	if (std::find(valid_weapons.begin(), valid_weapons.end(), weapon.item_index) == valid_weapons.end())
		return;

	ImVec2 center(
		floorf(io.DisplaySize.x * 0.5f),
		floorf(io.DisplaySize.y * 0.5f));

	// The one set in the game, so it looks like every other weapon
	if (cfg::world::crosshair::style == cfg::world::crosshair::STYLE_GAME) {
		GameCrosshair::Get().Draw(d, center, io.DisplaySize.y);
		return;
	}

	constexpr float size = 6.f;
	constexpr float thickness = 1.0f;

	d->AddLine(
		ImVec2(center.x - size, center.y),
		ImVec2(center.x + size + 1, center.y),
		IM_COL32(255, 255, 255, 255),

		thickness);
	d->AddLine(
		ImVec2(center.x, center.y - size),
		ImVec2(center.x, center.y + size + 1),
		IM_COL32(255, 255, 255, 255),
		thickness);
}

void Esp::RenderPlayerTracers(const Player& source, const Player& player, const cfg::esp::group_t& group, bool visible) {
	if (!group.tracers)
		return;

	Vec2_t screenPos;
	bool projected = matrix.wts(player.pos, io.DisplaySize, screenPos, false);

	if (!projected)
	{
		Vec3_t camPos = source.pos;
		Vec3_t dir = player.pos - camPos;

		// projection for off screen players
		Vec3_t viewDir;
		viewDir.x = matrix[0][0] * dir.x + matrix[0][1] * dir.y + matrix[0][2] * dir.z;
		viewDir.y = matrix[1][0] * dir.x + matrix[1][1] * dir.y + matrix[1][2] * dir.z;
		viewDir.z = matrix[2][0] * dir.x + matrix[2][1] * dir.y + matrix[2][2] * dir.z;

		if (viewDir.z > 0.0f)
		{
			viewDir.x = -viewDir.x;
			viewDir.y = -viewDir.y;
		}

		// normalize
		float len = sqrt(viewDir.x * viewDir.x + viewDir.y * viewDir.y);
		if (len > 0.001f)
		{
			viewDir.x /= len;
			viewDir.y /= len;
		}

		screenPos.x = io.DisplaySize.x * 0.5f + viewDir.x * io.DisplaySize.x * 0.5f;
		screenPos.y = io.DisplaySize.y * 0.5f - viewDir.y * io.DisplaySize.y * 0.5f;

		float margin = 10.f;
		screenPos.x = std::clamp(screenPos.x, margin, io.DisplaySize.x - margin);
		screenPos.y = std::clamp(screenPos.y, margin, io.DisplaySize.y - margin);
	}

	auto color = visible ? group.tracer_visible : group.tracer_invisible;

	d->AddLine(
		Vec2_t(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
		screenPos,
		ImColor(color),
		1.0f
	);
}

namespace {
	struct GrenadeStyle {
		const char* name;
		const char* icon;
		ImU32 color;
	};

	GrenadeStyle GetGrenadeStyle(GrenadeType type) {
		namespace colors = cfg::esp::colors::grenades;

		switch (type) {
		case GrenadeType::Smoke:	return { "Smoke", WeaponIcons::SMOKEGRENADE, ImColor(colors::smoke) };
		case GrenadeType::Molotov:	return { "Molotov", WeaponIcons::MOLOTOV, ImColor(colors::molotov) };
		case GrenadeType::Flash:	return { "Flash", WeaponIcons::FLASHBANG, ImColor(colors::flash) };
		case GrenadeType::HE:		return { "HE", WeaponIcons::FRAG_GRENADE, ImColor(colors::he) };
		case GrenadeType::Decoy:	return { "Decoy", WeaponIcons::DECOY, ImColor(colors::decoy) };
		case GrenadeType::Fire:		return { "Fire", WeaponIcons::MOLOTOV, ImColor(colors::molotov) };
		}
		return { "Grenade", WeaponIcons::unknown, IM_COL32(255, 255, 255, 255) };
	}

	ImU32 WithAlpha(ImU32 color, float alpha) {
		auto a = static_cast<int>(((color >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
		return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a, 0, 255)) << IM_COL32_A_SHIFT);
	}

	constexpr float SMOKE_RADIUS = 144.f;
}

void Esp::RenderGroundCircle(const Vec3_t& center, float radius, ImU32 color, int segments, bool filled, bool glow) {
	std::vector<ImVec2> points;
	points.reserve(segments);

	// Ring on the ground, false when part of it is behind the camera
	auto project = [&](float r) {
		points.clear();
		bool complete = true;

		for (int i = 0; i < segments; i++) {
			float angle = (std::numbers::pi_v<float> * 2.f) * i / segments;
			Vec3_t point(center.x + cosf(angle) * r, center.y + sinf(angle) * r, center.z);

			Vec2_t screen;
			if (!matrix.wts(point, io.DisplaySize, screen, false)) {
				complete = false;
				continue;
			}

			points.push_back(screen);
		}

		return complete && points.size() >= 3;
	};

	// Points behind the camera break the shape, only drawn when it is fully in front
	if (!project(radius))
		return;

	if (glow) {
		// Soft light getting brighter towards the middle, then a halo around the edge
		auto flags = d->Flags;
		d->Flags &= ~ImDrawListFlags_AntiAliasedFill;

		for (int ring = 1; ring <= 5; ring++) {
			if (project(radius * (1.f - ring * 0.17f)))
				d->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), WithAlpha(color, 0.07f));
		}

		d->Flags = flags;
		project(radius);

		d->AddPolyline(points.data(), static_cast<int>(points.size()), WithAlpha(color, 0.08f), ImDrawFlags_Closed, 9.f);
		d->AddPolyline(points.data(), static_cast<int>(points.size()), WithAlpha(color, 0.18f), ImDrawFlags_Closed, 4.5f);
	}

	if (filled)
		d->AddConvexPolyFilled(points.data(), static_cast<int>(points.size()), WithAlpha(color, glow ? 0.1f : 0.12f));

	d->AddPolyline(points.data(), static_cast<int>(points.size()), WithAlpha(color, glow ? 0.95f : 0.8f), ImDrawFlags_Closed, 1.5f);
}

void Esp::RenderArea(const AreaShape& area, ImU32 color, bool glow) {
	if (!area.Valid())
		return;

	int min_x = INT_MAX, min_y = INT_MAX, max_x = INT_MIN, max_y = INT_MIN;

	for (const auto& cell : area.cells) {
		min_x = std::min(min_x, cell.x);
		min_y = std::min(min_y, cell.y);
		max_x = std::max(max_x, cell.x);
		max_y = std::max(max_y, cell.y);
	}

	// The cells in a grid with a free row & column around, found by place instead of hashed. Kept between
	// calls, so drawing allocates nothing
	size_t width = static_cast<size_t>(max_x - min_x) + 3, height = static_cast<size_t>(max_y - min_y) + 3;
	if (width * height > (1u << 20))
		return;

	static std::vector<const AreaShape::Cell*> grid;
	static std::vector<uint8_t> projected;	// 0 not yet, 1 on screen, 2 behind
	static std::vector<Vec2_t> centers;
	grid.assign(width * height, nullptr);
	projected.assign(width * height, 0);
	centers.resize(width * height);

	auto index = [&](int x, int y) { return static_cast<size_t>(y - min_y + 1) * width + static_cast<size_t>(x - min_x + 1); };
	for (const auto& cell : area.cells)
		grid[index(cell.x, cell.y)] = &cell;

	struct Vertex {
		float x, y;     // World
		float floor;
		bool boundary;  // On the edge of the area
		size_t cell;    // Of a cell center, projected once for the squares sharing it
	};

	constexpr size_t NO_CELL = SIZE_MAX;

	auto project = [&](const Vertex& vertex, Vec2_t& out) {
		if (vertex.cell == NO_CELL)
			return matrix.wts(Vec3_t(vertex.x, vertex.y, vertex.floor), io.DisplaySize, out, false);

		auto& state = projected[vertex.cell];
		if (!state)
			state = matrix.wts(Vec3_t(vertex.x, vertex.y, vertex.floor), io.DisplaySize, centers[vertex.cell], false) ? 1 : 2;
		out = centers[vertex.cell];
		return state == 1;
	};

	auto fill = WithAlpha(color, glow ? 0.17f : 0.12f);
	auto outline = WithAlpha(color, glow ? 0.95f : 0.8f);

	static std::vector<Vertex> polygon;
	static std::vector<ImVec2> screen;
	static std::vector<uint8_t> on_screen;
	static std::vector<std::pair<Vec2_t, Vec2_t>> lines;
	lines.clear();

	// Neighbouring squares share edges, anti aliasing would show the seams
	auto flags = d->Flags;
	d->Flags &= ~ImDrawListFlags_AntiAliasedFill;

	// Marching squares between cell centers, smoothing the grid into an outline
	constexpr int corner_x[4] = { 0, 1, 1, 0 };
	constexpr int corner_y[4] = { 0, 0, 1, 1 };

	for (int y = min_y - 1; y <= max_y; y++) {
		for (int x = min_x - 1; x <= max_x; x++) {
			const AreaShape::Cell* corners[4];
			bool any = false;

			for (int i = 0; i < 4; i++) {
				corners[i] = grid[index(x + corner_x[i], y + corner_y[i])];
				any |= corners[i] != nullptr;
			}

			if (!any)
				continue;

			// Inside corners plus the middle of the edges leaving the area, in order around the square
			polygon.clear();

			for (int i = 0; i < 4; i++) {
				const auto* corner = corners[i];
				const auto* next = corners[(i + 1) % 4];

				float corner_wx = area.origin_x + (x + corner_x[i]) * area.cell_size;
				float corner_wy = area.origin_y + (y + corner_y[i]) * area.cell_size;

				if (corner)
					polygon.push_back({ corner_wx, corner_wy, corner->floor, false, index(x + corner_x[i], y + corner_y[i]) });

				if ((corner != nullptr) != (next != nullptr)) {
					const auto* inside = corner ? corner : next;
					float next_wx = area.origin_x + (x + corner_x[(i + 1) % 4]) * area.cell_size;
					float next_wy = area.origin_y + (y + corner_y[(i + 1) % 4]) * area.cell_size;

					polygon.push_back({ (corner_wx + next_wx) * 0.5f, (corner_wy + next_wy) * 0.5f, inside->floor, true, NO_CELL });
				}
			}

			screen.resize(polygon.size());
			on_screen.resize(polygon.size());
			bool all = true;
			for (size_t i = 0; i < polygon.size(); i++) {
				Vec2_t point;
				on_screen[i] = project(polygon[i], point);
				screen[i] = point;
				all &= on_screen[i] != 0;
			}

			if (all && screen.size() >= 3)
				d->AddConvexPolyFilled(screen.data(), static_cast<int>(screen.size()), fill);

			// The outline: from one edge middle to the next, already projected
			for (size_t i = 0; i < polygon.size(); i++) {
				size_t next = (i + 1) % polygon.size();
				if (polygon[i].boundary && polygon[next].boundary && on_screen[i] && on_screen[next])
					lines.emplace_back(Vec2_t(screen[i].x, screen[i].y), Vec2_t(screen[next].x, screen[next].y));
			}
		}
	}

	d->Flags = flags;

	// Outline on the ground, with a halo around it when glowing
	if (glow) {
		for (const auto& [from, to] : lines)
			d->AddLine(from, to, WithAlpha(color, 0.07f), 10.f);
		for (const auto& [from, to] : lines)
			d->AddLine(from, to, WithAlpha(color, 0.16f), 5.f);
	}

	for (const auto& [from, to] : lines)
		d->AddLine(from, to, outline, 1.5f);
}

void Esp::RenderGrenades(const std::vector<Grenade>& grenades) {
	if (!cfg::esp::grenades::enabled)
		return;

	for (const auto& grenade : grenades) {
		auto style = GetGrenadeStyle(grenade.type);

		// Flight path, fading in towards the grenade
		if (cfg::esp::grenades::trails && !grenade.detonated && grenade.trail.size() > 1) {
			auto count = grenade.trail.size();
			auto trail_color = cfg::esp::grenades::trail_type_color
				? style.color
				: static_cast<ImU32>(ImColor(cfg::esp::colors::grenades::trail));

			for (size_t i = 1; i < count; i++) {
				Vec2_t from, to;
				if (!matrix.wts(grenade.trail[i - 1], io.DisplaySize, from, false) ||
					!matrix.wts(grenade.trail[i], io.DisplaySize, to, false))
					continue;

				d->AddLine(from, to, WithAlpha(trail_color, 0.2f + 0.8f * i / count), 1.5f);
			}
		}

		if (cfg::esp::grenades::landing && grenade.landing.valid)
			RenderGrenadeLanding(grenade, style.color);

		// Area of effect, glowing once it popped. Smokes & fires each have their own switch
		bool glow = cfg::esp::grenades::glow && grenade.detonated;
		bool area = ShowsArea(grenade.type);

		if (area && grenade.area.Valid())
			RenderArea(grenade.area, style.color, glow);
		else if (area && grenade.type == GrenadeType::Smoke && grenade.detonated)
			RenderGroundCircle(grenade.pos, SMOKE_RADIUS, style.color, 40, true, glow);

		// Label: icon, then the name & time left under it
		Vec2_t screen;
		if (grenade.pos.zero() || !matrix.wts(grenade.pos, io.DisplaySize, screen))
			continue;

		if (cfg::esp::grenades::icons) {
			auto icon_size = font_merged_icons->CalcTextSizeA(16.f, FLT_MAX, 0.f, style.icon);
			d->AddText(font_merged_icons, 16.f, screen - Vec2_t(icon_size.x * 0.5f, icon_size.y), style.color, style.icon);
		}

		std::string label = cfg::esp::grenades::names ? style.name : "";
		bool has_timer = cfg::esp::grenades::timers && grenade.time_left >= 0.f && grenade.duration > 0.f;

		if (has_timer)
			label += std::format("{}{:.1f}s", label.empty() ? "" : " ", grenade.time_left);

		float text_size = cfg::esp::grenades::text_size;
		float below = screen.y + 2.f;

		if (!label.empty()) {
			auto label_size = font->CalcTextSizeA(text_size, FLT_MAX, 0.f, label.c_str());
			auto label_pos = Vec2_t(floorf(screen.x - label_size.x * 0.5f), floorf(below));

			d->AddText(font, text_size, label_pos + Vec2_t(1.f, 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
			d->AddText(font, text_size, label_pos, IM_COL32(255, 255, 255, 255), label.c_str());
			below += label_size.y + 2.f;
		}

		if (has_timer && cfg::esp::grenades::timer_bars) {
			float bar_width = std::max(40.f, text_size * 3.2f);
			auto bar_pos = Vec2_t(screen.x - bar_width * 0.5f, below);
			float progress = std::clamp(grenade.time_left / grenade.duration, 0.f, 1.f);

			d->AddRectFilled(bar_pos, bar_pos + Vec2_t(bar_width, 3.f), IM_COL32(0, 0, 0, 160), 1.5f);
			d->AddRectFilled(bar_pos, bar_pos + Vec2_t(bar_width * progress, 3.f), style.color, 1.5f);
		}
	}
}

void Esp::RenderGrenadePrediction(const GrenadePath& path) {
	if (!cfg::esp::grenades::enabled || !cfg::esp::grenades::prediction || !path.valid || path.points.size() < 2)
		return;

	auto style = GetGrenadeStyle(path.type);

	// Path, skipping the segments behind the camera
	for (size_t i = 1; i < path.points.size(); i++) {
		Vec2_t from, to;
		if (!matrix.wts(path.points[i - 1], io.DisplaySize, from, false) ||
			!matrix.wts(path.points[i], io.DisplaySize, to, false))
			continue;

		d->AddLine(from, to, IM_COL32(0, 0, 0, 120), 3.f);
		d->AddLine(from, to, style.color, 1.5f);
	}

	// Bounces
	for (const auto& bounce : path.bounces) {
		Vec2_t screen;
		if (!matrix.wts(bounce, io.DisplaySize, screen))
			continue;

		d->AddCircleFilled(screen, 3.5f, IM_COL32(0, 0, 0, 160));
		d->AddCircleFilled(screen, 2.5f, IM_COL32(255, 255, 255, 230));
	}

	RenderPathEnd(path, style.color, style.icon, path.time);
}

bool Esp::ShowsArea(GrenadeType type) {
	switch (type) {
	case GrenadeType::Smoke:	return cfg::esp::grenades::smoke_area;
	case GrenadeType::Fire:
	case GrenadeType::Molotov:	return cfg::esp::grenades::fire_area;
	default:					return true;
	}
}

void Esp::RenderPathEnd(const GrenadePath& path, ImU32 color, const char* icon, float time) {
	// Where it ends, with the area for smokes & fires: their switch off, before landing too
	if (path.area.Valid()) {
		if (ShowsArea(path.type))
			RenderArea(path.area, color);
	}
	else if (path.on_ground)
		RenderGroundCircle(path.end, 16.f, color, 16, true);

	Vec2_t screen;
	if (!matrix.wts(path.end, io.DisplaySize, screen))
		return;

	auto icon_size = font_merged_icons->CalcTextSizeA(16.f, FLT_MAX, 0.f, icon);
	d->AddText(font_merged_icons, 16.f, screen - Vec2_t(icon_size.x * 0.5f, icon_size.y + 4.f), color, icon);

	if (time > 0.f) {
		auto label = std::format("{:.1f}s", time);
		auto label_size = ImGui::CalcTextSize(label.c_str());
		auto label_pos = screen + Vec2_t(-label_size.x * 0.5f, 2.f);

		d->AddText(label_pos + Vec2_t(1.f, 1.f), IM_COL32(0, 0, 0, 200), label.c_str());
		d->AddText(label_pos, IM_COL32(255, 255, 255, 255), label.c_str());
	}
}

void Esp::RenderGrenadeLanding(const Grenade& grenade, ImU32 color) {
	const auto& points = grenade.landing.points;

	// Continue from where the grenade is right now
	size_t closest = grenade.landing_from;

	auto faded = WithAlpha(color, 0.6f);

	for (size_t i = closest + 1; i < points.size(); i++) {
		Vec2_t from, to;
		if (!matrix.wts(points[i - 1], io.DisplaySize, from, false) ||
			!matrix.wts(points[i], io.DisplaySize, to, false))
			continue;

		// Dashed, so it reads as a prediction next to the real trail
		if ((i / 3) % 2 == 0)
			d->AddLine(from, to, faded, 1.5f);
	}

	RenderPathEnd(grenade.landing, color, GetGrenadeStyle(grenade.type).icon, grenade.landing_time_left);
}
