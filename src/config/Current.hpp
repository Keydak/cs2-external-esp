#pragma once

namespace cfg {
	inline bool enabled = true;

	namespace esp {
		inline bool bomb = true;

		// Traces from your eyes to the player against the map collision, for the visible & invisible colors

		// Things drawn around the box of a player, placed by dragging them in the menu
		enum flag_t {
			FLAG_NAME,
			FLAG_HEALTH_BAR,
			FLAG_HEALTH,		// Not used anymore, the number is part of the health bar. Kept so saved layouts keep their numbers
			FLAG_ARMOR_BAR,
			FLAG_MONEY,
			FLAG_PING,
			FLAG_WEAPON,
			FLAG_WEAPON_NAME,
			FLAG_AMMO,
			FLAG_DISTANCE,
			FLAG_FLASHED,
			FLAG_RELOADING,
			FLAG_DEFUSING,
			FLAG_SCOPED,
			FLAG_C4,
			FLAG_FLASHED_TEXT,		// Text versions of the icons, each in its own color
			FLAG_RELOADING_TEXT,
			FLAG_SCOPED_TEXT,
			FLAG_KIT,				// Has a defuse kit
			FLAG_COUNT
		};

		enum side_t { SIDE_TOP, SIDE_BOTTOM, SIDE_LEFT, SIDE_RIGHT, SIDE_COUNT };

		// Settings of team mates or enemies
		struct group_t {
			bool enabled = true;

			bool box = true;
			bool skeleton = true;
			bool head_tracker = true;
			bool tracers = false;
			bool visible_only = false;	// Hidden while behind a wall

			// Visible / behind a wall
			color_t box_visible{ 1.f, 1.f, 1.f, 0.8f };
			color_t box_invisible{ 1.f, 1.f, 1.f, 0.4f };
			color_t skeleton_visible{ 1.f, 1.f, 1.f, 0.8f };
			color_t skeleton_invisible{ 1.f, 1.f, 1.f, 0.4f };
			color_t tracker_visible{ 1.f, 1.f, 1.f, 0.5f };
			color_t tracker_invisible{ 1.f, 1.f, 1.f, 0.3f };
			color_t tracer_visible{ 1.f, 1.f, 1.f, 0.6f };
			color_t tracer_invisible{ 1.f, 1.f, 1.f, 0.3f };

			color_t text{ 1.f, 1.f, 1.f, 1.f };	// Text flags
			color_t icon{ 1.f, 1.f, 1.f, 0.9f };	// Icon flags, flashed, reloading...

			color_t flashed{ 1.f, 0.95f, 0.45f, 1.f };	// Text flags of the states
			color_t reloading{ 1.f, 0.6f, 0.2f, 1.f };
			color_t scoped{ 0.4f, 0.8f, 1.f, 1.f };
			color_t defusing{ 1.f, 0.3f, 0.3f, 1.f };

			float text_size = 12.f;		// Name, weapon, ping...
			float state_size = 10.f;	// FLASHED, RELOADING, SCOPED & DEFUSING
			float icon_size = 15.f;

			bool health_number = false;	// The value on the health bar
			bool armor_number = false;	// The value on the armor bar

			std::vector<int> layout[SIDE_COUNT];	// flag_t of each side, in drawing order. Flags in no side are not drawn
		};

		inline group_t MakeEnemy() {
			group_t g;
			g.box_visible = { 1.f, 0.24f, 0.24f, 0.9f };
			g.box_invisible = { 1.f, 0.62f, 0.2f, 0.6f };
			g.skeleton_visible = { 1.f, 0.24f, 0.24f, 0.8f };
			g.skeleton_invisible = { 1.f, 0.62f, 0.2f, 0.5f };
			g.tracker_visible = { 1.f, 1.f, 1.f, 0.5f };
			g.tracker_invisible = { 1.f, 1.f, 1.f, 0.25f };
			g.tracer_visible = { 1.f, 0.24f, 0.24f, 0.6f };
			g.tracer_invisible = { 1.f, 0.62f, 0.2f, 0.4f };
			g.layout[SIDE_TOP] = { FLAG_NAME };
			g.layout[SIDE_LEFT] = { FLAG_HEALTH_BAR };
			g.layout[SIDE_BOTTOM] = { FLAG_WEAPON };
			g.layout[SIDE_RIGHT] = { FLAG_PING, FLAG_FLASHED, FLAG_RELOADING, FLAG_DEFUSING, FLAG_SCOPED, FLAG_C4 };
			return g;
		}

