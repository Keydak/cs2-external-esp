#include "Game.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"

bool Game::Update() {
	if (!Engine::GetProcess())
		return false;	
	
	if (!UpdateMatrix()) {
		LOGF(FATAL, "Failed to update view matrix");
		return false;
	}

	// No need to be updated along with the view matrix
	//if (!UpdateEntityList()) {
	//	LOGF(FATAL, "Failed to update entity list");
	//	return false;
	//}

	return true;
}

bool Game::UpdateMatrix() {
	auto p = Engine::GetProcess();
	auto client = Engine::GetClient();

	this->view_matrix = p->read<view_matrix_t>(client.base + offsets::viewMatrix);

	return true;
}

bool Game::UpdateEntityList() {
	auto p = Engine::GetProcess();
	auto client = Engine::GetClient();

	this->entity_list = p->read<DWORD64>(client.base + offsets::entityList);
	this->list_entry = p->read<DWORD64>(this->entity_list + 0x10);

	return true;
}

// The game mode is the class of the mode rules object, found through the RTTI of its vtable
bool Game::UpdateMode() {
	auto p = Engine::GetProcess();
	auto client = Engine::GetClient();

	auto rules = p->read<uintptr_t>(client.base + offsets::rules::dwGameRules);
	auto mode = rules ? p->read<uintptr_t>(rules + offsets::rules::m_pGameModeRules) : 0;

	if (mode == this->mode_rules)
		return true;

	this->mode_rules = 0;
	this->deathmatch = false;

	if (!mode)
		return true;

	// CompleteObjectLocator right before the vtable, its RVAs are relative to the module it is in
	auto vtable = p->read<uintptr_t>(mode);
	auto locator = vtable ? p->read<uintptr_t>(vtable - 8) : 0;
	if (!locator)
		return false;

	auto module = locator - p->read<uint32_t>(locator + 0x14);
	auto type = module + p->read<uint32_t>(locator + 0xC);

	char name[64]{};
	if (!p->read_raw(type + 0x10, name, sizeof(name) - 1) || strncmp(name, ".?AV", 4) != 0)
		return false;

	this->mode_rules = mode;
	this->deathmatch = strstr(name, "Deathmatch") != nullptr;
	LOGF(INFO, "Game mode rules: {}{}", name, this->deathmatch ? ", everyone is an enemy" : "");

	return true;
}
