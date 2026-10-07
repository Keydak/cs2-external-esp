#include "Dumper.hpp"

#include "core/engine/Engine.hpp"
#include "updater/http/HttpHelper.hpp"

#include <algorithm>
#include <format>
#include <set>
#include <optional>
#include <functional>
#include <unordered_map>

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

    // The schema fields we use, looked up in cs2-dumper & then in the game itself. Same mapping as scripts/update_offsets.py
    const std::vector<SchemaField>& SchemaFields() {
        static const std::vector<SchemaField> fields = {
            { offsets::controller::m_iPing,                     "CCSPlayerController",                      "m_iPing" },
            { offsets::controller::m_hPawn,                     "CCSPlayerController",                      "m_hPawn" },
            { offsets::controller::m_steamID,                   "CCSPlayerController",                      "m_steamID" },
            { offsets::controller::m_iszPlayerName,             "CCSPlayerController",                      "m_iszPlayerName" },
            { offsets::controller::m_sSanitizedClanTag,         "CCSPlayerController",                      "m_sSanitizedClanTag" },
            { offsets::controller::m_szClan,                    "CCSPlayerController",                      "m_szClan" },
            { offsets::controller::m_bIsLocalPlayerController,  "CCSPlayerController",                      "m_bIsLocalPlayerController" },
            { offsets::controller::m_iCompetitiveRanking,       "CCSPlayerController",                      "m_iCompetitiveRanking" },
            { offsets::controller::m_iCompetitiveWins,          "CCSPlayerController",                      "m_iCompetitiveWins" },
            { offsets::controller::m_iCompetitiveRankType,      "CCSPlayerController",                      "m_iCompetitiveRankType" },
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

            { offsets::hits::m_pBulletServices,                 "C_CSPlayerPawn",                           "m_pBulletServices" },
            { offsets::hits::m_totalHitsOnServer,               "CCSPlayer_BulletServices",                 "m_totalHitsOnServer" },
            { offsets::hits::m_pActionTrackingServices,         "CCSPlayerController",                      "m_pActionTrackingServices" },
            { offsets::hits::m_iNumRoundKills,                  "CCSPlayerController_ActionTrackingServices", "m_iNumRoundKills" },

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
            { offsets::votes::m_iActiveIssueIndex,              "C_VoteController",                         "m_iActiveIssueIndex" },
            { offsets::votes::m_iOnlyTeamToVote,                "C_VoteController",                         "m_iOnlyTeamToVote" },
            { offsets::votes::m_nVoteOptionCount,               "C_VoteController",                         "m_nVoteOptionCount" },
            { offsets::votes::m_nPotentialVotes,                "C_VoteController",                         "m_nPotentialVotes" },
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

            { offsets::visuals::m_flFlashMaxAlpha,              "C_CSPlayerPawnBase",                       "m_flFlashMaxAlpha" },
            { offsets::visuals::m_bGunGameImmunity,             "C_CSPlayerPawn",                           "m_bGunGameImmunity" },
            { offsets::visuals::m_bSmokeEffectSpawned,          "C_SmokeGrenadeProjectile",                 "m_bSmokeEffectSpawned" },
            { offsets::visuals::m_zoomLevel,                    "C_CSWeaponBaseGun",                        "m_zoomLevel" },
            { offsets::visuals::m_clrRender,                    "C_BaseModelEntity",                        "m_clrRender" },
            { offsets::visuals::m_Glow,                         "C_BaseModelEntity",                        "m_Glow" },
            { offsets::visuals::m_iGlowType,                    "CGlowProperty",                            "m_iGlowType" },
            { offsets::visuals::m_glowColorOverride,            "CGlowProperty",                            "m_glowColorOverride" },
            { offsets::visuals::m_bGlowing,                     "CGlowProperty",                            "m_bGlowing" },
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
            { offsets::rules::m_gamePhase,                     "C_CSGameRules",                            "m_gamePhase" },

            { offsets::bone::m_modelState,                      "CSkeletonInstance",                        "m_modelState" },

            { offsets::observerServices::m_iObserverMode,       "CPlayer_ObserverServices",                 "m_iObserverMode" },
            { offsets::observerServices::m_hObserverTarget,     "CPlayer_ObserverServices",                 "m_hObserverTarget" },

            { offsets::jump::m_pMovementServices,               "C_BasePlayerPawn",                         "m_pMovementServices" },
            { offsets::jump::m_nTickBase,                       "CBasePlayerController",                    "m_nTickBase" },
            { offsets::jump::m_ModernJump,                      "CCSPlayer_MovementServices",               "m_ModernJump" },
            { offsets::jump::m_nLastLandedTick,                 "CCSPlayerModernJump",                      "m_nLastLandedTick" },
            { offsets::jump::m_flLastLandedFrac,                "CCSPlayerModernJump",                      "m_flLastLandedFrac" },
            { offsets::jump::m_nLastJumpTick,                   "CCSPlayer_MovementServices",               "m_nLastJumpTick" },
            { offsets::jump::m_flLastJumpFrac,                 "CCSPlayer_MovementServices",               "m_flLastJumpFrac" },
            { offsets::jump::m_flLastJumpVelocityZ,             "CCSPlayer_MovementServices",               "m_flLastJumpVelocityZ" },
            { offsets::jump::m_flHeightAtJumpStart,             "CCSPlayer_MovementServices",               "m_flHeightAtJumpStart" },
            { offsets::jump::m_flFallVelocity,                  "CPlayer_MovementServices_Humanoid",        "m_flFallVelocity" },
            { offsets::jump::m_flDuckSpeed,                    "CCSPlayer_MovementServices",               "m_flDuckSpeed" },
            { offsets::jump::m_flLastDuckTime,                  "CCSPlayer_MovementServices",               "m_flLastDuckTime" },
            { offsets::jump::m_flGravityScale,                  "C_BaseEntity",                             "m_flGravityScale" },
        };
        return fields;
    }

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