		inline group_t MakeTeam() {
			group_t g;
			g.skeleton = false;
			g.head_tracker = false;
			g.box_visible = { 0.f, 1.f, 0.4f, 0.7f };
			g.box_invisible = { 0.f, 0.6f, 0.3f, 0.4f };
			g.skeleton_visible = { 0.f, 1.f, 0.4f, 0.7f };
			g.skeleton_invisible = { 0.f, 0.6f, 0.3f, 0.4f };
			g.tracker_visible = { 1.f, 1.f, 1.f, 0.3f };
			g.tracker_invisible = { 1.f, 1.f, 1.f, 0.2f };
			g.tracer_visible = { 0.f, 1.f, 0.4f, 0.5f };
			g.tracer_invisible = { 0.f, 0.6f, 0.3f, 0.3f };
			g.layout[SIDE_TOP] = { FLAG_NAME };
			g.layout[SIDE_LEFT] = { FLAG_HEALTH_BAR };
			return g;
		}

		inline group_t enemy = MakeEnemy();
		inline group_t team = MakeTeam();

		// Items lying on the ground
		namespace items {
			struct category_t {
				bool enabled = true;
				bool icon = true;
				bool name = false;
				bool ammo = false;		// Weapons only
				bool distance = true;
				color_t color;
			};

			inline bool enabled = false;
			inline float max_distance = 40.f;	// Meters
			inline float text_size = 11.f;
			inline float icon_size = 14.f;

			inline category_t weapons{ true, true, true, true, true, { 0.85f, 0.85f, 0.9f, 1.f } };
			inline category_t utility{ true, true, false, false, true, { 0.45f, 0.85f, 1.f, 1.f } };
			inline category_t bomb{ true, true, true, false, true, { 1.f, 0.84f, 0.f, 1.f } };
			inline category_t kits{ true, true, false, false, true, { 0.4f, 0.8f, 1.f, 1.f } };
		}

		namespace grenades {
			inline bool enabled = true;
			inline bool trails = true;
			inline bool trail_type_color = true; // Use each grenade's own color instead of the trail color
			inline bool timers = true;
			inline bool prediction = true; // Grenade in hand
			inline bool landing = true; // Grenades in the air, thrown by anyone

			inline bool smoke_area = true;	// Shape of a popped smoke on the ground
			inline bool fire_area = true;	// Shape of a molotov fire on the ground
			inline bool glow = true;		// Popped smokes & fires glow on the ground
			inline bool icons = true;		// Label of each grenade
			inline bool names = false;
			inline bool timer_bars = true;
			inline float text_size = 13.f;	// Names & timers
		}

		namespace colors {
			inline color_t bomb{ 1.f, 0.84f, 0.f, 1.f };

			// Trail, icon & area of each grenade type
			namespace grenades {
				inline color_t smoke{ 0.78f, 0.80f, 0.84f, 1.f };
				inline color_t molotov{ 1.f, 0.55f, 0.16f, 1.f }; // Also used for the fire
				inline color_t flash{ 1.f, 0.94f, 0.59f, 1.f };
				inline color_t he{ 1.f, 0.31f, 0.31f, 1.f };
				inline color_t decoy{ 0.67f, 0.67f, 0.67f, 1.f };
				inline color_t trail{ 0.14f, 0.56f, 1.f, 1.f };
			}
		}
	}

	namespace world {
		namespace spectators {
			inline bool enabled = false;

			inline bool detailed = false;
			inline bool self_only = true;

			inline Vec2_t pos{ 10.f, 100.f };
		}

		// Window with the votes of the match: the one going on & the last ones, counts only
		namespace votes {
			inline bool enabled = true;
			inline bool names = false;	// Who voted what, from a listener of ours in the game (memory writing). Not saved, off at every start
			inline Vec2_t pos{ 10.f, 400.f };
		}

