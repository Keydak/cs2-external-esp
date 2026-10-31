// The patterns the emotes need that are not next to their feature code (checked on the 2026-09-25 client).
#pragma once

// client.dll
#define FRAMESTAGENOTIFY_PATTERN   "48 89 5C 24 ? 48 89 6C 24 ? 57 48 83 EC ? 48 8B F9 33 ED"
#define MOUSEINPUTENABLED_PATTERN  "40 53 48 83 EC 20 80 B9 ? ? ? ? ? 48 8B D9 75 78"
#define GETENTITYBYINDEX_PATTERN   "4C 8D 49 10 81 FA FE 7F 00 00 77 47 8B CA C1 F9"
#define GETLOCALCONTROLLER_PATTERN "48 83 EC ? 83 F9 ? 75 ? 48 8B 0D ? ? ? ? 48 8D 54 24 ? ? ? ? FF 90 ? ? ? ? ? ? 48 63 C1 48 8D 0D ? ? ? ? ? ? ? ? 48 83 C4 ? C3 ? ? ? ? ? ? ? ? ? ? ? ? ? 48 83 EC ? 83 F9"
#define LEVELSHUTDOWN_PATTERN      "48 83 EC ? 48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 45 33 C9 45 33 C0 ? ? ? FF 50 ? 48 85 C0 74 ? 48 8B 0D ? ? ? ? 48 8B D0 ? ? ? 41 FF 50 ? 48 83 C4"
#define ONREMOVEENTITY_PATTERN     "48 89 74 24 ? 57 48 83 EC ? 41 B9 ? ? ? ? 41 8B C0 41 23 C1 48 8B F2 41 83 F8 ? 48 8B F9 44 0F 45 C8 41 81 F9 ? ? ? ? 73 ? FF 89"
#define SETLOCALANGLES_PATTERN     "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? F2 0F 10 02 48 8D B9 ? ? ? ? 8B 42 08 48 8B F2 48 8B D9"
#define ENTITYSYSTEM_PATTERN       "48 89 ? ? ? ? ? 4C 63 ? ? ? ? ? 44 3B ? ? ? ? ? 0F"
#define CSGOINPUT_PATTERN          "48 8B 0D ? ? ? ? 4C 8B C6 8B 10 E8"
#define GAMEEVENTMANAGER_PATTERN   "48 8B 0D ? ? ? ? 48 8D 15 ? ? ? ? 45 33 C9 45 33 C0 48 8B 01 FF 50 30"

// engine2.dll
#define ISINGAME_PATTERN "48 8B 05 ? ? ? ? 48 85 C0 74 ? 80 B8 ? ? ? ? 00 75 ? 83 B8 ? ? ? ? ? 7C"

// scenesystem.dll
#define GENERATEPRIMITIVES_PATTERN "48 8B C4 48 89 58 ? 4C 89 40 ? 48 89 50 ? 48 89 48 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 44 0F 29 40"
