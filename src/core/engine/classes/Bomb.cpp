#include "Bomb.hpp"

#include "core/engine/Engine.hpp"
#include "core/offsets/Dumper.hpp"

bool Bomb::Update() {

	auto p = Engine::GetProcess();

	if (!p)
		return false;

	auto client = Engine::GetClient();

	// Resolve the C4 carrier via the global weaponC4 pointer
	// client.base + weaponC4 -> ptr to C4 entity -> m_hOwnerEntity (0x520) = pawn handle
	auto c4_ptr = p->read<uintptr_t>(client.base + offsets::weaponC4);
	if (c4_ptr) {
		if (auto e = p->read<uintptr_t>(c4_ptr))
			carrier = (uintptr_t)p->read<int>(e + 0x520 /* Magic Offset? */);
	}

	this->address = p->read<uintptr_t>(client.base + offsets::plantedC4);
	this->is_planted = (this->address != 0);

	if (!this->is_planted) {
		Bomb::prev_is_planted = false;
		this->timed = false;
		return true;
	}

	auto site = p->read<uint32_t>(this->address + offsets::bomb::m_nBombSite);
	this->site = (site == 1) ? BombSite::B : BombSite::A;

	// Done with, nothing left to show
	if (p->read<bool>(this->address + offsets::bomb::m_bBombDefused) || p->read<bool>(this->address + offsets::bomb::m_bHasExploded)) {
		this->is_planted = false;
		this->timed = false;
		return true;
	}

	this->timer_length = p->read<float>(this->address + offsets::bomb::m_flTimerLength);
	if (this->timer_length <= 0.f || this->timer_length > 120.f)
		this->timer_length = 40.f;

	this->c4_blow = p->read<float>(this->address + offsets::bomb::m_flC4Blow);
	this->defusing = p->read<bool>(this->address + offsets::bomb::m_bBeingDefused);
	this->defuse_length = p->read<float>(this->address + offsets::bomb::m_flDefuseLength);
	this->defuse_kit = this->defuse_length > 0.f && this->defuse_length < 7.5f;
	this->defuse_countdown = p->read<float>(this->address + offsets::bomb::m_flDefuseCountDown);

	if (this->defuse_length <= 0.f)
		this->defuse_length = 10.f;

	auto node = p->read<uintptr_t>(this->address + offsets::pawn::m_pGameSceneNode);

	if (node) {
		this->pos = p->read<Vec3_t>(node + offsets::bomb::m_vecAbsOrigin);
	}

	if (!Bomb::prev_is_planted) 
		plant_time = std::time(nullptr);

	// Until the game time is known, from the plant time
	if (!this->timed) {
		this->time_left = 41 - (std::time(nullptr) - plant_time);
		this->defuse_left = this->c4_blow > 0.f
			? std::max(0.f, this->defuse_countdown - (this->c4_blow - this->time_left))
			: this->defuse_length;
	}
	else
		Tick();

	Bomb::prev_is_planted = true;
	return true;
}

std::time_t Bomb::plant_time{};
bool Bomb::prev_is_planted = false;

void Bomb::SetGameTime(float now) {
	using namespace std::chrono;

	if (!this->is_planted || now <= 0.f || this->c4_blow <= 0.f) {
		this->timed = false;
		return;
	}

	auto clock = steady_clock::now();
	auto blow = clock + duration_cast<steady_clock::duration>(duration<float>(this->c4_blow - now));

	// Kept while it agrees, the game time steps by ticks & would make the timer shake
	constexpr auto DRIFT = milliseconds(150);
	auto apart = [](steady_clock::time_point a, steady_clock::time_point b) { return a > b ? a - b : b - a; };

	if (!this->timed || apart(blow, this->blow_at) > DRIFT)
		this->blow_at = blow;

	if (this->defusing) {
		auto defuse = clock + duration_cast<steady_clock::duration>(duration<float>(this->defuse_countdown - now));

		// A new defuse, or one that drifted
		if (!this->timed || this->defuse_countdown != this->last_countdown || apart(defuse, this->defuse_at) > DRIFT)
			this->defuse_at = defuse;
	}

	this->last_countdown = this->defuse_countdown;
	this->timed = true;
	Tick();
}

void Bomb::Tick() {
	using namespace std::chrono;

	if (!this->timed)
		return;

	auto now = steady_clock::now();
	this->time_left = std::max(0.f, duration<float>(this->blow_at - now).count());

	if (this->defusing)
		this->defuse_left = std::max(0.f, duration<float>(this->defuse_at - now).count());
}
