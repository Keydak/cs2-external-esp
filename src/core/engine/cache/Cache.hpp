#pragma once
#include "core/engine/classes/Game.hpp"
#include "core/engine/classes/Bomb.hpp"
#include "core/engine/classes/Player.hpp"
#include "core/engine/classes/Globals.hpp"
#include "core/engine/classes/Grenades.hpp"
#include "core/engine/classes/Items.hpp"
#include "core/engine/classes/GrenadePrediction.hpp"

using namespace std::chrono;

struct Snapshot {
	Game game;
	Bomb bomb;
	Player local;
	Globals globals;
	std::vector<Player> players;
	std::vector<Grenade> grenades;
	std::vector<Item> items;
	GrenadePath grenade_path;
};

class Cache {
public:
	Game game;
	Bomb bomb;
	Player local;
	Globals globals;
	std::vector<Player> players;
	std::vector<Grenade> grenades;
	std::vector<Item> items;
	GrenadePath grenade_path;
public:
	static Cache& Get()
	{
		static Cache instance{};
		return instance;
	}

	static Snapshot CopySnapshot();

	static bool Refresh();
private:
	std::mutex mtx;
	milliseconds duration{1};
	steady_clock::time_point last{};

	// Scanning the whole entity list is expensive, grenades dont need the player refresh rate
	Grenades grenade_tracker;
	Items item_tracker;
	milliseconds item_rate{100};
	steady_clock::time_point last_item_scan{};
	milliseconds grenade_rate{30};
	steady_clock::time_point last_grenade_scan{};

	// Who we can see, by player index
	std::unordered_map<int, bool> visibility;
	milliseconds visibility_rate{15};
	steady_clock::time_point last_visibility{};

	// Area at the end of the throw preview, only reshaped when the landing spot moves. Shaping takes many
	// traces, it runs on its own thread & the preview shows the last finished one meanwhile
	AreaShape preview_area;
	Vec3_t preview_end{};
	GrenadeType preview_type = GrenadeType::HE;
	bool preview_valid = false;

	std::mutex shape_mtx;
	std::atomic<bool> shaping = false;
	AreaShape shaped_area;
	Vec3_t shaped_end{};
	GrenadeType shaped_type = GrenadeType::HE;
	bool shaped_ready = false;
private:
	bool RefreshImpl();
	void ShapePreviewArea(GrenadePath& path);
};