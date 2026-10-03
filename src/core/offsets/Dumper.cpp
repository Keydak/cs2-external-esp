#include "Dumper.hpp"

#include "core/engine/Engine.hpp"
#include "updater/http/HttpHelper.hpp"

#include <set>
#include <optional>

namespace {
    const std::string client_dll_url = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/client_dll.json";
    const std::string offsets_url = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/offsets.json";
    const std::string buttons_url = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/buttons.json";
    const std::string info_url = "https://raw.githubusercontent.com/a2x/cs2-dumper/main/output/info.json";

    struct SchemaField {
        std::ptrdiff_t& value;
        const char* klass;
        const char* field;
    };

    // Looks for the field in the class, walking up the parent chain if needed
    std::optional<std::ptrdiff_t> ResolveField(const json& classes, std::string klass, const std::string& field) {
        std::set<std::string> seen;

        while (!klass.empty() && seen.insert(klass).second) {
            auto entry = classes.find(klass);
            if (entry == classes.end())
                return std::nullopt;

            if (auto fields = entry->find("fields"); fields != entry->end() && fields->is_object()) {
                if (auto value = fields->find(field); value != fields->end() && value->is_number_integer())
                    return value->get<std::ptrdiff_t>();
            }

            auto parent = entry->find("parent");
            if (parent == entry->end() || !parent->is_string())
                return std::nullopt;

            klass = parent->get<std::string>();
        }

        return std::nullopt;
    }
}

bool Dumper::Init() {
    return GetInstance().InitImpl();
}

bool Dumper::FetchRemote() {
    return GetInstance().FetchRemoteImpl();
}