// Something of the game we need was not found in this build: logged, & the program stops before it runs on it
template<typename... Args>
void Dumper::Missing(std::format_string<Args...> format, Args&&... args) {
    auto text = std::format(format, std::forward<Args>(args)...);
    LOGF(WARNING, "{}", text);
    this->unverified.push_back(std::move(text));
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


    int updated = 0, unresolved = 0;
    for (const auto& f : SchemaFields()) {
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
        Missing("Could not find offset for 'viewMatrix'");
        return false;
    }

    offsets::viewMatrix = temp - client.base;
    LOGF(VERBOSE, "Found 'viewMatrix' offset at 0x{:X}", offsets::viewMatrix);

    // Global Variables
    if (!(temp = Scan(offsets::signatures::globalVars, client))) {
        Missing("Could not find offset for 'globalVars'");
        return false;
    }

    offsets::globalVars = temp - client.base;
    LOGF(VERBOSE, "Found 'globalVars' offset at 0x{:X}", offsets::globalVars);

    // Entity List
    if (!(temp = Scan(offsets::signatures::entityList, client))) {
        Missing("Could not find offset for 'entityList'");
        return false;
    }

    offsets::entityList = temp - client.base;
    LOGF(VERBOSE, "Found 'entityList' offset at 0x{:X}", offsets::entityList);

    // Game rules, from the game itself: cs2-dumper can still be on the build before an update. Without it, the
    // remote or built-in value
    if ((temp = Scan(offsets::signatures::gameRules, client))) {
        offsets::rules::dwGameRules = temp - client.base;
        LOGF(VERBOSE, "Found 'dwGameRules' offset at 0x{:X}", offsets::rules::dwGameRules);
    } else {
        Missing("Could not find 'dwGameRules', using 0x{:X}: deathmatch might not be detected", offsets::rules::dwGameRules);
    }

    // Field offsets from the game itself: cs2-dumper can be on the build before an update
    ResolveSchema(client);

    // Input buttons, written by the movement features: from the game, the remote ones can be of the build before
    if (!ResolveButtons(client)) {
        Missing("Could not find every input button, using the remote values: movement might press the wrong thing");
    }

    // Local Player Controller
    if (!(temp = Scan(offsets::signatures::localPlayerController, client))) {
        Missing("Could not find offset for 'localPlayerController'");
        return false;
    }

    offsets::localPlayerController = temp - client.base;
    LOGF(VERBOSE, "Found 'localPlayerController' offset at 0x{:X}", offsets::localPlayerController);

    // C4
    if (!(temp = Scan(offsets::signatures::plantedC4, client))) {
        Missing("Could not find offset for 'weaponC4'");
        return false;
    }

    offsets::plantedC4 = temp - client.base;
    LOGF(VERBOSE, "Found 'weaponC4' offset at 0x{:X}", offsets::plantedC4);

    // C4 carrier pointer
    if (!(temp = Scan(offsets::signatures::weaponC4, client))) {
        Missing("Could not find offset for 'weaponC4 carrier'");
        return false;
    }

    offsets::weaponC4 = temp - client.base;
    LOGF(VERBOSE, "Found 'weaponC4 carrier' offset at 0x{:X}", offsets::weaponC4);

#if 0
    // Local Player Pawn (tbh idk how to read it :1)
    if (temp = Scan(offsets::signatures::localPlayerPawn, client); !temp) {
        Missing("Could not find offset for 'localPlayerPawn'");
        return false;
    }

    offsets::localPlayerPawn = temp + 0x138 - client.base;
    LOGF(VERBOSE, "Found 'localPlayerPawn' offset at 0x{:X}", offsets::localPlayerPawn);
 

    // Input
    if (temp = Scan(offsets::signatures::csgoInput, client); !temp) {
        Missing("Could not find offset for 'csgoInput'");
        return false;
    }

    offsets::csgoInput = temp - client.base;
    LOGF(VERBOSE, "Found 'csgoInput' offset at 0x{:X}", offsets::csgoInput);
#endif

    // CSGOInput & its third person fields, optional
    if (!ResolveThirdPerson(client))
        Missing("Could not find the third person code, third person is disabled");

    // The view of each frame, optional
    if (!ResolveCamera(client))
        Missing("Could not find the camera code, free cam & spectating are disabled");

    // Accepting a match from memory, optional: without it the button is clicked
    {
        auto ready = ScanMemory(offsets::signatures::setLocalPlayerReady, client.base, client.base + client.size);
        if (!ready.empty()) {
            auto at = ready.at(0);
            offsets::lobby::matchmaking = at + 0x39 + process->read<int32_t>(at + 0x34) - client.base;
            offsets::lobby::accept = at + 0x70 + process->read<int32_t>(at + 0x6C) - client.base;
            LOGF(VERBOSE, "Found the match accept at 0x{:X} (matchmaking 0x{:X})", offsets::lobby::accept, offsets::lobby::matchmaking);
        }
        else {
            Missing("Could not find the match accept code, auto accept clicks the button");
        }
    }

    // Skin regeneration, optional
    if (!ResolveSkins(client))
        Missing("Could not find the skin regeneration code, skin changer is disabled");

    // Music of the main menu, optional
    if (!ResolveMenuMusic(client))
        Missing("Could not find the music of the main menu, it keeps the kit of the inventory");

    // Clan tag, optional
    if (auto found = ScanMemory(offsets::signatures::updateClanTag, client.base, client.base + client.size); !found.empty()) {
        offsets::controller::fnUpdateClanTag = found.at(0) - client.base;
        LOGF(VERBOSE, "Found the clan tag update at 0x{:X}", offsets::controller::fnUpdateClanTag);
    } else
        Missing("Could not find the clan tag update, the clan tag is disabled");

    // User commands, optional: only read for the subtick strafe
    {
        auto managers = ScanMemory(offsets::signatures::userCmdManagers, client.base, client.base + client.size);
        auto sequence = ScanMemory(offsets::signatures::userCmdSequence, client.base, client.base + client.size);

        if (!managers.empty() && !sequence.empty()) {
            constexpr size_t lea = 36; // mov r14, [rip + managers]
            offsets::usercmd::dwManagers = managers.at(0) + lea + 7 + Engine::GetProcess()->read<int32_t>(managers.at(0) + lea + 3) - client.base;
            offsets::usercmd::m_nSequence = Engine::GetProcess()->read<int32_t>(sequence.at(0) + 3);
            LOGF(VERBOSE, "Found the user commands at 0x{:X}, sequence 0x{:X}", offsets::usercmd::dwManagers, offsets::usercmd::m_nSequence);
        } else
            Missing("Could not find the user commands");
    }

    // Game event manager, optional: only to hear who voted what
    if (auto found = ScanMemory(offsets::signatures::gameEventManager, client.base, client.base + client.size); !found.empty()) {
        auto at = found.at(0) + 12; // mov rcx, [rip + manager]
        offsets::votes::dwGameEventManager = at + 7 + Engine::GetProcess()->read<int32_t>(at + 3) - client.base;
        LOGF(VERBOSE, "Found the game event manager at 0x{:X}", offsets::votes::dwGameEventManager);
    } else
        Missing("Could not find the game event manager, the vote list shows no names");

    // The "playvol" command of the sound system, called with arguments of ours: the hit & kill sounds through the game
    {
        auto sound = Engine::GetProcess()->GetModule("soundsystem.dll");
        auto found = sound.base ? ScanMemory(offsets::signatures::playVol, sound.base, sound.base + sound.size) : std::vector<DWORD64>{};
        if (!found.empty()) {
            offsets::sounds::fnPlayVol = found.at(0) - sound.base;
            LOGF(VERBOSE, "Found 'playvol' of the sound system at 0x{:X}", offsets::sounds::fnPlayVol);
        } else
            Missing("Could not find 'playvol' of the sound system, hit & kill sounds play through Windows");
    }

    // Name, optional
    if (auto found = ScanMemory(offsets::signatures::updateName, client.base, client.base + client.size); !found.empty()) {
        offsets::controller::fnUpdateName = found.at(0) - client.base;
        LOGF(VERBOSE, "Found the name update at 0x{:X}", offsets::controller::fnUpdateName);
    } else
        Missing("Could not find the name update, the name change is disabled");

    // Render color, optional: only for chams
    if (auto found = ScanMemory(offsets::signatures::setRenderColor, client.base, client.base + client.size); !found.empty()) {
        offsets::visuals::fnSetRenderColor = found.at(0) - client.base;
        LOGF(VERBOSE, "Found 'SetRenderColor' at 0x{:X}", offsets::visuals::fnSetRenderColor);
    } else
        Missing("Could not find 'SetRenderColor', chams are disabled");

    // Glow chams, optional
    {
        auto process = Engine::GetProcess();
        auto end = client.base + client.size;
        auto attributes = ScanMemory(offsets::signatures::chamsAttributes, client.base, end);
        auto node = ScanMemory(offsets::signatures::chamsSceneNode, client.base, end);
        auto list = ScanMemory(offsets::signatures::chamsSceneList, client.base, end);
        auto object = ScanMemory(offsets::signatures::chamsSceneObject, client.base, end);
        auto updater = ScanMemory(offsets::signatures::chamsUpdater, client.base, end);

        if (!attributes.empty() && !node.empty() && !list.empty() && !object.empty() && !updater.empty()) {
            namespace vis = offsets::visuals;
            auto at = attributes.at(0);
            constexpr size_t CALL = 55; // The call at the end of the pattern

            vis::sceneObjectAttributes = process->read<int32_t>(at + 3);
            vis::dwSceneSystem = at + 17 + process->read<int32_t>(at + 13) - client.base;
            vis::sceneSystemAllocateAttributes = process->read<int32_t>(at + 25);
            vis::fnSetAttributeFloat4 = at + CALL + 5 + process->read<int32_t>(at + CALL + 1) - client.base;
            vis::m_pSceneNode = process->read<int32_t>(node.at(0) + 13);
            vis::sceneNodeCount = process->read<uint8_t>(list.at(0) + 2);
            vis::sceneNodeList = process->read<uint8_t>(list.at(0) + 11);
            vis::sceneHandleObject = process->read<uint8_t>(object.at(0) + 11);
            vis::m_pSceneObjectUpdater = process->read<int32_t>(updater.at(0) + 22);

            LOGF(VERBOSE, "Found the glow chams: updater 0x{:X}, node 0x{:X} ({:X}, {:X}), object 0x{:X}, attributes 0x{:X}, set 0x{:X}",
                vis::m_pSceneObjectUpdater, vis::m_pSceneNode, vis::sceneNodeCount, vis::sceneNodeList, vis::sceneHandleObject,
                vis::sceneObjectAttributes, vis::fnSetAttributeFloat4);
        } else
            Missing("Could not find the code for the glow chams, only textured chams");
    }

    // Smoke clouds, optional: only for no smoke
    {
        auto process = Engine::GetProcess();
        auto volume = ScanMemory(offsets::signatures::smokeVolume, client.base, client.base + client.size);
        auto start = ScanMemory(offsets::signatures::smokeStart, client.base, client.base + client.size);

        if (!volume.empty() && !start.empty()) {
            // The start time of the cloud itself is written a bit later: movss [rsi + start], xmm6
            uint8_t code[0x100]{};
            process->read_raw(start.at(0), code, sizeof(code));

            for (size_t i = 20; i + 5 <= sizeof(code); i++) {
                if (code[i] == 0xF3 && code[i + 1] == 0x0F && code[i + 2] == 0x11 && code[i + 3] == 0x76) {
                    offsets::visuals::smokeVolumeStart = code[i + 4];
                    break;
                }
            }

            offsets::visuals::smokeVolume = process->read<int32_t>(volume.at(0) + 12);
            offsets::visuals::smokeRenderObject = *reinterpret_cast<int32_t*>(&code[3]);
            offsets::visuals::smokeRenderStart = *reinterpret_cast<int32_t*>(&code[16]);
        }

        if (offsets::visuals::smokeVolume && offsets::visuals::smokeVolumeStart)
            LOGF(VERBOSE, "Found the smoke cloud at 0x{:X}, start 0x{:X}, render object 0x{:X}, its start 0x{:X}",
                offsets::visuals::smokeVolume, offsets::visuals::smokeVolumeStart,
                offsets::visuals::smokeRenderObject, offsets::visuals::smokeRenderStart);
        else {
            offsets::visuals::smokeVolume = 0;
            Missing("Could not find the smoke cloud, no smoke is disabled");
        }
    }

    // server.dll, optional: only the MVP anthem needs it. Loaded with the first map, so tried again later
    ResolveServer();

    // engine2.dll

    // Build Number
    if (!(temp = Scan(offsets::signatures::buildNumber, engine))) {
        Missing("Could not find offset for 'buildNumber'");
        return false;
    }

    offsets::buildNumber = temp - engine.base;
    LOGF(VERBOSE, "Found 'buildNumber' offset at 0x{:X}", offsets::buildNumber);

    // Network client, its delta tick is written to ask for a full update: from the game too
    if ((temp = Scan(offsets::signatures::networkGameClient, engine))) {
        offsets::network::dwNetworkGameClient = temp - engine.base;
        LOGF(VERBOSE, "Found 'dwNetworkGameClient' offset at 0x{:X}", offsets::network::dwNetworkGameClient);
    } else {
        Missing("Could not find 'dwNetworkGameClient', using 0x{:X}", offsets::network::dwNetworkGameClient);
    }

    if (auto found = ScanMemory(offsets::signatures::networkDeltaTick, engine.base, engine.base + engine.size); !found.empty()) {
        offsets::network::deltaTick = process->read<int32_t>(found.at(0) + 3);
        LOGF(VERBOSE, "Found the network delta tick at 0x{:X}", offsets::network::deltaTick);
    } else {
        Missing("Could not find the network delta tick, using 0x{:X}", offsets::network::deltaTick);
    }

    LOGF(INFO, "Successfully dumped offsets...");

    return true;
}