		// Window with the keys of the features on right now (third person, free cam)
		namespace keybinds {
			inline bool enabled = true;
			inline Vec2_t pos{ 10.f, 250.f };
			inline bool hide_bhop = false;          // Movement rows left out of the window
			inline bool hide_air_strafe = false;
			inline bool hide_jump_bug = false;
		}

		namespace bomb {
			inline bool location = true;
			inline bool timer = true;
			inline Vec2_t pos{ 10.f, 300.f };
		}

		namespace crosshair {
			enum style_t { STYLE_CLASSIC, STYLE_GAME };

			inline bool enabled = false;
			inline int style = STYLE_CLASSIC;	// Small white cross, or the crosshair set in the game
		}

		// Marks of our hits: at the crosshair & on the player that was hit
		namespace hitmarker {
			inline bool crosshair = false;
			inline bool world = false;
			inline bool damage = true;			// The damage rising from the world mark
			inline float duration = 0.5f;		// Seconds
			inline float size = 6.f;
			inline color_t color{ 1.f, 1.f, 1.f, 1.f };
			inline color_t kill_color{ 1.f, 0.27f, 0.27f, 1.f };
		}

		namespace radar {
			enum mode_t { MODE_OVERLAY, MODE_GAME };

			inline bool enabled = true;
			inline int mode = MODE_OVERLAY;	// Our own window, or enemies shown on the radar of the game (-insecure)
			inline bool no_rotate = false;
			inline float range = 2000.f;
			inline Vec2_t pos{ 10.f, 10.f };
			inline Vec2_t size{ 200.f, 200.f };
		}

		namespace velocity {
			inline bool enabled = false;
			inline int sample_rate = 35;
			inline float sample_length = 5.f;

			inline Vec2_t size{ 400.f, 100.f };
			inline Vec2_t pos{ 10.f, 400.f };
		}
	}

	namespace view {
		inline bool fov_enabled = false;
		inline int fov = 90;

		// Weapon in our hands, the viewmodel_* console variables of the game
		inline bool viewmodel_enabled = false;
		inline float viewmodel_fov = 68.f;
		inline float viewmodel_x = 2.5f;
		inline float viewmodel_y = 0.f;
		inline float viewmodel_z = -1.5f;

		inline bool third_person = false;
		inline int third_person_mode = 0;               // Toggle, hold, always
		inline int third_person_key = VK_XBUTTON2;
		inline bool third_person_scoped_off = true;     // Back to first person while scoped

		// Free cam: the camera flies on its own while our player stands still, see Freecam
		inline bool freecam = false;
		inline int freecam_key = VK_F6;                 // Toggles it
		inline float freecam_speed = 600.f;             // Units per second, x3 with shift
		inline float freecam_sensitivity = 1.f;         // Times the sensitivity of the game

		// Dead: anyone like casual, clicks switch the player, space goes first person, third person, free cam
		inline bool dead_spectate = true;
		inline float spectate_distance = 100.f;         // Behind them in third person
	}

	namespace skins {
		struct item_t {
			int paint_kit = 0;
			float wear = 0.0001f;
			int seed = 0;
		};

		struct loadout_t {
			int agent = 0;                   // Definition index, 0 keeps the model the game gives
			std::string custom_model;        // "characters/models/x/y.vmdl" of the game folder, over the agent once the game loaded it
			int glove = 0;                   // Definition index, 0 keeps the default gloves
			int knife = 0;                   // Definition index, 0 keeps the default knife
			std::map<int, item_t> items;     // By definition index, also holds the glove & knife paint kits
		};

		enum team_t { TERRORIST, COUNTER_TERRORIST, TEAM_COUNT };

		inline bool enabled = false;
		inline bool glove_hide_third_person = true; // For player models with gloves built in, they would show both
		inline loadout_t loadouts[TEAM_COUNT];      // Like the game, each team has its own
		inline int music_kit = 0;                    // Definition index, 0 keeps the one the game gives. Both teams

		inline std::mutex mutex;             // Menu & skin thread
	}

	namespace misc {
		inline bool bhop = false;
		inline bool quick_stop = false;
		inline bool null_binds = false;
		inline bool auto_strafe = false;
		inline int air_strafe_mode = 1;         // Toggle, hold, always
		inline int air_strafe_key = VK_SPACE;
		inline bool jump_bug = false;
		inline int jump_bug_mode = 1;           // Toggle, hold, always
		inline int jump_bug_key = VK_XBUTTON1;
		inline bool auto_accept = false;