bool Dumper::FetchRemoteImpl() {
    LOGF(INFO, "Fetching latest offsets from cs2-dumper...");

    json response;
    auto http_status = HttpHelper::Get(client_dll_url, response);
    if (http_status != 200) {
        LOGF(WARNING, "Failed to fetch client.dll offsets (status {})", http_status);
        return false;
    }

    if (!response.contains("client.dll") || !response["client.dll"].contains("classes")) {
        LOGF(WARNING, "Unexpected client.dll offsets structure, the dumper format may have changed");
        return false;
    }

    const auto& classes = response["client.dll"]["classes"];

    // Same mapping as scripts/update_offsets.py
    const SchemaField fields[] = {
        { offsets::controller::m_iPing,                     "CCSPlayerController",                      "m_iPing" },
        { offsets::controller::m_hPawn,                     "CCSPlayerController",                      "m_hPawn" },
        { offsets::controller::m_steamID,                   "CCSPlayerController",                      "m_steamID" },
        { offsets::controller::m_iszPlayerName,             "CCSPlayerController",                      "m_iszPlayerName" },
        { offsets::controller::m_bIsLocalPlayerController,  "CCSPlayerController",                      "m_bIsLocalPlayerController" },
        { offsets::controller::m_pInGameMoneyServices,      "CCSPlayerController",                      "m_pInGameMoneyServices" },
        { offsets::controller::m_iAccount,                  "CCSPlayerController_InGameMoneyServices",  "m_iAccount" },

        { offsets::pawn::m_vOldOrigin,                      "C_BasePlayerPawn",                         "m_vOldOrigin" },
        { offsets::pawn::m_vecViewOffset,                   "C_BaseModelEntity",                        "m_vecViewOffset" },
        { offsets::pawn::m_bSpotted,                        "EntitySpottedState_t",                     "m_bSpotted" },
        { offsets::pawn::m_iHealth,                         "C_BaseEntity",                             "m_iHealth" },
        { offsets::pawn::m_fFlags,                          "C_BaseEntity",                             "m_fFlags" },
        { offsets::pawn::m_iTeamNum,                        "C_BaseEntity",                             "m_iTeamNum" },
        { offsets::pawn::m_bIsScoped,                       "C_CSPlayerPawn",                           "m_bIsScoped" },
        { offsets::pawn::m_ArmorValue,                      "C_CSPlayerPawn",                           "m_ArmorValue" },
        { offsets::pawn::m_bIsDefusing,                     "C_CSPlayerPawn",                           "m_bIsDefusing" },
        { offsets::pawn::m_pItemServices,                   "C_BasePlayerPawn",                         "m_pItemServices" },
        { offsets::pawn::m_bHasDefuser,                     "CCSPlayer_ItemServices",                   "m_bHasDefuser" },
        { offsets::pawn::m_vecAbsVelocity,                  "C_BaseEntity",                             "m_vecAbsVelocity" },
        { offsets::pawn::m_flSimulationTime,                "C_BaseEntity",                             "m_flSimulationTime" },
        { offsets::pawn::m_pGameSceneNode,                  "C_BaseEntity",                             "m_pGameSceneNode" },
        { offsets::pawn::m_entitySpottedState,              "C_CSPlayerPawn",                           "m_entitySpottedState" },
        { offsets::pawn::m_bSpottedByMask,                  "EntitySpottedState_t",                     "m_bSpottedByMask" },
        { offsets::pawn::m_flFlashOverlayAlpha,             "C_CSPlayerPawn",                           "m_flFlashOverlayAlpha" },
        { offsets::pawn::m_pWeaponServices,                 "C_BasePlayerPawn",                         "m_pWeaponServices" },
        { offsets::pawn::m_hActiveWeapon,                   "CPlayer_WeaponServices",                   "m_hActiveWeapon" },
        { offsets::pawn::m_AttributeManager,                "C_EconEntity",                             "m_AttributeManager" },
        { offsets::pawn::m_Item,                            "C_AttributeContainer",                     "m_Item" },
        { offsets::pawn::m_iItemDefinitionIndex,            "C_EconItemView",                           "m_iItemDefinitionIndex" },
        { offsets::pawn::m_iClip1,                          "C_BasePlayerWeapon",                       "m_iClip1" },
        { offsets::pawn::m_bInReload,                       "C_CSWeaponBase",                           "m_bInReload" },
        { offsets::pawn::m_pObserverServices,               "C_BasePlayerPawn",                         "m_pObserverServices" },

        { offsets::bomb::m_bC4Activated,                    "C_PlantedC4",                              "m_bC4Activated" },
        { offsets::bomb::m_nBombSite,                       "C_PlantedC4",                              "m_nBombSite" },
        { offsets::bomb::m_flC4Blow,                        "C_PlantedC4",                              "m_flC4Blow" },
        { offsets::bomb::m_flTimerLength,                   "C_PlantedC4",                              "m_flTimerLength" },
        { offsets::bomb::m_bBeingDefused,                   "C_PlantedC4",                              "m_bBeingDefused" },
        { offsets::bomb::m_flDefuseLength,                  "C_PlantedC4",                              "m_flDefuseLength" },
        { offsets::bomb::m_flDefuseCountDown,               "C_PlantedC4",                              "m_flDefuseCountDown" },
        { offsets::bomb::m_bBombDefused,                    "C_PlantedC4",                              "m_bBombDefused" },
        { offsets::bomb::m_bHasExploded,                    "C_PlantedC4",                              "m_bHasExploded" },
        { offsets::bomb::m_vecAbsOrigin,                    "CGameSceneNode",                           "m_vecAbsOrigin" },

        { offsets::view::m_pCameraServices,                 "C_BasePlayerPawn",                         "m_pCameraServices" },
        { offsets::view::m_iFOV,                            "CCSPlayerBase_CameraServices",             "m_iFOV" },
        { offsets::view::m_iFOVStart,                       "CCSPlayerBase_CameraServices",             "m_iFOVStart" },

        { offsets::econ::m_iMusicKitID,                    "CCSPlayerController",                      "m_iMusicKitID" },
        { offsets::grenade::m_designerName,                 "CEntityIdentity",                          "m_designerName" },
        { offsets::grenade::m_pCollision,                   "C_BaseEntity",                             "m_pCollision" },
        { offsets::grenade::m_vecMins,                      "CCollisionProperty",                       "m_vecMins" },
        { offsets::grenade::m_vecMaxs,                      "CCollisionProperty",                       "m_vecMaxs" },
        { offsets::grenade::m_usSolidFlags,                 "CCollisionProperty",                       "m_usSolidFlags" },
        { offsets::grenade::m_nSolidType,                   "CCollisionProperty",                       "m_nSolidType" },
        { offsets::grenade::m_angAbsRotation,               "CGameSceneNode",                           "m_angAbsRotation" },
        { offsets::grenade::m_bDidSmokeEffect,              "C_SmokeGrenadeProjectile",                 "m_bDidSmokeEffect" },
        { offsets::grenade::m_vSmokeDetonationPos,          "C_SmokeGrenadeProjectile",                 "m_vSmokeDetonationPos" },
        { offsets::grenade::m_firePositions,                "C_Inferno",                                "m_firePositions" },
        { offsets::grenade::m_bFireIsBurning,               "C_Inferno",                                "m_bFireIsBurning" },
        { offsets::grenade::m_fireCount,                    "C_Inferno",                                "m_fireCount" },
        { offsets::grenade::m_nFireLifetime,                "C_Inferno",                                "m_nFireLifetime" },
        { offsets::grenade::m_maxFireHalfWidth,             "C_Inferno",                                "m_maxFireHalfWidth" },
        { offsets::grenade::m_bExplodeEffectBegan,          "C_BaseCSGrenadeProjectile",                "m_bExplodeEffectBegan" },
        { offsets::grenade::m_nExplodeEffectTickBegin,      "C_BaseCSGrenadeProjectile",                "m_nExplodeEffectTickBegin" },
        { offsets::grenade::m_bPinPulled,                   "C_BaseCSGrenade",                          "m_bPinPulled" },
        { offsets::grenade::m_flThrowStrength,              "C_BaseCSGrenade",                          "m_flThrowStrength" },
        { offsets::grenade::m_angEyeAngles,                 "C_CSPlayerPawn",                           "m_angEyeAngles" },
        { offsets::grenade::m_vInitialPosition,             "C_BaseCSGrenadeProjectile",                "m_vInitialPosition" },
        { offsets::grenade::m_vInitialVelocity,             "C_BaseCSGrenadeProjectile",                "m_vInitialVelocity" },

        { offsets::econ::m_hMyWeapons,                      "CPlayer_WeaponServices",                   "m_hMyWeapons" },
        { offsets::econ::m_AttributeList,                   "C_EconItemView",                           "m_AttributeList" },
        { offsets::econ::m_Attributes,                      "CAttributeList",                           "m_Attributes" },
        { offsets::econ::m_iItemIDHigh,                     "C_EconItemView",                           "m_iItemIDHigh" },
        { offsets::econ::m_iAccountID,                      "C_EconItemView",                           "m_iAccountID" },
        { offsets::econ::m_iEntityQuality,                  "C_EconItemView",                           "m_iEntityQuality" },
        { offsets::econ::m_bInitialized,                    "C_EconItemView",                           "m_bInitialized" },
        { offsets::econ::m_nFallbackPaintKit,               "C_EconEntity",                             "m_nFallbackPaintKit" },
        { offsets::econ::m_nFallbackSeed,                   "C_EconEntity",                             "m_nFallbackSeed" },
        { offsets::econ::m_flFallbackWear,                  "C_EconEntity",                             "m_flFallbackWear" },
        { offsets::econ::m_nFallbackStatTrak,               "C_EconEntity",                             "m_nFallbackStatTrak" },
        { offsets::econ::m_OriginalOwnerXuidLow,            "C_EconEntity",                             "m_OriginalOwnerXuidLow" },
        { offsets::econ::m_EconGloves,                      "C_CSPlayerPawn",                           "m_EconGloves" },
        { offsets::econ::m_bNeedToReApplyGloves,            "C_CSPlayerPawn",                           "m_bNeedToReApplyGloves" },
        { offsets::econ::m_hHudModelArms,                   "C_CSPlayerPawn",                           "m_hHudModelArms" },
        { offsets::econ::m_MeshGroupMask,                   "CModelState",                              "m_MeshGroupMask" },
        { offsets::econ::m_pChild,                          "CGameSceneNode",                           "m_pChild" },
        { offsets::econ::m_pNextSibling,                    "CGameSceneNode",                           "m_pNextSibling" },
        { offsets::econ::m_pOwner,                          "CGameSceneNode",                           "m_pOwner" },
        { offsets::econ::m_hOwnerEntity,                    "C_BaseEntity",                             "m_hOwnerEntity" },
        { offsets::econ::m_pInventoryServices,              "CCSPlayerController",                      "m_pInventoryServices" },
        { offsets::econ::m_fEffects,                        "C_BaseEntity",                             "m_fEffects" },
        { offsets::econ::m_ModelName,                       "CModelState",                              "m_ModelName" },
        { offsets::econ::m_nSubclassID,                     "C_BaseEntity",                             "m_nSubclassID" },
        { offsets::econ::m_unMusicID,                       "CCSPlayerController_InventoryServices",    "m_unMusicID" },

        { offsets::rules::m_pGameModeRules,                "C_CSGameRules",                            "m_pGameModeRules" },

        { offsets::bone::m_modelState,                      "CSkeletonInstance",                        "m_modelState" },

        { offsets::observerServices::m_iObserverMode,       "CPlayer_ObserverServices",                 "m_iObserverMode" },
        { offsets::observerServices::m_hObserverTarget,     "CPlayer_ObserverServices",                 "m_hObserverTarget" },
    };

    int updated = 0, unresolved = 0;
    for (const auto& f : fields) {
        auto value = ResolveField(classes, f.klass, f.field);

        if (!value) {
            LOGF(WARNING, "Could not resolve '{}::{}', keeping built-in value 0x{:X}", f.klass, f.field, f.value);
            unresolved++;
            continue;
        }

        if (*value != f.value) {
            LOGF(VERBOSE, "Updated '{}::{}' 0x{:X} -> 0x{:X}", f.klass, f.field, f.value, *value);
            f.value = *value;
            updated++;
        }
    }

    json globals;
    if (HttpHelper::Get(offsets_url, globals) == 200 && globals.contains("engine2.dll")) {
        const auto& engine2 = globals["engine2.dll"];

        const std::pair<std::ptrdiff_t&, const char*> network_fields[] = {
            { offsets::network::dwNetworkGameClient, "dwNetworkGameClient" },
            { offsets::network::deltaTick, "dwNetworkGameClient_deltaTick" },
        };

        for (const auto& [value, name] : network_fields) {
            auto remote = engine2.value(name, std::ptrdiff_t{ 0 });

            if (!remote) {
                LOGF(WARNING, "Could not resolve 'engine2.dll::{}', keeping built-in value 0x{:X}", name, value);
                unresolved++;
            }
            else if (remote != value) {
                LOGF(VERBOSE, "Updated 'engine2.dll::{}' 0x{:X} -> 0x{:X}", name, value, remote);
                value = remote;
                updated++;
            }
        }
    }

    if (globals.contains("client.dll")) {
        auto remote = globals["client.dll"].value("dwGameRules", std::ptrdiff_t{ 0 });

        if (!remote) {
            LOGF(WARNING, "Could not resolve 'client.dll::dwGameRules', keeping built-in value 0x{:X}", offsets::rules::dwGameRules);
            unresolved++;
        }
        else if (remote != offsets::rules::dwGameRules) {
            LOGF(VERBOSE, "Updated 'client.dll::dwGameRules' 0x{:X} -> 0x{:X}", offsets::rules::dwGameRules, remote);
            offsets::rules::dwGameRules = remote;
            updated++;
        }
    }
    else {
        LOGF(WARNING, "Failed to fetch global offsets, keeping built-in network client offsets");
        unresolved++;
    }

    json buttons;
    if (HttpHelper::Get(buttons_url, buttons) == 200 && buttons.contains("client.dll")) {
        const std::pair<std::ptrdiff_t&, const char*> button_fields[] = {
            { offsets::buttons::jump,       "jump" },
            { offsets::buttons::forward,    "forward" },
            { offsets::buttons::back,       "back" },
            { offsets::buttons::left,       "left" },
            { offsets::buttons::right,      "right" },
        };

        for (const auto& [offset, name] : button_fields) {
            auto value = buttons["client.dll"].value(name, std::ptrdiff_t{ 0 });

            if (!value) {
                LOGF(WARNING, "Could not resolve 'buttons::{}', keeping built-in value 0x{:X}", name, offset);
                unresolved++;
            }
            else if (value != offset) {
                LOGF(VERBOSE, "Updated 'buttons::{}' 0x{:X} -> 0x{:X}", name, offset, value);
                offset = value;
                updated++;
            }
        }
    }
    else {
        LOGF(WARNING, "Failed to fetch button offsets, keeping built-in values");
        unresolved++;
    }

    json info;
    if (HttpHelper::Get(info_url, info) == 200)
        LOGF(INFO, "Remote offsets were dumped for game build {}", info.value("build_number", 0));

    LOGF(INFO, "Successfully fetched remote offsets, {} updated, {} unresolved", updated, unresolved);
    return true;
}

