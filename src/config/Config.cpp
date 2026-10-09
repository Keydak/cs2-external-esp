#include "Config.hpp"

bool Config::Read() {
	return GetInstance().ReadImpl();
}

bool Config::Write() {
	return GetInstance().WriteImpl();
}

namespace {
	const char* SIDE_KEYS[] = { "top", "bottom", "left", "right" };
}

void Config::ReadGroup(const json& from, cfg::esp::group_t& group, const cfg::esp::group_t& def) {
	group = def;

	if (!from.is_object())
		return;

	group.enabled = from.value("enabled", def.enabled);
	group.box = from.value("box", def.box);
	group.skeleton = from.value("skeleton", def.skeleton);
	group.head_tracker = from.value("head_tracker", def.head_tracker);
	group.tracers = from.value("tracers", def.tracers);
	group.visible_only = from.value("visible_only", def.visible_only);

	group.box_visible = JsonToColor(from, "box_visible", def.box_visible);
	group.box_invisible = JsonToColor(from, "box_invisible", def.box_invisible);
	group.skeleton_visible = JsonToColor(from, "skeleton_visible", def.skeleton_visible);
	group.skeleton_invisible = JsonToColor(from, "skeleton_invisible", def.skeleton_invisible);
	group.tracker_visible = JsonToColor(from, "tracker_visible", def.tracker_visible);
	group.tracker_invisible = JsonToColor(from, "tracker_invisible", def.tracker_invisible);
	group.tracer_visible = JsonToColor(from, "tracer_visible", def.tracer_visible);
	group.tracer_invisible = JsonToColor(from, "tracer_invisible", def.tracer_invisible);
	group.text = JsonToColor(from, "text", def.text);
	group.icon = JsonToColor(from, "icon", def.icon);
	group.flashed = JsonToColor(from, "flashed", def.flashed);
	group.reloading = JsonToColor(from, "reloading", def.reloading);
	group.scoped = JsonToColor(from, "scoped", def.scoped);
	group.defusing = JsonToColor(from, "defusing", def.defusing);
	group.text_size = std::clamp(from.value("text_size", def.text_size), 8.f, 24.f);
	group.state_size = std::clamp(from.value("state_size", def.state_size), 8.f, 24.f);
	group.icon_size = std::clamp(from.value("icon_size", def.icon_size), 8.f, 28.f);
	group.health_number = from.value("health_number", def.health_number);
	group.armor_number = from.value("armor_number", def.armor_number);

	if (auto layout = from.find("layout"); layout != from.end() && layout->is_object()) {
		bool used[cfg::esp::FLAG_COUNT]{};

		for (int side = 0; side < cfg::esp::SIDE_COUNT; side++) {
			group.layout[side].clear();

			auto flags = layout->find(SIDE_KEYS[side]);
			if (flags == layout->end() || !flags->is_array())
				continue;

			for (const auto& flag : *flags) {
				if (!flag.is_number_integer())
					continue;

				int value = flag.get<int>();
				if (value < 0 || value >= cfg::esp::FLAG_COUNT || used[value])
					continue;

				// The health text became the number on the health bar
				if (value == cfg::esp::FLAG_HEALTH) {
					if (!from.contains("health_number"))
						group.health_number = true;
					continue;
				}

				used[value] = true;
				group.layout[side].push_back(value);
			}
		}
	}
}

namespace {
	void ReadCategory(const json& from, cfg::esp::items::category_t& category) {
		if (!from.is_object())
			return;

		category.enabled = from.value("enabled", category.enabled);
		category.icon = from.value("icon", category.icon);
		category.name = from.value("name", category.name);
		category.ammo = from.value("ammo", category.ammo);
		category.distance = from.value("distance", category.distance);
		category.color = Config::JsonToColor(from, "color", category.color);
	}

	void WriteCategory(json& to, const cfg::esp::items::category_t& category) {
		to["enabled"] = category.enabled;
		to["icon"] = category.icon;
		to["name"] = category.name;
		to["ammo"] = category.ammo;
		to["distance"] = category.distance;
		Config::ColorToJson(to, "color", category.color);
	}
}

void Config::WriteGroup(json& to, const cfg::esp::group_t& group) {
	to["enabled"] = group.enabled;
	to["box"] = group.box;
	to["skeleton"] = group.skeleton;
	to["head_tracker"] = group.head_tracker;
	to["tracers"] = group.tracers;
	to["visible_only"] = group.visible_only;

	ColorToJson(to, "box_visible", group.box_visible);
	ColorToJson(to, "box_invisible", group.box_invisible);
	ColorToJson(to, "skeleton_visible", group.skeleton_visible);
	ColorToJson(to, "skeleton_invisible", group.skeleton_invisible);
	ColorToJson(to, "tracker_visible", group.tracker_visible);
	ColorToJson(to, "tracker_invisible", group.tracker_invisible);
	ColorToJson(to, "tracer_visible", group.tracer_visible);
	ColorToJson(to, "tracer_invisible", group.tracer_invisible);
	ColorToJson(to, "text", group.text);
	ColorToJson(to, "icon", group.icon);
	ColorToJson(to, "flashed", group.flashed);
	ColorToJson(to, "reloading", group.reloading);
	ColorToJson(to, "scoped", group.scoped);
	ColorToJson(to, "defusing", group.defusing);
	to["text_size"] = group.text_size;
	to["state_size"] = group.state_size;
	to["icon_size"] = group.icon_size;
	to["health_number"] = group.health_number;
	to["armor_number"] = group.armor_number;

	for (int side = 0; side < cfg::esp::SIDE_COUNT; side++)
		to["layout"][SIDE_KEYS[side]] = group.layout[side];
}