const std::vector<std::string>& Dumper::Unverified() {
    return GetInstance().unverified;
}

bool Dumper::ResolveSchema(ProcessModule client) {
    auto process = Engine::GetProcess();
    if (!process || !client.base || !client.size)
        return false;

    // The class infos are data of client.dll itself: one read of the whole image, then everything is looked up in it
    std::vector<uint8_t> image(client.size);
    constexpr size_t CHUNK = 1 << 20;
    for (size_t at = 0; at < client.size; at += CHUNK)
        process->read_raw(client.base + at, image.data() + at, std::min(CHUNK, client.size - at));

    auto u8 = [&](size_t rva) -> uint8_t { return rva < image.size() ? image[rva] : 0; };
    auto u16 = [&](size_t rva) -> uint16_t { return rva + 2 <= image.size() ? *reinterpret_cast<uint16_t*>(&image[rva]) : 0; };
    auto i32 = [&](size_t rva) -> int32_t { return rva + 4 <= image.size() ? *reinterpret_cast<int32_t*>(&image[rva]) : 0; };
    auto u64 = [&](size_t rva) -> uint64_t { return rva + 8 <= image.size() ? *reinterpret_cast<uint64_t*>(&image[rva]) : 0; };

    // Pointers in the image are addresses in the game, into the image when they point into client.dll
    auto to_rva = [&](uint64_t address) -> std::optional<size_t> {
        if (address < client.base || address >= client.base + client.size)
            return std::nullopt;
        return static_cast<size_t>(address - client.base);
    };

    auto text = [&](size_t rva) {
        size_t end = rva;
        while (end < image.size() && end - rva < 128 && image[end])
            end++;
        return std::string_view(reinterpret_cast<const char*>(image.data() + rva), end - rva);
    };

    // SchemaClassInfoData_t: name +0x08, size +0x20, field count +0x24, base count +0x29, fields +0x30, bases +0x38
    constexpr size_t CLASS_NAME = 0x08, CLASS_SIZE = 0x20, CLASS_FIELD_COUNT = 0x24, CLASS_BASE_COUNT = 0x29;
    constexpr size_t CLASS_FIELDS = 0x30, CLASS_BASES = 0x38;
    // SchemaClassFieldData_t, 0x20 each: name +0x00, offset +0x10. A base class: offset +0x00, class +0x08, 0x10 each
    constexpr size_t FIELD_SIZE = 0x20, FIELD_OFFSET = 0x10, BASE_SIZE = 0x10, BASE_CLASS = 0x08;

    std::set<std::string_view> wanted;
    for (const auto& f : SchemaFields())
        wanted.insert(f.klass);

    // Every 8 byte value pointing into the image may be the name of a class info, the rest of it has to fit
    std::unordered_map<std::string_view, size_t> classes;
    for (size_t at = CLASS_NAME; at + CLASS_BASES + 8 <= image.size(); at += 8) {
        auto name = to_rva(u64(at));
        if (!name)
            continue;

        size_t info = at - CLASS_NAME;
        auto fields = to_rva(u64(info + CLASS_FIELDS));
        auto count = u16(info + CLASS_FIELD_COUNT);
        auto size = i32(info + CLASS_SIZE);
        if (!fields || *fields % 8 || count == 0 || count > 4096 || size <= 0 || size > 0x100000)
            continue;

        auto class_name = text(*name);
        if (wanted.contains(class_name))
            classes.try_emplace(class_name, info);
    }

    if (classes.empty()) {
        Missing("Could not read the schema of the game, the class layout might have changed: using cs2-dumper offsets");
        return false;
    }

    // The field in the class or one it derives from, with the offset of that base added
    std::function<std::optional<std::ptrdiff_t>(size_t, std::string_view, int)> find = [&](size_t info, std::string_view field, int depth) -> std::optional<std::ptrdiff_t> {
        if (depth > 16)
            return std::nullopt;

        auto fields = to_rva(u64(info + CLASS_FIELDS));
        for (uint16_t i = 0, count = u16(info + CLASS_FIELD_COUNT); fields && i < count; i++) {
            auto entry = *fields + i * FIELD_SIZE;
            auto name = to_rva(u64(entry));
            if (name && text(*name) == field)
                return i32(entry + FIELD_OFFSET);
        }

        auto bases = to_rva(u64(info + CLASS_BASES));
        for (uint8_t i = 0, count = u8(info + CLASS_BASE_COUNT); bases && i < count; i++) {
            auto entry = *bases + i * BASE_SIZE;
            if (auto base = to_rva(u64(entry + BASE_CLASS))) {
                if (auto value = find(*base, field, depth + 1))
                    return static_cast<std::ptrdiff_t>(static_cast<uint32_t>(i32(entry))) + *value;
            }
        }

        return std::nullopt;
    };

    int found = 0, differ = 0;
    std::string missing;
    for (const auto& f : SchemaFields()) {
        auto info = classes.find(f.klass);
        auto value = info != classes.end() ? find(info->second, f.field, 0) : std::nullopt;

        if (!value) {
            missing += std::format("{}{}::{}", missing.empty() ? "" : ", ", f.klass, f.field);
            continue;
        }

        found++;
        if (*value != f.value) {
            LOGF(VERBOSE, "Schema '{}::{}' at 0x{:X} (cs2-dumper 0x{:X})", f.klass, f.field, *value, f.value);
            f.value = *value;
            differ++;
        }
    }

    if (!missing.empty())
        Missing("Not in the schema of the game, kept from cs2-dumper: {}", missing);

    LOGF(INFO, "Read {} of {} field offsets from the game itself ({} classes), {} differ from cs2-dumper", found, SchemaFields().size(), classes.size(), differ);
    return missing.empty();
}