bool Dumper::InitImpl() {
    auto process = Engine::GetProcess();
    auto client = Engine::GetClient();
    auto engine = Engine::GetEngine();

    DWORD64 temp = 0;

    // client.dll

    // View Matrix
    if (!(temp = Scan(offsets::signatures::viewMatrix, client))) {
        LOGF(FATAL, "Could not find offset for 'viewMatrix'");
        return false;
    }

    offsets::viewMatrix = temp - client.base;
    LOGF(VERBOSE, "Found 'viewMatrix' offset at 0x{:X}", offsets::viewMatrix);

    // Global Variables
    if (!(temp = Scan(offsets::signatures::globalVars, client))) {
        LOGF(FATAL, "Could not find offset for 'globalVars'");
        return false;
    }

    offsets::globalVars = temp - client.base;
    LOGF(VERBOSE, "Found 'globalVars' offset at 0x{:X}", offsets::globalVars);

    // Entity List
    if (!(temp = Scan(offsets::signatures::entityList, client))) {
        LOGF(FATAL, "Could not find offset for 'entityList'");
        return false;
    }

    offsets::entityList = temp - client.base;
    LOGF(VERBOSE, "Found 'entityList' offset at 0x{:X}", offsets::entityList);

    // Local Player Controller
    if (!(temp = Scan(offsets::signatures::localPlayerController, client))) {
        LOGF(FATAL, "Could not find offset for 'localPlayerController'");
        return false;
    }

    offsets::localPlayerController = temp - client.base;
    LOGF(VERBOSE, "Found 'localPlayerController' offset at 0x{:X}", offsets::localPlayerController);

    // C4
    if (!(temp = Scan(offsets::signatures::plantedC4, client))) {
        LOGF(FATAL, "Could not find offset for 'weaponC4'");
        return false;
    }

    offsets::plantedC4 = temp - client.base;
    LOGF(VERBOSE, "Found 'weaponC4' offset at 0x{:X}", offsets::plantedC4);

    // C4 carrier pointer
    if (!(temp = Scan(offsets::signatures::weaponC4, client))) {
        LOGF(FATAL, "Could not find offset for 'weaponC4 carrier'");
        return false;
    }

    offsets::weaponC4 = temp - client.base;
    LOGF(VERBOSE, "Found 'weaponC4 carrier' offset at 0x{:X}", offsets::weaponC4);

#if 0
    // Local Player Pawn (tbh idk how to read it :1)
    if (temp = Scan(offsets::signatures::localPlayerPawn, client); !temp) {
        LOGF(FATAL, "Could not find offset for 'localPlayerPawn'");
        return false;
    }

    offsets::localPlayerPawn = temp + 0x138 - client.base;
    LOGF(VERBOSE, "Found 'localPlayerPawn' offset at 0x{:X}", offsets::localPlayerPawn);
 

    // Input
    if (temp = Scan(offsets::signatures::csgoInput, client); !temp) {
        LOGF(FATAL, "Could not find offset for 'csgoInput'");
        return false;
    }

    offsets::csgoInput = temp - client.base;
    LOGF(VERBOSE, "Found 'csgoInput' offset at 0x{:X}", offsets::csgoInput);
#endif

    // CSGOInput & its third person fields, optional
    if (!ResolveThirdPerson(client))
        LOGF(WARNING, "Could not find the third person code, third person is disabled");

    // Skin regeneration, optional
    if (!ResolveSkins(client))
        LOGF(WARNING, "Could not find the skin regeneration code, skin changer is disabled");

    // server.dll, optional: only the MVP anthem needs it. Loaded with the first map, so tried again later
    ResolveServer();

    // engine2.dll

    // Build Number
    if (!(temp = Scan(offsets::signatures::buildNumber, engine))) {
        LOGF(FATAL, "Could not find offset for 'buildNumber'");
        return false;
    }

    offsets::buildNumber = temp - engine.base;
    LOGF(VERBOSE, "Found 'buildNumber' offset at 0x{:X}", offsets::buildNumber);

    LOGF(INFO, "Successfully dumped offsets...");

    return true;
}