void Config::ApplySettings(json data) {
	// general
	cfg::enabled = data.value("enabled", true);

	// esp
	const auto& esp = data["esp"];
	cfg::esp::bomb = esp.value("bomb", true);

	if (auto items = esp.value("items", json::object()); items.is_object()) {
		namespace it = cfg::esp::items;
		it::enabled = items.value("enabled", it::enabled);
		it::max_distance = std::clamp(items.value("max_distance", it::max_distance), 5.f, 200.f);
		it::text_size = std::clamp(items.value("text_size", it::text_size), 8.f, 24.f);
		it::icon_size = std::clamp(items.value("icon_size", it::icon_size), 8.f, 28.f);
		ReadCategory(items.value("weapons", json::object()), it::weapons);
		ReadCategory(items.value("utility", json::object()), it::utility);
		ReadCategory(items.value("bomb", json::object()), it::bomb);
		ReadCategory(items.value("kits", json::object()), it::kits);
	}

	if (esp.contains("enemy") || esp.contains("team")) {
		ReadGroup(esp.value("enemy", json::object()), cfg::esp::enemy, cfg::esp::MakeEnemy());
		ReadGroup(esp.value("team", json::object()), cfg::esp::team, cfg::esp::MakeTeam());
	}
	else if (!esp.empty()) {
		// Older configs, one set of toggles for both
		for (auto* group : { &cfg::esp::enemy, &cfg::esp::team }) {
			group->box = esp.value("box", true);
			group->skeleton = esp.value("skeleton", true);
			group->head_tracker = esp.value("head_tracker", true);
			group->tracers = esp.value("tracers", false);

			const auto flags = esp.value("flags", json::object());
			auto& layout = group->layout;
			for (auto& side : layout)
				side.clear();

			if (flags.value("name", true)) layout[cfg::esp::SIDE_TOP].push_back(cfg::esp::FLAG_NAME);
			if (esp.value("health", true)) layout[cfg::esp::SIDE_LEFT].push_back(cfg::esp::FLAG_HEALTH_BAR);
			if (esp.value("armor", true)) layout[cfg::esp::SIDE_BOTTOM].push_back(cfg::esp::FLAG_ARMOR_BAR);
			if (flags.value("weapon", false)) layout[cfg::esp::SIDE_BOTTOM].push_back(cfg::esp::FLAG_WEAPON);
			if (flags.value("ammo", false)) layout[cfg::esp::SIDE_BOTTOM].push_back(cfg::esp::FLAG_AMMO);
			group->health_number = esp.value("health_number", false);
			if (flags.value("money", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_MONEY);
			if (flags.value("ping", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_PING);
			if (flags.value("flashed", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_FLASHED);
			if (flags.value("reloading", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_RELOADING);
			if (flags.value("defusing", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_DEFUSING);
			if (flags.value("scoped", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_SCOPED);
			if (flags.value("has_c4", false)) layout[cfg::esp::SIDE_RIGHT].push_back(cfg::esp::FLAG_C4);
		}

		cfg::esp::team.enabled = esp.value("team", true);
	}

	if (esp.contains("grenades")) {
		cfg::esp::grenades::enabled = esp["grenades"].value("enabled", true);
		cfg::esp::grenades::trails = esp["grenades"].value("trails", true);
		cfg::esp::grenades::trail_type_color = esp["grenades"].value("trail_type_color", true);
		cfg::esp::grenades::timers = esp["grenades"].value("timers", true);
		cfg::esp::grenades::prediction = esp["grenades"].value("prediction", true);
		cfg::esp::grenades::landing = esp["grenades"].value("landing", true);
		cfg::esp::grenades::smoke_area = esp["grenades"].value("smoke_area", true);
		cfg::esp::grenades::fire_area = esp["grenades"].value("fire_area", true);
		cfg::esp::grenades::glow = esp["grenades"].value("glow", true);
		cfg::esp::grenades::icons = esp["grenades"].value("icons", true);
		cfg::esp::grenades::names = esp["grenades"].value("names", false);
		cfg::esp::grenades::timer_bars = esp["grenades"].value("timer_bars", true);
		cfg::esp::grenades::text_size = std::clamp(esp["grenades"].value("text_size", 13.f), 8.f, 32.f);
	}

	// colors
	const auto& col = data["esp"]["colors"];
	cfg::esp::colors::bomb = JsonToColor(col, "bomb", { 1.f, 0.84f, 0.f, 1.f });

	// grenade colors
	const auto& gcol = data["esp"]["colors"]["grenades"];
	cfg::esp::colors::grenades::smoke = JsonToColor(gcol, "smoke", cfg::esp::colors::grenades::smoke);
	cfg::esp::colors::grenades::molotov = JsonToColor(gcol, "molotov", cfg::esp::colors::grenades::molotov);
	cfg::esp::colors::grenades::flash = JsonToColor(gcol, "flash", cfg::esp::colors::grenades::flash);
	cfg::esp::colors::grenades::he = JsonToColor(gcol, "he", cfg::esp::colors::grenades::he);
	cfg::esp::colors::grenades::decoy = JsonToColor(gcol, "decoy", cfg::esp::colors::grenades::decoy);
	cfg::esp::colors::grenades::trail = JsonToColor(gcol, "trail", cfg::esp::colors::grenades::trail);

	// world
	// spectator list
	cfg::world::spectators::enabled = data["world"]["spectators"].value("enabled", true);
	cfg::world::spectators::detailed = data["world"]["spectators"].value("detailed", false);
	cfg::world::spectators::self_only = data["world"]["spectators"].value("self_only", true);
	cfg::world::spectators::pos = JsonToVec2(data["world"]["spectators"], "pos", {10.f, 100.f});

	// New, missing in configs made before it: value() of the null that [] makes throws & stops the whole load
	cfg::world::keybinds::enabled = true;
	cfg::world::keybinds::pos = { 10.f, 250.f };
	cfg::world::keybinds::hide_bhop = false;
	cfg::world::keybinds::hide_air_strafe = false;
	cfg::world::keybinds::hide_jump_bug = false;
	if (data["world"].contains("keybinds") && data["world"]["keybinds"].is_object()) {
		cfg::world::keybinds::enabled = data["world"]["keybinds"].value("enabled", true);
		cfg::world::keybinds::pos = JsonToVec2(data["world"]["keybinds"], "pos", { 10.f, 250.f });
		cfg::world::keybinds::hide_bhop = data["world"]["keybinds"].value("hide_bhop", false);
		cfg::world::keybinds::hide_air_strafe = data["world"]["keybinds"].value("hide_air_strafe", false);
		cfg::world::keybinds::hide_jump_bug = data["world"]["keybinds"].value("hide_jump_bug", false);
	}

	cfg::world::votes::enabled = true;
	cfg::world::votes::names = false;
	cfg::world::votes::pos = { 10.f, 400.f };
	if (data["world"].contains("votes") && data["world"]["votes"].is_object()) {
		cfg::world::votes::enabled = data["world"]["votes"].value("enabled", true);
		cfg::world::votes::pos = JsonToVec2(data["world"]["votes"], "pos", { 10.f, 400.f });
	}

	// bomb
	cfg::world::bomb::location = data["world"]["bomb"].value("location", true);
	cfg::world::bomb::timer = data["world"]["bomb"].value("timer", true);
	cfg::world::bomb::pos = JsonToVec2(data["world"]["bomb"], "pos", { 10.f, 300.f });

	// hitmarker
	{
		namespace hm = cfg::world::hitmarker;
		const auto& from = data["world"].contains("hitmarker") && data["world"]["hitmarker"].is_object() ? data["world"]["hitmarker"] : json::object();
		hm::crosshair = from.value("crosshair", false);
		hm::world = from.value("world", false);
		hm::damage = from.value("damage", true);
		hm::duration = from.value("duration", 0.5f);
		hm::size = from.value("size", 6.f);
		hm::color = JsonToColor(from, "color", color_t(1.f, 1.f, 1.f, 1.f));
		hm::kill_color = JsonToColor(from, "kill_color", color_t(1.f, 0.27f, 0.27f, 1.f));
	}

	// crosshair
	cfg::world::crosshair::enabled = data["world"]["crosshair"].value("enabled", false);
	cfg::world::crosshair::style = data["world"]["crosshair"].value("style", static_cast<int>(cfg::world::crosshair::STYLE_CLASSIC));

	// radar
	cfg::world::radar::enabled = data["world"]["radar"].value("enabled", true);
	cfg::world::radar::no_rotate = data["world"]["radar"].value("no_rotate", false);
	cfg::world::radar::mode = data["world"]["radar"].value("mode", 0);
	cfg::world::radar::range = data["world"]["radar"].value("range", 2000.f);
	cfg::world::radar::pos = JsonToVec2(data["world"]["radar"], "pos", { 10.f, 10.f });
	cfg::world::radar::size = JsonToVec2(data["world"]["radar"], "size", { 200.f, 200.f });

	// velocity
	cfg::world::velocity::enabled = data["world"]["velocity"].value("enabled", false);
	cfg::world::velocity::sample_rate = data["world"]["velocity"].value("sample_rate", 10);
	cfg::world::velocity::sample_length = data["world"]["velocity"].value("sample_length", 5.f);
	cfg::world::velocity::pos = JsonToVec2(data["world"]["velocity"], "pos", { 10.f, 400.f });
	cfg::world::velocity::size = JsonToVec2(data["world"]["velocity"], "size", { 400.f, 100.f });

	// misc
	if (data.contains("misc")) {
		cfg::misc::bhop = data["misc"].value("bhop", false);
		cfg::misc::quick_stop = data["misc"].value("quick_stop", false);
		cfg::misc::null_binds = data["misc"].value("null_binds", false);
		cfg::misc::auto_strafe = data["misc"].value("auto_strafe", false);
		// Configs from before the key: "only with space" was holding SPACE, without it always
		bool strafe_only_space = data["misc"].value("auto_strafe_space", true);
		cfg::misc::air_strafe_mode = std::clamp(data["misc"].value("air_strafe_mode", strafe_only_space ? 1 : 2), 0, 2);
		cfg::misc::air_strafe_key = data["misc"].value("air_strafe_key", VK_SPACE);
		cfg::misc::jump_bug = data["misc"].value("jump_bug", false);
		cfg::misc::jump_bug_mode = std::clamp(data["misc"].value("jump_bug_mode", 1), 0, 2);
		cfg::misc::jump_bug_key = data["misc"].value("jump_bug_key", VK_XBUTTON1);
		cfg::misc::auto_accept = data["misc"].value("auto_accept", false);
		cfg::misc::clantag = data["misc"].value("clantag", false);
		cfg::misc::clantag_mode = std::clamp(data["misc"].value("clantag_mode", 0), 0, 11);
		cfg::misc::clantag_speed = std::clamp(data["misc"].value("clantag_speed", 350.f), 100.f, 1500.f);
		snprintf(cfg::misc::clantag_text, sizeof(cfg::misc::clantag_text), "%s", data["misc"].value("clantag_text", std::string("Cs2 External")).c_str());
		cfg::misc::clantag_source = std::clamp(data["misc"].value("clantag_source", 0), 0, 1);
		cfg::misc::clantag_target = std::clamp(data["misc"].value("clantag_target", data["misc"].value("clantag_brackets", true) ? 0 : 1), 0, 3);
		cfg::misc::name_change = data["misc"].value("name_change", false);
		snprintf(cfg::misc::name_text, sizeof(cfg::misc::name_text), "%s", data["misc"].value("name_text", std::string()).c_str());
		cfg::misc::hitsound = data["misc"].value("hitsound", false);
		cfg::misc::hitsound_file = data["misc"].value("hitsound_file", std::string());
		cfg::misc::hitsound_volume = std::clamp(data["misc"].value("hitsound_volume", 70), 0, 100);
		cfg::misc::killsound = data["misc"].value("killsound", false);
		cfg::misc::killsound_file = data["misc"].value("killsound_file", std::string());
		cfg::misc::killsound_volume = std::clamp(data["misc"].value("killsound_volume", 70), 0, 100);
	}

	// visuals
	if (data.contains("visuals")) {
		namespace vis = cfg::visuals;
		const auto& from = data["visuals"];
		vis::no_flash = from.value("no_flash", false);
		vis::flash_alpha = std::clamp(from.value("flash_alpha", 0.f), 0.f, 255.f);
		vis::no_smoke = from.value("no_smoke", false);

		if (from.contains("chams")) {
			const auto& chams = from["chams"];
			vis::chams::enemies = chams.value("enemies", false);
			vis::chams::team = chams.value("team", false);
			vis::chams::enemy_type = std::clamp(chams.value("enemy_type", 0), 0, vis::chams::TYPE_COUNT - 1);
			vis::chams::team_type = std::clamp(chams.value("team_type", 0), 0, vis::chams::TYPE_COUNT - 1);
			vis::chams::enemy_color = JsonToColor(chams, "enemy_color", vis::chams::enemy_color);
			vis::chams::team_color = JsonToColor(chams, "team_color", vis::chams::team_color);
		}

		if (from.contains("glow")) {
			const auto& glow = from["glow"];
			vis::glow::enemies = glow.value("enemies", false);
			vis::glow::team = glow.value("team", false);
			vis::glow::bomb = glow.value("bomb", false);
			vis::glow::items = glow.value("items", false);
			vis::glow::utility = glow.value("utility", false);
			vis::glow::dropped_bomb = glow.value("dropped_bomb", false);
			vis::glow::thrown = glow.value("thrown", false);
			vis::glow::enemy_color = JsonToColor(glow, "enemy_color", vis::glow::enemy_color);
			vis::glow::team_color = JsonToColor(glow, "team_color", vis::glow::team_color);
			vis::glow::bomb_color = JsonToColor(glow, "bomb_color", vis::glow::bomb_color);
			vis::glow::item_color = JsonToColor(glow, "item_color", vis::glow::item_color);
			vis::glow::utility_color = JsonToColor(glow, "utility_color", vis::glow::utility_color);
			vis::glow::dropped_bomb_color = JsonToColor(glow, "dropped_bomb_color", vis::glow::dropped_bomb_color);
			namespace thrown = vis::glow::thrown_colors;
			thrown::smoke = JsonToColor(glow, "thrown_smoke", thrown::smoke);
			thrown::molotov = JsonToColor(glow, "thrown_molotov", thrown::molotov);
			thrown::flash = JsonToColor(glow, "thrown_flash", thrown::flash);
			thrown::he = JsonToColor(glow, "thrown_he", thrown::he);
			thrown::decoy = JsonToColor(glow, "thrown_decoy", thrown::decoy);
		}
	}

	// view
	if (data.contains("view")) {
		cfg::view::fov_enabled = data["view"].value("fov_enabled", false);
		cfg::view::fov = data["view"].value("fov", 90);
		cfg::view::third_person = data["view"].value("third_person", false);
		cfg::view::third_person_mode = data["view"].value("third_person_mode", 0);
		cfg::view::third_person_key = data["view"].value("third_person_key", VK_XBUTTON2);
		cfg::view::third_person_scoped_off = data["view"].value("third_person_scoped_off", true);
		cfg::view::freecam = data["view"].value("freecam", false);
		cfg::view::freecam_key = data["view"].value("freecam_key", VK_F6);
		cfg::view::freecam_speed = std::clamp(data["view"].value("freecam_speed", 600.f), 100.f, 3000.f);
		cfg::view::freecam_sensitivity = std::clamp(data["view"].value("freecam_sensitivity", 1.f), 0.1f, 5.f);
		cfg::view::spectate_distance = std::clamp(data["view"].value("spectate_distance", 100.f), 40.f, 300.f);
		cfg::view::dead_spectate = data["view"].value("dead_spectate", true);
		cfg::view::viewmodel_enabled = data["view"].value("viewmodel_enabled", false);
		cfg::view::viewmodel_fov = data["view"].value("viewmodel_fov", 68.f);
		cfg::view::viewmodel_x = data["view"].value("viewmodel_x", 2.5f);
		cfg::view::viewmodel_y = data["view"].value("viewmodel_y", 0.f);
		cfg::view::viewmodel_z = data["view"].value("viewmodel_z", -1.5f);
	}

	// utils
	//cfg::settings::console = data["utils"].value("console", true);
	cfg::settings::watermark = data["utils"].value("watermark", true);
	cfg::settings::notifications = data["utils"].value("notifications", true);
	cfg::settings::streamproof = data["utils"].value("streamproof", false);
	cfg::settings::free_cpu = data["utils"].value("free_cpu", true);
	cfg::settings::accent = JsonToColor(data["utils"], "accent", cfg::settings::accent);
	cfg::settings::menu_opacity = std::clamp(data["utils"].value("menu_opacity", 0.86f), 0.3f, 1.f);
	cfg::settings::ui_scale = std::clamp(data["utils"].value("ui_scale", 1.15f), 0.8f, 1.6f);

	{
		namespace logs = cfg::settings::logs;
		auto log = data["utils"].value("log", nlohmann::json::object());
		logs::skins = log.value("skins", true);
		logs::other = log.value("other", true);
	}
	//cfg::settings::open_menu_key = data["utils"].value("open_menu_key", 0);
}

void Config::ApplySkins(json skins) {
	std::lock_guard<std::mutex> lock(cfg::skins::mutex);

	cfg::skins::enabled = skins.value("enabled", false);
	cfg::skins::glove_hide_third_person = skins.value("glove_hide_third_person", true);
	cfg::skins::music_kit = skins.value("music_kit", 0);

	auto read_loadout = [](const json& from, cfg::skins::loadout_t& loadout) {
		loadout = {};
		loadout.glove = from.value("glove", 0);
		loadout.agent = from.value("agent", 0);
		loadout.custom_model = from.value("custom_model", std::string());
		loadout.knife = from.value("knife", 0);

		if (auto items = from.find("items"); items != from.end() && items->is_object()) {
			for (const auto& [key, value] : items->items()) {
				cfg::skins::item_t item;
				item.paint_kit = value.value("paint_kit", 0);
				item.wear = value.value("wear", 0.0001f);
				item.seed = value.value("seed", 0);

				if (item.paint_kit > 0)
					loadout.items[std::stoi(key)] = item;
			}
		}
	};

	const char* team_keys[] = { "t", "ct" };
	for (int team = 0; team < cfg::skins::TEAM_COUNT; team++) {
		// Older configs had one loadout for both teams
		const auto& from = skins.contains(team_keys[team]) ? skins[team_keys[team]] : skins;
		read_loadout(from, cfg::skins::loadouts[team]);
	}
}

json Config::SettingsJson() {
	json data;

	data["enabled"] = cfg::enabled;

	// esp
	data["esp"]["bomb"] = cfg::esp::bomb;

	{
		namespace it = cfg::esp::items;
		auto& items = data["esp"]["items"];
		items["enabled"] = it::enabled;
		items["max_distance"] = it::max_distance;
		items["text_size"] = it::text_size;
		items["icon_size"] = it::icon_size;
		WriteCategory(items["weapons"], it::weapons);
		WriteCategory(items["utility"], it::utility);
		WriteCategory(items["bomb"], it::bomb);
		WriteCategory(items["kits"], it::kits);
	}
	WriteGroup(data["esp"]["enemy"], cfg::esp::enemy);
	WriteGroup(data["esp"]["team"], cfg::esp::team);

	data["esp"]["grenades"]["enabled"] = cfg::esp::grenades::enabled;
	data["esp"]["grenades"]["trails"] = cfg::esp::grenades::trails;
	data["esp"]["grenades"]["trail_type_color"] = cfg::esp::grenades::trail_type_color;
	data["esp"]["grenades"]["timers"] = cfg::esp::grenades::timers;
	data["esp"]["grenades"]["prediction"] = cfg::esp::grenades::prediction;
	data["esp"]["grenades"]["landing"] = cfg::esp::grenades::landing;
	data["esp"]["grenades"]["smoke_area"] = cfg::esp::grenades::smoke_area;
	data["esp"]["grenades"]["fire_area"] = cfg::esp::grenades::fire_area;
	data["esp"]["grenades"]["glow"] = cfg::esp::grenades::glow;
	data["esp"]["grenades"]["icons"] = cfg::esp::grenades::icons;
	data["esp"]["grenades"]["names"] = cfg::esp::grenades::names;
	data["esp"]["grenades"]["timer_bars"] = cfg::esp::grenades::timer_bars;
	data["esp"]["grenades"]["text_size"] = cfg::esp::grenades::text_size;

	// world
	// spectator list
	data["world"]["spectators"]["enabled"] = cfg::world::spectators::enabled;
	data["world"]["spectators"]["detailed"] = cfg::world::spectators::detailed;
	data["world"]["spectators"]["self_only"] = cfg::world::spectators::self_only;
	Vec2ToJson(data["world"]["spectators"], "pos", cfg::world::spectators::pos);

	data["world"]["keybinds"]["enabled"] = cfg::world::keybinds::enabled;
	Vec2ToJson(data["world"]["keybinds"], "pos", cfg::world::keybinds::pos);
	data["world"]["keybinds"]["hide_bhop"] = cfg::world::keybinds::hide_bhop;
	data["world"]["keybinds"]["hide_air_strafe"] = cfg::world::keybinds::hide_air_strafe;
	data["world"]["keybinds"]["hide_jump_bug"] = cfg::world::keybinds::hide_jump_bug;

	data["world"]["votes"]["enabled"] = cfg::world::votes::enabled;
	Vec2ToJson(data["world"]["votes"], "pos", cfg::world::votes::pos);

	// bomb
	data["world"]["bomb"]["location"] = cfg::world::bomb::location;
	data["world"]["bomb"]["timer"] = cfg::world::bomb::timer;
	Vec2ToJson(data["world"]["bomb"], "pos", cfg::world::bomb::pos);

	// hitmarker
	{
		namespace hm = cfg::world::hitmarker;
		auto& to = data["world"]["hitmarker"];
		to["crosshair"] = hm::crosshair;
		to["world"] = hm::world;
		to["damage"] = hm::damage;
		to["duration"] = hm::duration;
		to["size"] = hm::size;
		ColorToJson(to, "color", hm::color);
		ColorToJson(to, "kill_color", hm::kill_color);
	}

	// crosshair
	data["world"]["crosshair"]["enabled"] = cfg::world::crosshair::enabled;
	data["world"]["crosshair"]["style"] = cfg::world::crosshair::style;

	// radar
	data["world"]["radar"]["enabled"] = cfg::world::radar::enabled;
	data["world"]["radar"]["no_rotate"] = cfg::world::radar::no_rotate;
	data["world"]["radar"]["mode"] = cfg::world::radar::mode;
	data["world"]["radar"]["range"] = cfg::world::radar::range;
	Vec2ToJson(data["world"]["radar"], "pos", cfg::world::radar::pos);
	Vec2ToJson(data["world"]["radar"], "size", cfg::world::radar::size);

	// velocity
	data["world"]["velocity"]["enabled"] = cfg::world::velocity::enabled;
	data["world"]["velocity"]["sample_rate"] = cfg::world::velocity::sample_rate;
	data["world"]["velocity"]["sample_length"] = cfg::world::velocity::sample_length;
	Vec2ToJson(data["world"]["velocity"], "pos", cfg::world::velocity::pos);
	Vec2ToJson(data["world"]["velocity"], "size", cfg::world::velocity::size);

	// colors
	auto& col = data["esp"]["colors"];
	ColorToJson(col, "bomb", cfg::esp::colors::bomb);

	// grenade colors
	auto& gcol = col["grenades"];
	ColorToJson(gcol, "smoke", cfg::esp::colors::grenades::smoke);
	ColorToJson(gcol, "molotov", cfg::esp::colors::grenades::molotov);
	ColorToJson(gcol, "flash", cfg::esp::colors::grenades::flash);
	ColorToJson(gcol, "he", cfg::esp::colors::grenades::he);
	ColorToJson(gcol, "decoy", cfg::esp::colors::grenades::decoy);
	ColorToJson(gcol, "trail", cfg::esp::colors::grenades::trail);

	// misc
	data["misc"]["bhop"] = cfg::misc::bhop;
	data["misc"]["quick_stop"] = cfg::misc::quick_stop;
	data["misc"]["null_binds"] = cfg::misc::null_binds;
	data["misc"]["auto_strafe"] = cfg::misc::auto_strafe;
	data["misc"]["air_strafe_mode"] = cfg::misc::air_strafe_mode;
	data["misc"]["air_strafe_key"] = cfg::misc::air_strafe_key;
	data["misc"]["jump_bug"] = cfg::misc::jump_bug;
	data["misc"]["jump_bug_mode"] = cfg::misc::jump_bug_mode;
	data["misc"]["jump_bug_key"] = cfg::misc::jump_bug_key;
	data["misc"]["auto_accept"] = cfg::misc::auto_accept;
	data["misc"]["clantag"] = cfg::misc::clantag;
	data["misc"]["clantag_mode"] = cfg::misc::clantag_mode;
	data["misc"]["clantag_speed"] = cfg::misc::clantag_speed;
	data["misc"]["clantag_text"] = std::string(cfg::misc::clantag_text);
	data["misc"]["hitsound"] = cfg::misc::hitsound;
	data["misc"]["hitsound_file"] = cfg::misc::hitsound_file;
	data["misc"]["hitsound_volume"] = cfg::misc::hitsound_volume;
	data["misc"]["killsound"] = cfg::misc::killsound;
	data["misc"]["killsound_file"] = cfg::misc::killsound_file;
	data["misc"]["killsound_volume"] = cfg::misc::killsound_volume;
	data["misc"]["clantag_target"] = cfg::misc::clantag_target;
	data["misc"]["clantag_source"] = cfg::misc::clantag_source;
	data["misc"]["name_change"] = cfg::misc::name_change;
	data["misc"]["name_text"] = std::string(cfg::misc::name_text);

	// visuals
	{
		namespace vis = cfg::visuals;
		auto& to = data["visuals"];
		to["no_flash"] = vis::no_flash;
		to["flash_alpha"] = vis::flash_alpha;
		to["no_smoke"] = vis::no_smoke;

		auto& chams = to["chams"];
		chams["enemies"] = vis::chams::enemies;
		chams["team"] = vis::chams::team;
		chams["enemy_type"] = vis::chams::enemy_type;
		chams["team_type"] = vis::chams::team_type;
		ColorToJson(chams, "enemy_color", vis::chams::enemy_color);
		ColorToJson(chams, "team_color", vis::chams::team_color);

		auto& glow = to["glow"];
		glow["enemies"] = vis::glow::enemies;
		glow["team"] = vis::glow::team;
		glow["bomb"] = vis::glow::bomb;
		glow["items"] = vis::glow::items;
		glow["utility"] = vis::glow::utility;
		glow["dropped_bomb"] = vis::glow::dropped_bomb;
		glow["thrown"] = vis::glow::thrown;
		ColorToJson(glow, "enemy_color", vis::glow::enemy_color);
		ColorToJson(glow, "team_color", vis::glow::team_color);
		ColorToJson(glow, "bomb_color", vis::glow::bomb_color);
		ColorToJson(glow, "item_color", vis::glow::item_color);
		ColorToJson(glow, "utility_color", vis::glow::utility_color);
		ColorToJson(glow, "dropped_bomb_color", vis::glow::dropped_bomb_color);
		ColorToJson(glow, "thrown_smoke", vis::glow::thrown_colors::smoke);
		ColorToJson(glow, "thrown_molotov", vis::glow::thrown_colors::molotov);
		ColorToJson(glow, "thrown_flash", vis::glow::thrown_colors::flash);
		ColorToJson(glow, "thrown_he", vis::glow::thrown_colors::he);
		ColorToJson(glow, "thrown_decoy", vis::glow::thrown_colors::decoy);
	}

	// view
	data["view"]["fov_enabled"] = cfg::view::fov_enabled;
	data["view"]["fov"] = cfg::view::fov;
	data["view"]["third_person"] = cfg::view::third_person;
	data["view"]["third_person_mode"] = cfg::view::third_person_mode;
	data["view"]["third_person_key"] = cfg::view::third_person_key;
	data["view"]["third_person_scoped_off"] = cfg::view::third_person_scoped_off;
	data["view"]["freecam"] = cfg::view::freecam;
	data["view"]["freecam_key"] = cfg::view::freecam_key;
	data["view"]["freecam_speed"] = cfg::view::freecam_speed;
	data["view"]["freecam_sensitivity"] = cfg::view::freecam_sensitivity;
	data["view"]["spectate_distance"] = cfg::view::spectate_distance;
	data["view"]["dead_spectate"] = cfg::view::dead_spectate;
	data["view"]["viewmodel_enabled"] = cfg::view::viewmodel_enabled;
	data["view"]["viewmodel_fov"] = cfg::view::viewmodel_fov;
	data["view"]["viewmodel_x"] = cfg::view::viewmodel_x;
	data["view"]["viewmodel_y"] = cfg::view::viewmodel_y;
	data["view"]["viewmodel_z"] = cfg::view::viewmodel_z;

	// utils
	//data["utils"]["console"] = cfg::settings::console;
	data["utils"]["watermark"] = cfg::settings::watermark;
	data["utils"]["notifications"] = cfg::settings::notifications;
	data["utils"]["streamproof"] = cfg::settings::streamproof;
	data["utils"]["free_cpu"] = cfg::settings::free_cpu;
	ColorToJson(data["utils"], "accent", cfg::settings::accent);
	data["utils"]["menu_opacity"] = cfg::settings::menu_opacity;
	data["utils"]["ui_scale"] = cfg::settings::ui_scale;
	data["utils"]["log"] = {
		{ "skins", cfg::settings::logs::skins },
		{ "other", cfg::settings::logs::other },
	};
	//data["utils"]["open_menu_key"] = cfg::settings::open_menu_key;

	return data;
}

json Config::SkinsJson() {
	json data;

	std::lock_guard<std::mutex> lock(cfg::skins::mutex);

	data["enabled"] = cfg::skins::enabled;
	data["glove_hide_third_person"] = cfg::skins::glove_hide_third_person;
	data["music_kit"] = cfg::skins::music_kit;

	const char* team_keys[] = { "t", "ct" };
	for (int team = 0; team < cfg::skins::TEAM_COUNT; team++) {
		const auto& loadout = cfg::skins::loadouts[team];
		auto& to = data[team_keys[team]];

		to["glove"] = loadout.glove;
		to["agent"] = loadout.agent;
		to["custom_model"] = loadout.custom_model;
		to["knife"] = loadout.knife;
		to["items"] = json::object();

		for (const auto& [index, item] : loadout.items) {
			auto& entry = to["items"][std::to_string(index)];
			entry["paint_kit"] = item.paint_kit;
			entry["wear"] = item.wear;
			entry["seed"] = item.seed;
		}
	}

	return data;
}

bool Config::ReadImpl() {
	std::ifstream f("config.json");

	if (!f.good()) {
		LOGF(FATAL, "Configuration file does not exist, creating a new one");
		WriteImpl();
		return false;
	}

	json data;
	try {
		data = json::parse(f);
	}
	catch (const std::exception& e) {
		LOGF(FATAL, "Failed to parse configuration file");
		WriteImpl();
		return false;
	}

	if (data.empty())
		return false;

	try {
		ApplySettings(data);

		if (data.contains("skins"))
			ApplySkins(data["skins"]);
	}
	catch (const std::exception& e) {
		LOGF(FATAL, "Failed to parse configuration");
		WriteImpl();
		return false;
	}

	LOGF(INFO, "Successfully parsed configuration");
	return true;
}

bool Config::WriteImpl() {
	std::ofstream f("config.json");

	json data = SettingsJson();
	data["skins"] = SkinsJson();

	f << std::setw(4) << data << std::endl;
	f.close();

	LOGF(VERBOSE, "Writting configuration to file");

	return true;
}


// TODO: Refactor this
color_t Config::JsonToColor(const json& parent, const std::string& key, const color_t& def) {
	if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() != 4)
		return def;
	return color_t(
		parent[key][0].get<float>(),
		parent[key][1].get<float>(),
		parent[key][2].get<float>(),
		parent[key][3].get<float>()
	);
}

void Config::ColorToJson(json& parent, const std::string& key, const color_t& color) {
	parent[key] = { color.r, color.g, color.b, color.a };
}

Vec2_t Config::JsonToVec2(const json& parent, const std::string& key, const Vec2_t& def)
{
	if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() != 2)
		return def;

	return Vec2_t{
		parent[key][0].get<float>(),
		parent[key][1].get<float>()
	};
}

void Config::Vec2ToJson(json& parent, const std::string& key, const Vec2_t& vec)
{
	parent[key] = { vec.x, vec.y };
}
// Presets

namespace {
	constexpr auto PRESET_EXTENSION = ".json";
	constexpr size_t PRESET_NAME_MAX = 48;

	// Names are UTF-8, paths want char8_t for that
	std::filesystem::path Utf8Path(const std::string& text) {
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	std::string PathUtf8(const std::filesystem::path& path) {
		auto text = path.u8string();
		return std::string(text.begin(), text.end());
	}

	const char* PresetType(Config::Preset kind) {
		return kind == Config::Preset::SKINS ? "skins" : "config";
	}

	std::filesystem::path PresetPath(Config::Preset kind, const std::string& name) {
		return Config::PresetFolder(kind) / Utf8Path(name + PRESET_EXTENSION);
	}
}

std::filesystem::path Config::PresetFolder(Preset kind) {
	return std::filesystem::path("export") / (kind == Preset::SKINS ? "skins" : "configs");
}

std::string Config::CleanPresetName(const std::string& name) {
	std::string clean;
	for (char c : name) {
		if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
			continue;
		clean += c;
	}

	// No spaces or dots at the ends, Windows drops them
	auto first = clean.find_first_not_of(" .");
	if (first == std::string::npos)
		return "";
	clean = clean.substr(first, clean.find_last_not_of(" .") - first + 1);

	if (clean.size() > PRESET_NAME_MAX)
		clean.resize(PRESET_NAME_MAX);
	return clean;
}

std::vector<std::string> Config::ListPresets(Preset kind) {
	std::vector<std::pair<std::filesystem::file_time_type, std::string>> found;
	std::error_code error;

	for (const auto& entry : std::filesystem::directory_iterator(PresetFolder(kind), error)) {
		if (!entry.is_regular_file(error) || entry.path().extension() != PRESET_EXTENSION)
			continue;
		found.emplace_back(entry.last_write_time(error), PathUtf8(entry.path().stem()));
	}

	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

	std::vector<std::string> names;
	for (auto& [time, name] : found)
		names.push_back(std::move(name));
	return names;
}

std::string Config::SavePreset(Preset kind, const std::string& raw_name) {
	auto name = CleanPresetName(raw_name);
	if (name.empty())
		return "Give it a name first";

	std::error_code error;
	std::filesystem::create_directories(PresetFolder(kind), error);

	json file;
	file["type"] = PresetType(kind);
	file["data"] = kind == Preset::SKINS ? SkinsJson() : SettingsJson();

	std::ofstream out(PresetPath(kind, name));
	if (!out.good())
		return "Could not write " + PathUtf8(PresetPath(kind, name));

	out << std::setw(4) << file << std::endl;
	LOGF(INFO, "Exported {} '{}'", PresetType(kind), name);
	return "";
}

std::string Config::LoadPreset(Preset kind, const std::string& name) {
	std::ifstream in(PresetPath(kind, name));
	if (!in.good())
		return "The file is gone";

	json file;
	try {
		file = json::parse(in);
	}
	catch (const std::exception&) {
		return "Not a valid json file";
	}

	// Ours have a type, a plain config.json works too
	json data;
	if (file.contains("type") && file.contains("data")) {
		if (file.value("type", "") != PresetType(kind))
			return kind == Preset::SKINS ? "This is a config, not skins" : "These are skins, not a config";
		data = file["data"];
	}
	else if (kind == Preset::SKINS) {
		if (!file.contains("skins"))
			return "No skins in this file";
		data = file["skins"];
	}
	else {
		data = file;
		data.erase("skins");
	}

	try {
		if (kind == Preset::SKINS)
			ApplySkins(data);
		else
			ApplySettings(data);
	}
	catch (const std::exception&) {
		return "Some values could not be read";
	}

	// Kept after a restart
	Write();

	LOGF(INFO, "Imported {} '{}'", PresetType(kind), name);
	return "";
}

std::string Config::RenamePreset(Preset kind, const std::string& from, const std::string& raw_to) {
	auto to = CleanPresetName(raw_to);
	if (to.empty())
		return "Give it a name first";
	if (to == from)
		return "";

	std::error_code error;
	if (std::filesystem::exists(PresetPath(kind, to), error))
		return "There is one with that name already";

	std::filesystem::rename(PresetPath(kind, from), PresetPath(kind, to), error);
	if (error)
		return "Could not rename it";

	if (GetDefaultPreset(kind) == from)
		SetDefaultPreset(kind, to);
	return "";
}

std::string Config::DeletePreset(Preset kind, const std::string& name) {
	std::error_code error;
	std::filesystem::remove(PresetPath(kind, name), error);
	if (error)
		return "Could not delete it";

	if (GetDefaultPreset(kind) == name)
		SetDefaultPreset(kind, "");
	return "";
}

namespace {
	const std::filesystem::path DEFAULTS_FILE = std::filesystem::path("export") / "defaults.json";

	json ReadDefaults() {
		std::ifstream in(DEFAULTS_FILE);
		if (!in.good())
			return json::object();

		try {
			auto data = json::parse(in);
			return data.is_object() ? data : json::object();
		}
		catch (const std::exception&) {
			return json::object();
		}
	}
}

std::string Config::GetDefaultPreset(Preset kind) {
	auto data = ReadDefaults();
	auto value = data.value(PresetType(kind), std::string());

	// Gone since, by hand
	std::error_code error;
	return !value.empty() && std::filesystem::exists(PresetPath(kind, value), error) ? value : "";
}

void Config::SetDefaultPreset(Preset kind, const std::string& name) {
	auto data = ReadDefaults();
	if (name.empty())
		data.erase(PresetType(kind));
	else
		data[PresetType(kind)] = name;

	std::error_code error;
	std::filesystem::create_directories(DEFAULTS_FILE.parent_path(), error);

	std::ofstream out(DEFAULTS_FILE);
	out << std::setw(4) << data << std::endl;
}

void Config::LoadDefaultPresets() {
	for (auto kind : { Preset::CONFIG, Preset::SKINS }) {
		auto name = GetDefaultPreset(kind);
		if (name.empty())
			continue;

		auto error = LoadPreset(kind, name);
		if (!error.empty())
			LOGF(WARNING, "Could not import the default {} '{}': {}", PresetType(kind), name, error);
	}
}