bool Dumper::ResolveButtons(ProcessModule client) {
    auto process = Engine::GetProcess();

    const std::pair<std::ptrdiff_t&, const char*> buttons[] = {
        { offsets::buttons::jump, "jump" },
        { offsets::buttons::forward, "forward" },
        { offsets::buttons::back, "back" },
        { offsets::buttons::left, "left" },
        { offsets::buttons::right, "right" },
    };

    // Each registration names its button: the lea rdx points at the name
    constexpr size_t STATE = 0x28; // Pressed state, from the start of the button
    size_t found = 0;
    for (auto at : ScanMemory(offsets::signatures::buttonRegister, client.base, client.base + client.size, 1024)) {
        auto name_address = at + 10 + process->read<int32_t>(at + 6);
        auto button = at + 17 + process->read<int32_t>(at + 13);

        char name[16]{};
        process->read_raw(name_address, name, sizeof(name) - 1);

        for (auto& [offset, wanted] : buttons) {
            if (std::string_view(name) != wanted)
                continue;

            auto value = static_cast<std::ptrdiff_t>(button + STATE - client.base);
            if (value != offset)
                LOGF(VERBOSE, "Button '{}' at 0x{:X} (was 0x{:X})", wanted, value, offset);
            offset = value;
            found++;
        }
    }

    return found == std::size(buttons);
}