bool Dumper::ResolveServer() {
    if (offsets::econ::server_m_iMusicKitID)
        return true;

    auto process = Engine::GetProcess();
    auto server = process->GetModule("server.dll");
    if (!server.base)
        return false;

    auto matches = GetInstance().ScanMemory(offsets::signatures::serverMusicKit, server.base, server.base + server.size);
    if (matches.empty())
        return false;

    offsets::econ::server_m_iMusicKitID = process->read<int32_t>(matches.at(0) + 2);
    LOGF(VERBOSE, "Found server 'm_iMusicKitID' at 0x{:X}", offsets::econ::server_m_iMusicKitID);
    return offsets::econ::server_m_iMusicKitID != 0;
}

bool Dumper::ResolveThirdPerson(ProcessModule client) {
    auto process = Engine::GetProcess();

    auto matches = ScanMemory(offsets::signatures::thirdPerson, client.base, client.base + 0x4000000);
    if (matches.empty())
        return false;

    uint8_t code[128]{};
    if (!process->read_raw(matches.at(0), code, sizeof(code)))
        return false;

    // Displacement of the instruction at "at", if it starts with the expected opcode
    auto displacement = [&](size_t at, std::initializer_list<uint8_t> opcode, std::ptrdiff_t& out) {
        if (!std::equal(opcode.begin(), opcode.end(), code + at))
            return false;

        out = *reinterpret_cast<int32_t*>(code + at + opcode.size());
        return true;
    };

    std::ptrdiff_t global = 0;
    std::ptrdiff_t view_angles = 0, third_person = 0, camera = 0;

    bool found = displacement(0, { 0x4C, 0x8B, 0x05 }, global)
        && displacement(48, { 0x49, 0x8D, 0x80 }, view_angles)          // lea rax, [r8 + view angles]
        && displacement(55, { 0x41, 0x80, 0xB8 }, third_person)         // cmp byte ptr [r8 + third person], 0
        && displacement(104, { 0xF3, 0x41, 0x0F, 0x11, 0x88 }, camera); // movss [r8 + camera pitch], xmm1

    if (!found)
        return false;

    offsets::input::dwCSGOInput = matches.at(0) + global + 7 - client.base;
    offsets::input::m_angViewAngles = view_angles;
    offsets::input::m_bInThirdPerson = third_person;
    offsets::input::m_vecCameraOffset = camera;

    LOGF(VERBOSE, "Found 'dwCSGOInput' at 0x{:X}, third person 0x{:X}, camera 0x{:X}, view angles 0x{:X}",
        offsets::input::dwCSGOInput, third_person, camera, view_angles);

    // sv_cheats check of the camera, optional: without it third person needs sv_cheats 1
    constexpr size_t jump_at = 40;
    auto checks = ScanMemory(offsets::signatures::thirdPersonCheats, client.base, client.base + 0x4000000);

    if (!checks.empty() && process->read<uint8_t>(checks.at(0) + jump_at) == 0x75) {
        offsets::input::cheatsCheckJump = checks.at(0) + jump_at - client.base;
        LOGF(VERBOSE, "Found the third person sv_cheats check at 0x{:X}", offsets::input::cheatsCheckJump);
    }
    else {
        LOGF(WARNING, "Could not find the third person sv_cheats check, third person needs sv_cheats 1");
    }
    return true;
}

