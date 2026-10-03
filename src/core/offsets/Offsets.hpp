namespace offsets
{
	// client.dll
	inline DWORD entityList;
	inline DWORD viewMatrix;
	inline DWORD localPlayerController;
	inline DWORD globalVars;
	inline DWORD plantedC4;
	inline DWORD weaponC4;

	// engine2.dll
	inline DWORD buildNumber;

	// engine2.dll network client, fallbacks overwritten by Dumper::FetchRemote()
	namespace network {
		inline std::ptrdiff_t dwNetworkGameClient = 0x91AFC0;
		inline std::ptrdiff_t deltaTick = 0x24C; // int32, -1 requests a full update from the server
	}

	// Game rules, fallbacks overwritten by Dumper::FetchRemote()
	namespace rules {
		inline std::ptrdiff_t dwGameRules = 0x255D650; // C_CSGameRules*
		inline std::ptrdiff_t m_pGameModeRules = 0xD98; // CCSGameModeRules* - C_CSGameRules, its class tells the game mode
	}

	// client.dll button states, fallbacks overwritten by Dumper::FetchRemote()
	namespace buttons {
		inline std::ptrdiff_t jump = 0x22304E0;
		inline std::ptrdiff_t forward = 0x2230210;
		inline std::ptrdiff_t back = 0x22302A0;
		inline std::ptrdiff_t left = 0x2230330;
		inline std::ptrdiff_t right = 0x22303C0;
	}

	// Schema field offsets, values below are fallbacks
	// Overwritten at startup by Dumper::FetchRemote() with the latest cs2-dumper output
	namespace controller {
		inline std::ptrdiff_t m_iPing = 0x838; // uint32
		inline std::ptrdiff_t m_hPawn = 0x6BC; // CHandle<C_BasePlayerPawn>
		inline std::ptrdiff_t m_steamID = 0x788; // uint64
		inline std::ptrdiff_t m_iszPlayerName = 0x6FC; // char[128]
		inline std::ptrdiff_t m_bIsLocalPlayerController = 0x790; // bool
		inline std::ptrdiff_t m_pInGameMoneyServices = 0x818; // CCSPlayerController_InGameMoneyServices*
		inline std::ptrdiff_t m_iAccount = 0x40; // int32 - CCSPlayerController_InGameMoneyServices 
	}

	namespace pawn {
		inline std::ptrdiff_t m_vOldOrigin = 0x14A4; // Vector
		inline std::ptrdiff_t m_vecViewOffset = 0xF60; // Vector - C_BaseModelEntity, eyes above the origin
		inline std::ptrdiff_t m_iHealth = 0x34C; // int32
		inline std::ptrdiff_t m_fFlags = 0x3F4; // uint32
		inline std::ptrdiff_t m_iTeamNum = 0x3E7; // uint8
		inline std::ptrdiff_t m_bIsScoped = 0x1EA0; // bool
		inline std::ptrdiff_t m_ArmorValue = 0x1ECC; // int32
		inline std::ptrdiff_t m_bIsDefusing = 0x1EA2; // bool
		inline std::ptrdiff_t m_pItemServices = 0x12F8; // CPlayer_ItemServices* - C_BasePlayerPawn
		inline std::ptrdiff_t m_bHasDefuser = 0x48; // bool - CCSPlayer_ItemServices
		inline std::ptrdiff_t m_vecAbsVelocity = 0x3F8; // Vector
		inline std::ptrdiff_t m_flSimulationTime = 0x3B8; // float32 - C_BaseEntity, game time of the last update

		inline std::ptrdiff_t m_pGameSceneNode = 0x330; // CGameSceneNode*

		inline std::ptrdiff_t m_entitySpottedState = 0x1E88; // EntitySpottedState_t
		inline std::ptrdiff_t m_bSpottedByMask = 0xC; // uint32[2] - EntitySpottedState_t
		inline std::ptrdiff_t m_bSpotted = 0x8; // bool - EntitySpottedState_t, shows the player on the radar of the game

		inline std::ptrdiff_t m_flFlashOverlayAlpha = 0x1504; // float32 - C_CSPlayerPawnBase 

		inline std::ptrdiff_t m_pWeaponServices = 0x12F0; // CPlayer_WeaponServices*
		inline std::ptrdiff_t m_hActiveWeapon = 0x60; // CHandle<C_BasePlayerWeapon> - CPlayer_WeaponServices
		inline std::ptrdiff_t m_AttributeManager = 0x1290; // C_AttributeContainer - C_EconEntity (parent of C_BasePlayerWeapon)
		inline std::ptrdiff_t m_Item = 0x50; // C_EconItemView - C_AttributeContainer
		inline std::ptrdiff_t m_iItemDefinitionIndex = 0x1BA; // uint16 - C_EconItemView
		inline std::ptrdiff_t m_iClip1 = 0x1928; // int32 - C_BasePlayerWeapon
		inline std::ptrdiff_t m_bInReload = 0x1A3C; // bool - C_CSWeaponBase
		inline std::ptrdiff_t m_pObserverServices = 0x1308; // CPlayer_ObserverServices*
	}

	namespace bomb {
		inline std::ptrdiff_t m_isPlanted = 0x8; // unk
		inline std::ptrdiff_t m_bC4Activated = 0x12D0; // bool
		inline std::ptrdiff_t m_nBombSite = 0x128C; // int32
		inline std::ptrdiff_t m_flC4Blow = 0x12B8; // GameTime_t, when it explodes
		inline std::ptrdiff_t m_flTimerLength = 0x12C0; // float32
		inline std::ptrdiff_t m_bBeingDefused = 0x12C4; // bool
		inline std::ptrdiff_t m_flDefuseLength = 0x12D4; // float32, 5 with a kit & 10 without
		inline std::ptrdiff_t m_flDefuseCountDown = 0x12D8; // GameTime_t, when the defuse is done
		inline std::ptrdiff_t m_bBombDefused = 0x12DC; // bool
		inline std::ptrdiff_t m_bHasExploded = 0x12BD; // bool

		inline std::ptrdiff_t m_vecAbsOrigin = 0xC8; // VectorWS - CGameSceneNode
	}

	namespace view {
		inline std::ptrdiff_t m_pCameraServices = 0x1328; // CPlayer_CameraServices* - C_BasePlayerPawn
		inline std::ptrdiff_t m_iFOV = 0x298; // uint32 - CCSPlayerBase_CameraServices
		inline std::ptrdiff_t m_iFOVStart = 0x29C; // uint32 - CCSPlayerBase_CameraServices
	}

	namespace grenade {
		inline std::ptrdiff_t m_designerName = 0x20; // CUtlSymbolLarge - CEntityIdentity
		inline std::ptrdiff_t m_bDidSmokeEffect = 0x1364; // bool - C_SmokeGrenadeProjectile
		inline std::ptrdiff_t m_vSmokeDetonationPos = 0x1378; // Vector - C_SmokeGrenadeProjectile
		inline std::ptrdiff_t m_firePositions = 0x1108; // Vector[64] - C_Inferno
		inline std::ptrdiff_t m_bFireIsBurning = 0x1708; // bool[64] - C_Inferno
		inline std::ptrdiff_t m_fireCount = 0x1A48; // int32 - C_Inferno
		inline std::ptrdiff_t m_nFireLifetime = 0x1A50; // float32 - C_Inferno
		inline std::ptrdiff_t m_maxFireHalfWidth = 0x866C; // float32 - C_Inferno
		inline std::ptrdiff_t m_bExplodeEffectBegan = 0x12FC; // bool - C_BaseCSGrenadeProjectile
		inline std::ptrdiff_t m_nExplodeEffectTickBegin = 0x12D8; // int32 - C_BaseCSGrenadeProjectile

		// Throw prediction
		inline std::ptrdiff_t m_bPinPulled = 0x1F23; // bool - C_BaseCSGrenade
		inline std::ptrdiff_t m_flThrowStrength = 0x1F30; // float32 - C_BaseCSGrenade
		inline std::ptrdiff_t m_angEyeAngles = 0x35F0; // QAngle - C_CSPlayerPawn
		inline std::ptrdiff_t m_vInitialPosition = 0x12B0; // Vector - C_BaseCSGrenadeProjectile
		inline std::ptrdiff_t m_vInitialVelocity = 0x12BC; // Vector - C_BaseCSGrenadeProjectile

		// Solid things in the way of smokes & traces
		inline std::ptrdiff_t m_pCollision = 0x340; // CCollisionProperty* - C_BaseEntity
		inline std::ptrdiff_t m_vecMins = 0x40; // Vector - CCollisionProperty
		inline std::ptrdiff_t m_vecMaxs = 0x4C; // Vector - CCollisionProperty
		inline std::ptrdiff_t m_usSolidFlags = 0x5A; // uint8 - CCollisionProperty
		inline std::ptrdiff_t m_nSolidType = 0x5B; // SolidType_t - CCollisionProperty
		inline std::ptrdiff_t m_angAbsRotation = 0xD4; // QAngle - CGameSceneNode
	}

	namespace econ {
		inline std::ptrdiff_t m_iMusicKitID = 0x968; // int32 - CCSPlayerController

		// m_iMusicKitID of the controller in server.dll (the game hosts the server itself offline), sent with the MVP event.
		// From the code that writes the event, 0 when not found
		inline std::ptrdiff_t server_m_iMusicKitID = 0;
		inline std::ptrdiff_t m_hMyWeapons = 0x48; // C_NetworkUtlVectorBase<CHandle<C_BasePlayerWeapon>> - CPlayer_WeaponServices
		inline std::ptrdiff_t m_AttributeList = 0x208; // CAttributeList - C_EconItemView
		inline std::ptrdiff_t m_Attributes = 0x8; // C_UtlVectorEmbeddedNetworkVar<CEconItemAttribute> - CAttributeList
		inline std::ptrdiff_t m_iItemIDHigh = 0x1D0; // uint32 - C_EconItemView
		inline std::ptrdiff_t m_iAccountID = 0x1D8; // uint32 - C_EconItemView
		inline std::ptrdiff_t m_iEntityQuality = 0x1BC; // int32 - C_EconItemView
		inline std::ptrdiff_t m_bInitialized = 0x1E8; // bool - C_EconItemView
		inline std::ptrdiff_t m_nFallbackPaintKit = 0x18A8; // int32 - C_EconEntity
		inline std::ptrdiff_t m_nFallbackSeed = 0x18AC; // int32 - C_EconEntity
		inline std::ptrdiff_t m_flFallbackWear = 0x18B0; // float32 - C_EconEntity
		inline std::ptrdiff_t m_nFallbackStatTrak = 0x18B4; // int32 - C_EconEntity
		inline std::ptrdiff_t m_OriginalOwnerXuidLow = 0x18A0; // uint32 - C_EconEntity
		inline std::ptrdiff_t m_EconGloves = 0x1770; // C_EconItemView - C_CSPlayerPawn
		inline std::ptrdiff_t m_bNeedToReApplyGloves = 0x176D; // bool - C_CSPlayerPawn
		inline std::ptrdiff_t m_hHudModelArms = 0x1DA8; // CHandle<C_CS2HudModelArms> - C_CSPlayerPawn
		inline std::ptrdiff_t m_MeshGroupMask = 0x208; // uint64 - CModelState
		inline std::ptrdiff_t m_pChild = 0x40; // CGameSceneNode* - CGameSceneNode
		inline std::ptrdiff_t m_pNextSibling = 0x48; // CGameSceneNode* - CGameSceneNode
		inline std::ptrdiff_t m_pOwner = 0x30; // CEntityInstance* - CGameSceneNode
		inline std::ptrdiff_t m_hOwnerEntity = 0x520; // CHandle<C_BaseEntity> - C_BaseEntity
		inline std::ptrdiff_t m_pInventoryServices = 0x820; // CCSPlayerController_InventoryServices* - CCSPlayerController
		inline std::ptrdiff_t m_unMusicID = 0x58; // uint16 - CCSPlayerController_InventoryServices
		inline std::ptrdiff_t m_fEffects = 0x52C; // uint32 - C_BaseEntity
		inline std::ptrdiff_t m_ModelName = 0xA8; // CUtlSymbolLarge - CModelState
		inline std::ptrdiff_t m_nSubclassID = 0x380; // CUtlStringToken - C_BaseEntity, hash of the item definition index for weapons
	}

	// Skin regeneration, found at startup from the game code. 0 when not found
	namespace skins {
		inline std::ptrdiff_t regenerateWeaponSkins = 0; // void(), rebuilds the skin of every weapon that already has one
		inline std::ptrdiff_t clearMaterials = 0; // void(composite owner*, bool), drops the skin materials
		inline std::ptrdiff_t updateWeaponSkin = 0; // void(C_CSWeaponBase*, bool), builds the skin from the item attributes
		inline std::ptrdiff_t updateViewmodelSkin = 0; // void(C_CSWeaponBase*), builds the skin of the first person model. 0 when not found
		inline std::ptrdiff_t viewmodelSkinBuilt = 0x1B20; // bool - C_CSWeaponBase, the first person skin is only built while false
		inline std::ptrdiff_t setMeshGroupMask = 0; // void(CSkeletonInstance*, uint64), switches the model mesh. 0 when not found
		inline std::ptrdiff_t setModel = 0; // void(C_BaseModelEntity*, const char*), for agents. 0 when not found
		inline std::ptrdiff_t subclassChanged = 0; // void(C_BaseEntity*), OnSubclassIDChanged, loads the weapon data & recreates the first person model. 0 when not found
		inline std::ptrdiff_t gloveHelper = 0x15F8; // Glove model helper of C_CSPlayerPawn, the glove entity handle comes first
		inline std::ptrdiff_t gloveRemoveJump = 0; // "je" skipping the removal of gloves the player model says it has itself. 0 when not found
		inline std::ptrdiff_t showDefaultGloves = 0; // void(C_CSPlayerPawn*, bool), gloves that are part of the player model. 0 when not found
		inline std::ptrdiff_t precacheGloves = 0; // bool, lets the glove code load glove models nobody owns (player preview). 0 when not found
		inline std::ptrdiff_t compositeOwner = 0x610; // Owner of the skin materials - C_BaseModelEntity
		inline std::ptrdiff_t compositeMaterials = 0x4A8; // Material pointers, count right before - composite owner
		inline std::ptrdiff_t compositePending = 0x4B8; // int32, materials still being built - composite owner
	}

	namespace bone {
		inline std::ptrdiff_t m_modelState = 0x140; // CModelState
	}

	namespace observerServices {
		inline std::ptrdiff_t m_iObserverMode = 0x48;
		inline std::ptrdiff_t m_hObserverTarget = 0x4C;
	}

	// CCSGOInput, found at startup from the code of the "thirdperson" command
	namespace input {
		inline std::ptrdiff_t dwCSGOInput = 0; // 0 when not found
		inline std::ptrdiff_t m_angViewAngles = 0x688; // QAngle
		inline std::ptrdiff_t m_bInThirdPerson = 0x229; // bool
		inline std::ptrdiff_t m_vecCameraOffset = 0x230; // pitch, yaw, distance, moved by the game every frame

		// "jne" letting third person stay on while sv_cheats is set, the camera code turns it off otherwise. 0 when not found
		inline std::ptrdiff_t cheatsCheckJump = 0;
	}

	namespace global {
		constexpr std::ptrdiff_t maxClients = 0x10;
		constexpr std::ptrdiff_t currentMapName = 0x180;
		constexpr std::ptrdiff_t currentTime = 0x2C;
	}

	namespace signatures
	{
		const std::string viewMatrix = "48 8D 0D ?? ?? ?? ?? 48 C1 E0 06";
		const std::string globalVars = "48 89 15 ?? ?? ?? ?? 48 89 42";
		// server.dll, round_mvp: cmp dword ptr [rsi + m_iMusicKitID], -1 ... "musickitid" (its hash 0xFA4F8E96)
		const std::string serverMusicKit = "83 BE ?? ?? ?? ?? FF 74 ?? 48 8B 07 48 8D 0D ?? ?? ?? ?? BA 06 00 00 00 41 B8 96 8E 4F FA";

		// Callback of the "thirdperson" command: mov r8, [CSGOInput] ... cmp byte ptr [r8 + m_bInThirdPerson], 0
		const std::string thirdPerson =
			"4C 8B 05 ?? ?? ?? ?? 41 8B 80 ?? ?? ?? ?? 85 C0 74 ?? FF C8 48 63 C8 49 8B 80 ?? ?? ?? ?? "
			"48 69 D1 ?? ?? ?? ?? 48 05 ?? ?? ?? ?? 48 03 C2 EB ?? 49 8D 80 ?? ?? ?? ?? 41 80 B8 ?? ?? ?? ?? 00";

		// Camera update: if (in third person && !sv_cheats) back to first person. The "jne" is at +40
		const std::string thirdPersonCheats =
			"44 38 B4 3E ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? 8B 41 ?? "
			"48 0F BA E0 0A 72 ?? 44 38 71 ?? 75 ?? 44 88 B4 3E";
		// RegenerateWeaponSkins: walks the weapons,
		// "lea rcx, [rbx + composite owner]; mov dl, 1; call clear; xor edx, edx; mov rcx, rbx; call update" at +0x72
		const std::string regenerateWeaponSkins = "48 83 EC ?? E8 ?? ?? ?? ?? 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 10";
		// Skin of the first person model: "cmp byte ptr [rcx + built], 0; mov rbx, rcx; jne" at +18
		const std::string updateViewmodelSkin =
			"48 89 5C 24 20 55 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 80 B9 ?? ?? ?? ?? 00 48 8B D9 0F 85 ?? ?? ?? ?? "
			"48 8D 55 ?? 48 89 B4 24 ?? ?? ?? ?? C6 81";
		// CSkeletonInstance::SetMeshGroupMask, does nothing when the mask in memory already matches
		const std::string setMeshGroupMask = "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8D 99 ?? ?? ?? ?? 48 8B 71";
		// Pawn update: "lea rcx, [rbx + glove helper]; call ApplyGloves; mov rcx, rbx; call ...; mov rdi, [rbx + ...]"
		const std::string applyGlovesCall = "48 8D 8B ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CB E8 ?? ?? ?? ?? 48 8B BB";
		// C_BaseModelEntity::SetModel: model handle by name from the resource system, then set by handle
		const std::string setModel = "40 53 48 83 EC ?? 48 8B D9 4C 8B C2 48 8B 0D ?? ?? ?? ?? 48 8D 54 24";
		// OnSubclassIDChanged: "call UpdateSubclass; cmp [rbx + vdata], 0; je; mov rax, [rbx]; mov edx, 1" then the vdata changed vfunc
		const std::string subclassChanged = "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 BB ?? ?? ?? ?? 00 74 ?? 48 8B 03 BA 01 00 00 00";
		const std::string entityList = "48 8B 0D ?? ?? ?? ?? 48 89 7C 24 ?? 8B FA C1 EB";
		const std::string localPlayerController = "48 8B 05 ?? ?? ?? ?? 41 89 BE";
		const std::string plantedC4 = "48 8B 1D ?? ?? ?? ?? 45 32 F6";
		const std::string weaponC4 =
			"48 89 05 ?? ?? ?? ?? "
			"F7 C1 ?? ?? ?? ?? "
			"74 ?? "
			"81 E1 ?? ?? ?? ?? "
			"89 0D ?? ?? ?? ?? "
			"8B 05 ?? ?? ?? ?? "
			"89 1D ?? ?? ?? ?? "
			"EB ?? "
			"48 8B 15 ?? ?? ?? ?? "
			"48 8B 5C 24 ?? "
			"FF C0 "
			"89 05 ?? ?? ?? ?? "
			"48 8B C6 48 89 34 EA 80 BE";

#if 0
		const std::string localPlayerPawn = "48 8D 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 83 EC ?? 8B 0D";

		const std::string csgoInput = "48 89 05 ?? ?? ?? ?? 0F 57 C0 0F 11 05";
		const std::string viewAngles = "F2 42 0F 10 84 28 ?? ?? ?? ??";
#endif

		const std::string buildNumber = "89 05 ?? ?? ?? ?? 48 8d 0d ?? ?? ?? ?? ff 15 ?? ?? ?? ?? 48 8b 0d";

	}
}