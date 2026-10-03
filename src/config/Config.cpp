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

	// bomb
	cfg::world::bomb::location = data["world"]["bomb"].value("location", true);
	cfg::world::bomb::timer = data["world"]["bomb"].value("timer", true);
	cfg::world::bomb::pos = JsonToVec2(data["world"]["bomb"], "pos", { 10.f, 300.f });

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
		cfg::misc::slide_walk = data["misc"].value("slide_walk", false);
		cfg::misc::slide_walk_key = data["misc"].value("slide_walk_key", VK_XBUTTON1);
		cfg::misc::slide_walk_rate = data["misc"].value("slide_walk_rate", 8.f);
		cfg::misc::slide_walk_ratio = data["misc"].value("slide_walk_ratio", 0.35f);
		cfg::misc::slide_walk_mode = data["misc"].value("slide_walk_mode", 0);
		cfg::misc::slide_walk_pattern = data["misc"].value("slide_walk_pattern", 0);
		cfg::misc::slide_walk_speed = data["misc"].value("slide_walk_speed", 130.f);
		cfg::misc::slide_walk_auto = data["misc"].value("slide_walk_auto", false);
		cfg::misc::slide_walk_indicator = data["misc"].value("slide_walk_indicator", true);
	}

	// view
	if (data.contains("view")) {
		cfg::view::fov_enabled = data["view"].value("fov_enabled", false);
		cfg::view::fov = data["view"].value("fov", 90);
		cfg::view::third_person = data["view"].value("third_person", false);
		cfg::view::third_person_mode = data["view"].value("third_person_mode", 0);
		cfg::view::third_person_key = data["view"].value("third_person_key", VK_XBUTTON2);
		cfg::view::third_person_scoped_off = data["view"].value("third_person_scoped_off", true);
		cfg::view::viewmodel_enabled = data["view"].value("viewmodel_enabled", false);
		cfg::view::viewmodel_fov = data["view"].value("viewmodel_fov", 68.f);
		cfg::view::viewmodel_x = data["view"].value("viewmodel_x", 2.5f);
		cfg::view::viewmodel_y = data["view"].value("viewmodel_y", 0.f);
		cfg::view::viewmodel_z = data["view"].value("viewmodel_z", -1.5f);
	}

	// utils
	//cfg::settings::console = data["utils"].value("console", true);
	cfg::settings::watermark = data["utils"].value("watermark", true);
	cfg::settings::streamproof = data["utils"].value("streamproof", false);
	cfg::settings::vsync = data["utils"].value("vsync", true);
	cfg::settings::free_cpu = data["utils"].value("free_cpu", true);
	cfg::settings::accent = JsonToColor(data["utils"], "accent", cfg::settings::accent);
	cfg::settings::ui_scale = std::clamp(data["utils"].value("ui_scale", 1.15f), 0.8f, 1.6f);
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

	// bomb
	data["world"]["bomb"]["location"] = cfg::world::bomb::location;
	data["world"]["bomb"]["timer"] = cfg::world::bomb::timer;
	Vec2ToJson(data["world"]["bomb"], "pos", cfg::world::bomb::pos);

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
	data["misc"]["slide_walk"] = cfg::misc::slide_walk;
	data["misc"]["slide_walk_key"] = cfg::misc::slide_walk_key;
	data["misc"]["slide_walk_rate"] = cfg::misc::slide_walk_rate;
	data["misc"]["slide_walk_ratio"] = cfg::misc::slide_walk_ratio;
	data["misc"]["slide_walk_mode"] = cfg::misc::slide_walk_mode;
	data["misc"]["slide_walk_pattern"] = cfg::misc::slide_walk_pattern;
	data["misc"]["slide_walk_speed"] = cfg::misc::slide_walk_speed;
	data["misc"]["slide_walk_auto"] = cfg::misc::slide_walk_auto;
	data["misc"]["slide_walk_indicator"] = cfg::misc::slide_walk_indicator;

	// view
	data["view"]["fov_enabled"] = cfg::view::fov_enabled;
	data["view"]["fov"] = cfg::view::fov;
	data["view"]["third_person"] = cfg::view::third_person;
	data["view"]["third_person_mode"] = cfg::view::third_person_mode;
	data["view"]["third_person_key"] = cfg::view::third_person_key;
	data["view"]["third_person_scoped_off"] = cfg::view::third_person_scoped_off;
	data["view"]["viewmodel_enabled"] = cfg::view::viewmodel_enabled;
	data["view"]["viewmodel_fov"] = cfg::view::viewmodel_fov;
	data["view"]["viewmodel_x"] = cfg::view::viewmodel_x;
	data["view"]["viewmodel_y"] = cfg::view::viewmodel_y;
	data["view"]["viewmodel_z"] = cfg::view::viewmodel_z;

	// utils
	//data["utils"]["console"] = cfg::settings::console;
	data["utils"]["watermark"] = cfg::settings::watermark;
	data["utils"]["streamproof"] = cfg::settings::streamproof;
	data["utils"]["vsync"] = cfg::settings::vsync;
	data["utils"]["free_cpu"] = cfg::settings::free_cpu;
	ColorToJson(data["utils"], "accent", cfg::settings::accent);
	data["utils"]["ui_scale"] = cfg::settings::ui_scale;
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
		if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\|?*").find(c) != std::string_view::npos)
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