		// Clan tag in front of our name, in the scoreboard & kill feed (what the game shows us, offline)
		inline bool clantag = false;
		inline char clantag_text[64] = "Cs2 External";  // Several texts separated by | take turns
		inline int clantag_mode = 0;            // ClanTag::GetModes()
		inline float clantag_speed = 350.f;     // Milliseconds per step of the animation
		inline int clantag_target = 0;          // Clan slot ([tag] name), before the name, after it, the whole name
		inline int clantag_source = 0;          // The text above, or the names of the players in the match taking turns

		inline bool name_change = false;
		inline char name_text[33] = "";         // Empty keeps the real name

		// Sounds of ours on a hit & a kill, files of the sound folder
		inline bool hitsound = false;
		inline std::string hitsound_file;
		inline int hitsound_volume = 70;        // 0 - 100
		inline bool killsound = false;
		inline std::string killsound_file;
		inline int killsound_volume = 70;
	}

	// Removals & glow written into the game
	namespace visuals {
		inline bool no_flash = false;
		inline float flash_alpha = 0.f;         // 0 - 255, how white a flash still gets
		inline bool no_smoke = false;

		// Players tinted in a color by the game itself (its render color), seen where the model is seen
		namespace chams {
			// Textured: the model in the color. Glow: the spawn protection shader of the game in the color
			enum type_t { TYPE_TEXTURED, TYPE_GLOW, TYPE_BOTH, TYPE_COUNT };

			inline bool enemies = false;
			inline bool team = false;
			inline int enemy_type = TYPE_TEXTURED;
			inline int team_type = TYPE_TEXTURED;
			inline color_t enemy_color{ 1.f, 0.2f, 0.6f, 1.f };
			inline color_t team_color{ 0.2f, 0.6f, 1.f, 1.f };
		}

		namespace glow {
			inline bool enemies = false;
			inline bool team = false;
			inline bool bomb = false;           // Planted C4
			inline bool items = false;          // Dropped weapons & defuse kits
			inline bool utility = false;        // Grenades lying around
			inline bool dropped_bomb = false;   // C4 lying around
			inline bool thrown = false;         // Grenades in the air
			inline color_t enemy_color{ 1.f, 0.25f, 0.25f, 1.f };
			inline color_t team_color{ 0.3f, 0.6f, 1.f, 1.f };
			inline color_t bomb_color{ 1.f, 0.84f, 0.f, 1.f };
			inline color_t item_color{ 0.85f, 0.85f, 0.9f, 1.f };
			inline color_t utility_color{ 0.45f, 0.85f, 1.f, 1.f };
			inline color_t dropped_bomb_color{ 1.f, 0.6f, 0.1f, 1.f };

			// Grenades in the air, by type
			namespace thrown_colors {
				inline color_t smoke{ 0.35f, 0.9f, 0.45f, 1.f };
				inline color_t molotov{ 1.f, 0.5f, 0.1f, 1.f };   // Also the incendiary
				inline color_t flash{ 1.f, 1.f, 0.6f, 1.f };
				inline color_t he{ 1.f, 0.25f, 0.25f, 1.f };
				inline color_t decoy{ 0.75f, 0.75f, 0.8f, 1.f };
			}
		}
	}

	namespace settings {
		inline bool watermark = true;
		inline bool streamproof = false;
		inline bool free_cpu = true;
		inline bool frame_times = false; // Not stored, where the time of a frame goes in the watermark (debug builds)
		inline bool notifications = true; // Map building, notices of the program, the ESP off message
		inline color_t accent = { 1.f, 1.f, 1.f, 1.f }; // Menu, white by default
		inline float menu_opacity = 0.86f;	// Of the menu background, the group boxes stay solid
		inline float ui_scale = 1.15f; // Menu size

		// Which debug lines of the console are shown, warnings, errors & info lines always are
		namespace logs {
			inline bool skins = true;	// Skins, knives & agents
			inline bool other = true;	// Every other debug line
		}
	}

	// Not stored, just for testing
	namespace dev {
		inline bool console = true;
		inline int open_menu_key = false;
		inline int cache_refresh_rate = 5;
		inline bool force_show_flags = false;
	}
}