bool Dumper::ResolveSkins(ProcessModule client) {
    auto process = Engine::GetProcess();

    auto matches = ScanMemory(offsets::signatures::regenerateWeaponSkins, client.base, client.base + 0x4000000);
    if (matches.size() != 1)
        return false;

    auto function = matches.at(0);

    // lea rcx, [rbx + composite owner]; mov dl, 1; call clear; xor edx, edx; mov rcx, rbx; call update
    uint8_t code[24]{};
    if (!process->read_raw(function + 0x72, code, sizeof(code)))
        return false;

    constexpr uint8_t lea[] = { 0x48, 0x8D, 0x8B };
    constexpr uint8_t update_call[] = { 0x33, 0xD2, 0x48, 0x8B, 0xCB, 0xE8 };

    if (!std::equal(std::begin(lea), std::end(lea), code) || code[7] != 0xB2 || code[9] != 0xE8 ||
        !std::equal(std::begin(update_call), std::end(update_call), code + 14))
        return false;

    auto composite_owner = *reinterpret_cast<int32_t*>(code + 3);
    auto clear = function + 0x72 + 14 + *reinterpret_cast<int32_t*>(code + 10);
    auto update = function + 0x72 + 24 + *reinterpret_cast<int32_t*>(code + 20);

    // mov rsi, [rdi + materials] in the clear function
    uint8_t clear_code[0x100]{};
    if (!process->read_raw(clear, clear_code, sizeof(clear_code)))
        return false;

    // movsxd r8, [rdi + pending] in the clear function
    std::ptrdiff_t composite_materials = 0, composite_pending = 0;
    for (size_t i = 0; i + 7 <= sizeof(clear_code); i++) {
        if (!composite_materials && clear_code[i] == 0x48 && clear_code[i + 1] == 0x8B && clear_code[i + 2] == 0xB7)
            composite_materials = *reinterpret_cast<int32_t*>(clear_code + i + 3);

        if (!composite_pending && clear_code[i] == 0x4C && clear_code[i + 1] == 0x63 && clear_code[i + 2] == 0x87)
            composite_pending = *reinterpret_cast<int32_t*>(clear_code + i + 3);
    }

    if (!composite_materials || !composite_pending)
        return false;

    offsets::skins::regenerateWeaponSkins = function - client.base;
    offsets::skins::clearMaterials = clear - client.base;
    offsets::skins::updateWeaponSkin = update - client.base;
    offsets::skins::compositeOwner = composite_owner;
    offsets::skins::compositeMaterials = composite_materials;
    offsets::skins::compositePending = composite_pending;

    // First person skin, optional: without it the skin shows after switching weapons
    auto viewmodel = ScanMemory(offsets::signatures::updateViewmodelSkin, client.base, client.base + 0x4000000);
    if (viewmodel.size() == 1) {
        offsets::skins::updateViewmodelSkin = viewmodel.at(0) - client.base;
        offsets::skins::viewmodelSkinBuilt = process->read<int32_t>(viewmodel.at(0) + 20);

        LOGF(VERBOSE, "Found the first person skin update at 0x{:X}, built flag 0x{:X}",
            offsets::skins::updateViewmodelSkin, offsets::skins::viewmodelSkinBuilt);
    }
    else {
        LOGF(WARNING, "Could not find the first person skin update, skins show after switching weapons");
    }

    // Mesh switch for skins made for the old models, optional
    auto mesh = ScanMemory(offsets::signatures::setMeshGroupMask, client.base, client.base + 0x4000000);
    if (mesh.size() == 1) {
        offsets::skins::setMeshGroupMask = mesh.at(0) - client.base;
        LOGF(VERBOSE, "Found 'SetMeshGroupMask' at 0x{:X}", offsets::skins::setMeshGroupMask);
    }
    else {
        LOGF(WARNING, "Could not find 'SetMeshGroupMask', skins for the old models might look wrong");
    }

    // Agents, optional
    auto set_model = ScanMemory(offsets::signatures::setModel, client.base, client.base + 0x4000000);
    if (set_model.size() == 1) {
        offsets::skins::setModel = set_model.at(0) - client.base;
        LOGF(VERBOSE, "Found 'SetModel' at 0x{:X}", offsets::skins::setModel);
    }
    else {
        LOGF(WARNING, "Could not find 'SetModel', agents are disabled");
    }

    // Knives, optional
    auto subclass_changed = ScanMemory(offsets::signatures::subclassChanged, client.base, client.base + 0x4000000);
    if (subclass_changed.size() == 1) {
        offsets::skins::subclassChanged = subclass_changed.at(0) - client.base;
        LOGF(VERBOSE, "Found 'OnSubclassIDChanged' at 0x{:X}", offsets::skins::subclassChanged);
    }
    else {
        LOGF(WARNING, "Could not find 'OnSubclassIDChanged', knives are disabled");
    }

    // Glove model loading, optional: without it gloves only show when their model was already loaded
    auto gloves = ScanMemory(offsets::signatures::applyGlovesCall, client.base, client.base + 0x4000000);
    if (gloves.size() == 1) {
        uint8_t call[12]{};
        process->read_raw(gloves.at(0), call, sizeof(call));

        auto helper = *reinterpret_cast<int32_t*>(call + 3);
        auto apply = gloves.at(0) + 12 + *reinterpret_cast<int32_t*>(call + 8);

        // "call ModelHasOwnGloves; test al, al; je keep; cmp byte ptr [pawn + reapply], 0" at +0x42
        uint8_t remove[16]{};
        process->read_raw(apply + 0x42, remove, sizeof(remove));

        if (remove[0] == 0xE8 && remove[5] == 0x84 && remove[6] == 0xC0 && remove[7] == 0x74 && remove[9] == 0x80 && remove[10] == 0xBF) {
            offsets::skins::gloveRemoveJump = apply + 0x49 - client.base;
            LOGF(VERBOSE, "Found the glove removal check at 0x{:X}", offsets::skins::gloveRemoveJump);
        }

        // "mov dl, 1; mov rcx, rdi; call ShowDefaultGloves" at +0x581, when the glove entity could not be made
        uint8_t show[10]{};
        process->read_raw(apply + 0x581, show, sizeof(show));

        constexpr uint8_t show_call[] = { 0xB2, 0x01, 0x48, 0x8B, 0xCF, 0xE8 };
        if (std::equal(std::begin(show_call), std::end(show_call), show)) {
            offsets::skins::showDefaultGloves = apply + 0x581 + 10 + *reinterpret_cast<int32_t*>(show + 6) - client.base;
            LOGF(VERBOSE, "Found 'ShowDefaultGloves' at 0x{:X}", offsets::skins::showDefaultGloves);
        }

        // "cmp [rax], r13b; je ...; call ShouldPrecache" at +0x1DC, ShouldPrecache is "movzx eax, byte ptr [flag]; ret"
        uint8_t check[14]{}, flag[8]{};
        process->read_raw(apply + 0x1DC, check, sizeof(check));

        constexpr uint8_t expected[] = { 0x44, 0x38, 0x28, 0x0F, 0x84 };
        if (std::equal(std::begin(expected), std::end(expected), check) && check[9] == 0xE8) {
            auto should_precache = apply + 0x1DC + 14 + *reinterpret_cast<int32_t*>(check + 10);
            process->read_raw(should_precache, flag, sizeof(flag));

            if (flag[0] == 0x0F && flag[1] == 0xB6 && flag[2] == 0x05 && flag[7] == 0xC3) {
                offsets::skins::gloveHelper = helper;
                offsets::skins::precacheGloves = should_precache + 7 + *reinterpret_cast<int32_t*>(flag + 3) - client.base;

                LOGF(VERBOSE, "Found the glove code, helper 0x{:X}, precache flag 0x{:X}", helper, offsets::skins::precacheGloves);
            }
        }
    }

    if (!offsets::skins::precacheGloves)
        LOGF(WARNING, "Could not find the glove model loading, custom gloves might not show");

    LOGF(VERBOSE, "Found 'RegenerateWeaponSkins' at 0x{:X}, clear 0x{:X}, update 0x{:X}, composite owner 0x{:X}, materials 0x{:X}",
        offsets::skins::regenerateWeaponSkins, offsets::skins::clearMaterials, offsets::skins::updateWeaponSkin, composite_owner, composite_materials);
    return true;
}