bool Dumper::ResolveMenuMusic(ProcessModule client) {
    auto process = Engine::GetProcess();

    auto object = ScanMemory(offsets::signatures::menuMusic, client.base, client.base + client.size);
    auto fields = ScanMemory(offsets::signatures::menuMusicFields, client.base, client.base + client.size);
    if (object.empty() || fields.empty())
        return false;

    // call <getter>, the getter is "lea rax, [rip + object]; ret"
    auto getter = object.at(0) + 9 + process->read<int32_t>(object.at(0) + 5);

    uint8_t lea[8]{};
    process->read_raw(getter, lea, sizeof(lea));
    if (lea[0] != 0x48 || lea[1] != 0x8D || lea[2] != 0x05 || lea[7] != 0xC3)
        return false;

    offsets::econ::dwMenuMusic = getter + 7 + *reinterpret_cast<int32_t*>(&lea[3]) - client.base;
    offsets::econ::m_pszMenuMusicOverride = process->read<int32_t>(fields.at(0) + 3);
    offsets::econ::m_pszMenuMusicType = process->read<int32_t>(fields.at(0) + 10);

    LOGF(VERBOSE, "Found the menu music at 0x{:X}, override 0x{:X}, type 0x{:X}",
        offsets::econ::dwMenuMusic, offsets::econ::m_pszMenuMusicOverride, offsets::econ::m_pszMenuMusicType);
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

    if (!checks.empty() && RestoreJump(checks.at(0) + jump_at, 0x75)) {
        offsets::input::cheatsCheckJump = checks.at(0) + jump_at - client.base;
        LOGF(VERBOSE, "Found the third person sv_cheats check at 0x{:X}", offsets::input::cheatsCheckJump);
    }
    else {
        Missing("Could not find the third person sv_cheats check, third person needs sv_cheats 1");
    }
    return true;
}

bool Dumper::RestoreJump(uintptr_t address, uint8_t opcode) {
    auto process = Engine::GetProcess();
    auto current = process->read<uint8_t>(address);

    if (current == opcode)
        return true;

    // Still our jmp from an earlier run that was closed without putting it back
    constexpr uint8_t JMP_SHORT = 0xEB;
    if (current != JMP_SHORT)
        return false;

    if (!process->patch_code(address, &opcode, sizeof(opcode)))
        return false;

    LOGF(INFO, "Put back the jump at 0x{:X} an earlier run left patched", address);
    return true;
}

bool Dumper::ResolveCamera(ProcessModule client) {
    auto process = Engine::GetProcess();

    auto views = ScanMemory(offsets::signatures::overrideView, client.base, client.base + client.size);
    auto modes = ScanMemory(offsets::signatures::clientModes, client.base, client.base + client.size);
    if (views.empty() || modes.empty())
        return false;

    // The fields of CViewSetup, from the debug view of the function: movsd [rdi + disp32], xmm0
    constexpr size_t ORIGIN_AT = 0x98, ANGLES_AT = 0xC1;
    constexpr uint8_t MOVSD_STORE[] = { 0xF2, 0x0F, 0x11, 0x87 };
    // & the field of view at its end: movss [rdi + disp32], xmm0. Optional, the built-in value else
    constexpr size_t FOV_AT = 0x26F;
    constexpr uint8_t MOVSS_STORE[] = { 0xF3, 0x0F, 0x11, 0x87 };

    uint8_t code[FOV_AT + 8]{};
    if (!process->read_raw(views.at(0), code, sizeof(code)))
        return false;

    if (!std::equal(std::begin(MOVSD_STORE), std::end(MOVSD_STORE), code + ORIGIN_AT) ||
        !std::equal(std::begin(MOVSD_STORE), std::end(MOVSD_STORE), code + ANGLES_AT))
        return false;

    offsets::camera::m_vecOrigin = *reinterpret_cast<int32_t*>(code + ORIGIN_AT + sizeof(MOVSD_STORE));
    offsets::camera::m_angView = *reinterpret_cast<int32_t*>(code + ANGLES_AT + sizeof(MOVSD_STORE));
    if (std::equal(std::begin(MOVSS_STORE), std::end(MOVSS_STORE), code + FOV_AT))
        offsets::camera::m_flFov = *reinterpret_cast<int32_t*>(code + FOV_AT + sizeof(MOVSS_STORE));
    offsets::camera::overrideView = views.at(0) - client.base;

    // lea rcx, [rip + modes] at +3, 7 long
    offsets::camera::dwClientMode = modes.at(0) + 3 + 7 + process->read<int32_t>(modes.at(0) + 6) - client.base;

    LOGF(VERBOSE, "Found the camera: client mode 0x{:X}, OverrideView 0x{:X}, origin 0x{:X}, angles 0x{:X}",
        offsets::camera::dwClientMode, offsets::camera::overrideView, offsets::camera::m_vecOrigin, offsets::camera::m_angView);

    // GetLocalPawn: the CS OverrideView calls the base one at +0xA, which calls it at +0x2C after xor ecx, ecx
    auto call_target = [&](uintptr_t at) { return at + 5 + process->read<int32_t>(at + 1); };
    if (code[0xA] == 0xE8) {
        auto base = call_target(views.at(0) + 0xA);
        uint8_t call[3]{};
        if (process->read_raw(base + 0x2A, call, sizeof(call)) && call[0] == 0x33 && call[1] == 0xC9 && call[2] == 0xE8) {
            offsets::camera::getLocalPawn = call_target(base + 0x2C) - client.base;
            LOGF(VERBOSE, "Found GetLocalPawn at 0x{:X}", offsets::camera::getLocalPawn);
        }
    }

    // The spectator keys, optional. Its start: mov [rsp + 0x10], rbx; push rbp, or the patch an earlier run left
    auto binds = ScanMemory(offsets::signatures::spectatorBinds, client.base, client.base + client.size);
    if (!binds.empty()) {
        constexpr uint8_t ORIGINAL[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x55 };
        constexpr uint8_t PATCHED[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
        auto start = binds.at(0) - sizeof(ORIGINAL);

        uint8_t bytes[sizeof(ORIGINAL)]{};
        process->read_raw(start, bytes, sizeof(bytes));
        // Either all of them taken, or jmp rel32 + nop to the filter in our page
        bool left = std::equal(std::begin(PATCHED), std::end(PATCHED), bytes) || (bytes[0] == 0xE9 && bytes[5] == 0x90);
        if (left && process->patch_code(start, ORIGINAL, sizeof(ORIGINAL))) {
            LOGF(INFO, "Put back the spectator keys an earlier run left patched");
            std::copy(std::begin(ORIGINAL), std::end(ORIGINAL), bytes);
        }

        if (std::equal(std::begin(ORIGINAL), std::end(ORIGINAL), bytes)) {
            offsets::camera::spectatorBinds = start - client.base;
            LOGF(VERBOSE, "Found the spectator keys at 0x{:X}", offsets::camera::spectatorBinds);
        }
    }

    // The spectator camera, optional: without it the camera stays the one of the game after death
    constexpr size_t OBSERVER_JUMP_AT = 26;
    auto observer = ScanMemory(offsets::signatures::observerView, client.base, client.base + client.size);
    if (!observer.empty() && RestoreJump(observer.at(0) + OBSERVER_JUMP_AT, 0x74)) {
        offsets::camera::observerViewJump = observer.at(0) + OBSERVER_JUMP_AT - client.base;

        // lea r8, [rbx + angles]; mov rcx, rsi; lea rdx, [rbx + origin]; call
        constexpr size_t OBSERVER_CALL_AT = 45;
        if (process->read<uint8_t>(observer.at(0) + OBSERVER_CALL_AT) == 0xE8)
            offsets::camera::observerViewCall = observer.at(0) + OBSERVER_CALL_AT - client.base;
        LOGF(VERBOSE, "Found the spectator camera at 0x{:X}", offsets::camera::observerViewJump);
    }
    else {
        Missing("Could not find the spectator camera, free cam & spectating are only available while alive");
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
        Missing("Could not find the first person skin update, skins show after switching weapons");
    }

    // Mesh switch for skins made for the old models, optional
    auto mesh = ScanMemory(offsets::signatures::setMeshGroupMask, client.base, client.base + 0x4000000);
    if (mesh.size() == 1) {
        offsets::skins::setMeshGroupMask = mesh.at(0) - client.base;
        LOGF(VERBOSE, "Found 'SetMeshGroupMask' at 0x{:X}", offsets::skins::setMeshGroupMask);
    }
    else {
        Missing("Could not find 'SetMeshGroupMask', skins for the old models might look wrong");
    }

    // Agents, optional
    auto set_model = ScanMemory(offsets::signatures::setModel, client.base, client.base + 0x4000000);
    if (set_model.size() == 1) {
        offsets::skins::setModel = set_model.at(0) - client.base;
        LOGF(VERBOSE, "Found 'SetModel' at 0x{:X}", offsets::skins::setModel);
    }
    else {
        Missing("Could not find 'SetModel', agents are disabled");
    }

    // Knives, optional
    auto subclass_changed = ScanMemory(offsets::signatures::subclassChanged, client.base, client.base + 0x4000000);
    if (subclass_changed.size() == 1) {
        offsets::skins::subclassChanged = subclass_changed.at(0) - client.base;
        LOGF(VERBOSE, "Found 'OnSubclassIDChanged' at 0x{:X}", offsets::skins::subclassChanged);
    }
    else {
        Missing("Could not find 'OnSubclassIDChanged', knives are disabled");
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
        Missing("Could not find the glove model loading, custom gloves might not show");

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

        // A wildcard matches the next byte too: never skip past the last one
        int Num = (std::max)(next[buffer[i + length]], next[256]);
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