DWORD64 Dumper::Scan(const std::string sig, ProcessModule module) {
    auto process = Engine::GetProcess();

    if (!process)
        return 0;

    DWORD offsets = 0;
    DWORD64 address = 0;
    std::vector<DWORD64> list;

    //list = process->FindSignature(module, sig.data());
    list = ScanMemory(sig, module.base, module.base + 0x4000000);

    if (!list.size())
        return 0;

    if (!process->read_raw(list.at(0) + 3, &offsets, sizeof(DWORD)))
        return 0;

    address = list.at(0) + offsets + 7;
    return address;
}

std::vector<WORD> Dumper::StrSigToArray(const std::string& sig) {
    std::istringstream iss(sig);
    std::vector<WORD> bytes;
    std::string byte_str;

    while (iss >> byte_str) {
        if (byte_str == "??" || byte_str == "?")
            bytes.push_back(256);
        else
            bytes.push_back(static_cast<WORD>(std::stoul(byte_str, nullptr, 16)));
    }
    return bytes;
}

void Dumper::GetNextArray(std::vector<short>& next, const std::vector<WORD>& signature)
{
    auto size = signature.size();
    for (int i = 0; i < size; i++)
        next[signature[i]] = i;
}

void Dumper::ScanBlock(byte* buffer, const std::vector<short>& next, const std::vector<WORD>& signature, DWORD64 start, DWORD size, std::vector<DWORD64>& result)
{
    auto process = Engine::GetProcess();

    if (!process->read_raw(start, buffer, size))
        return;

    int length = signature.size();

    for (int i = 0, j, k; i < size;)
    {
        j = i; k = 0;

        for (; k < length && j < size && (signature[k] == buffer[j] || signature[k] == 256); k++, j++);

        if (k == length)
            result.push_back(start + i);

        if ((i + length) >= size)
            return;

        int Num = next[buffer[i + length]];
        if (Num == -1)
            i += (length - next[256]);
        else
            i += (length - Num);
    }
}

std::vector<DWORD64> Dumper::ScanMemory(const std::string& sig, DWORD64 start, DWORD64 end, int number)
{
    std::vector<DWORD64> result;
    std::vector<short> next(260, -1);

    auto process = Engine::GetProcess();

    if (!process)
        return result;

    byte* buffer = new byte[MAX_BLOCK_SIZE];

    auto signature = StrSigToArray(sig);
    if (!signature.size())
        return result;

    GetNextArray(next, signature);

    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(process->handle_, reinterpret_cast<LPCVOID>(start), &mbi, sizeof(mbi)) != 0)
    {
        int searches = 0;
        auto size = mbi.RegionSize;

        while (size >= MAX_BLOCK_SIZE)
        {
            if (result.size() >= number) {
                delete[] buffer;
	            return result;
            }

            ScanBlock(buffer, next, signature, start + (MAX_BLOCK_SIZE * searches), MAX_BLOCK_SIZE, result);

            size -= MAX_BLOCK_SIZE;
            searches++;
        }

        ScanBlock(buffer, next, signature, start + (MAX_BLOCK_SIZE * searches), size, result);

        start += mbi.RegionSize;

        if (result.size() >= number || end != 0 && start > end)
            break;
    }

	delete[] buffer;
	return result;
}