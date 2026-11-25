#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJGameLevel.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/loader/SettingEvent.hpp>

#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdint>

#include "input.hpp"

using namespace geode::prelude;

/*
Click Between Frames for GD 2.113 (Geode v1.3.10).

GD 2.1 delivers every input at the start of the frame, so the exact moment a
click happened is lost. This port works like this:

1. A background thread (input.cpp) timestamps jump presses/releases with QPC as
   soon as the OS reports them.
2. GD's own GJBaseGameLayer::pushButton/releaseButton call runs normally, once
   (so Mega Hack's CPS counter and anything else hooked there sees exactly one
   click). Only what makes the player move, PlayerObject::pushButton /
   releaseButton, is held back and stored with the timestamp of the raw event.
3. PlayLayer::update(dt) is split. The part of the frame before the input runs
   with PlayLayer::update(dt * fraction), then the stored PlayerObject call is
   replayed, then the remaining part of the frame runs.

Frames without any input run exactly like vanilla (a single update call).
*/

namespace {
	struct PendingInput {
		double time;
		PlayerObject* player;
		PlayerButton button;
		bool push;
	};

	std::vector<PendingInput> g_pending; // main thread only
	bool g_replaying = false;            // true while we forward an input to the game
	bool g_firstFrame = true;            // true -> do not split (start of level, resume, lag spike)
	double g_lastUpdateTime = 0.0;

	bool g_softToggle = false;           // true -> CBF disabled
	bool g_safeMode = false;

	// frames longer than this are treated as pause/lag: no splitting
	constexpr double MAX_SPLIT_FRAME = 0.25;
	// raw events older than this (relative to the moment GD delivers the input) are stale
	constexpr double MAX_RAW_AGE = 0.15;
	// do not run an update call for segments shorter than this
	constexpr float MIN_SEGMENT = 1e-5f;

	// dual mode: one click pushes both players right after each other; the
	// second one must reuse the timestamp of the first raw event
	double g_lastCaptureAt = -1.0;
	double g_lastCaptureTime = 0.0;
	bool g_lastCapturePush = false;

	void resetState() {
		g_pending.clear();
		g_firstFrame = true;
		g_lastUpdateTime = 0.0;
		g_lastCaptureAt = -1.0;
		rawInputClear();
	}

	bool shouldDefer(PlayerObject* player) {
		if (g_replaying || g_softToggle || g_firstFrame) return false;
		PlayLayer* pl = PlayLayer::get();
		if (!pl) return false;
		if (player != pl->m_player1 && player != pl->m_player2) return false; // editor, menu...
		if (pl->m_isPaused || pl->m_isDead || pl->m_hasCompletedLevel) return false;
		return true;
	}

	void capture(PlayerObject* player, PlayerButton button, bool push) {
		double now = qpcNow();
		double t = now;
		if (g_lastCaptureAt >= 0.0 && now - g_lastCaptureAt < 0.001 && g_lastCapturePush == push) {
			t = g_lastCaptureTime; // second player of the same click
		} else if (!rawInputTake(push, now, MAX_RAW_AGE, t)) {
			t = now; // no raw event (controller, Wine...): behave like vanilla
		}
		if (!g_pending.empty() && t < g_pending.back().time) t = g_pending.back().time; // keep order
		g_lastCaptureAt = now;
		g_lastCaptureTime = t;
		g_lastCapturePush = push;
		g_pending.push_back({ t, player, button, push });
	}

	// The replay calls PlayerObject::pushButton/releaseButton through the jmp
	// Geode put at the start of the function (resolved at load time), so hooks
	// placed on top of Geode's are not hit a second time.
#ifdef GEODE_IS_WINDOWS
	using PlayerButtonFn = void(__thiscall*)(PlayerObject*, PlayerButton);
	PlayerButtonFn g_directPush = nullptr;
	PlayerButtonFn g_directRelease = nullptr;

	PlayerButtonFn resolveDirect(uintptr_t offset) {
		auto addr = reinterpret_cast<uint8_t*>(geode::base::get() + offset);
		if (addr[0] != 0xE9) return nullptr; // not a plain jmp: use the normal call
		int32_t rel;
		std::memcpy(&rel, addr + 1, sizeof(rel));
		return reinterpret_cast<PlayerButtonFn>(addr + 5 + rel);
	}
#endif

	void apply(PendingInput const& in) {
		if (!in.player) return;
		g_replaying = true;
#ifdef GEODE_IS_WINDOWS
		if (in.push && g_directPush) g_directPush(in.player, in.button);
		else if (!in.push && g_directRelease) g_directRelease(in.player, in.button);
		else
#endif
		if (in.push) in.player->pushButton(in.button);
		else in.player->releaseButton(in.button);
		g_replaying = false;
	}

	void applyAll(std::vector<PendingInput> const& inputs, size_t from = 0) {
		for (size_t i = from; i < inputs.size(); ++i) apply(inputs[i]);
	}
}

class $modify(PlayerObject) {
	void pushButton(PlayerButton button) {
		if (shouldDefer(this)) {
			capture(this, button, true);
			return;
		}
		PlayerObject::pushButton(button);
	}

	void releaseButton(PlayerButton button) {
		if (shouldDefer(this)) {
			capture(this, button, false);
			return;
		}
		PlayerObject::releaseButton(button);
	}
};

class $modify(PlayLayer) {
	bool init(GJGameLevel* level) {
		resetState();
		return PlayLayer::init(level);
	}

	void resetLevel() {
		resetState();
		PlayLayer::resetLevel();
	}

	void update(float dt) {
		double now = qpcNow();
		double prev = g_lastUpdateTime;
		g_lastUpdateTime = now;

		double span = now - prev; // real time covered by this update
		bool noSplit = g_softToggle
			|| g_firstFrame
			|| prev == 0.0
			|| span <= 0.0
			|| span > MAX_SPLIT_FRAME
			|| dt <= 0.f
			|| this->m_isPaused
			|| this->m_isDead
			|| this->m_hasCompletedLevel;

		if (noSplit) {
			// apply anything we are still holding, then run a normal frame
			std::vector<PendingInput> inputs;
			inputs.swap(g_pending);
			applyAll(inputs);

			if (span > MAX_SPLIT_FRAME || prev == 0.0) rawInputClear();
			g_firstFrame = false;
			PlayLayer::update(dt);
			return;
		}

		if (g_pending.empty()) {
			PlayLayer::update(dt);
			return;
		}

		std::vector<PendingInput> inputs;
		inputs.swap(g_pending);

		float done = 0.f;
		size_t i = 0;
		for (; i < inputs.size(); ++i) {
			// if the player died/finished in the previous segment, stop splitting
			if (this->m_isDead || this->m_hasCompletedLevel) break;

			double f = std::clamp((inputs[i].time - prev) / span, 0.0, 1.0);
			float segment = dt * static_cast<float>(f) - done;
			if (segment > MIN_SEGMENT) {
				PlayLayer::update(segment);
				done += segment;
			}
			apply(inputs[i]);
		}
		applyAll(inputs, i); // anything left (death/complete) goes in immediately

		float rest = dt - done;
		if (rest > MIN_SEGMENT) PlayLayer::update(rest);
	}

	// disable progress in safe mode
	void levelComplete() {
		bool testMode = this->m_isTestMode;
		if (g_safeMode && !g_softToggle) this->m_isTestMode = true;

		PlayLayer::levelComplete();

		this->m_isTestMode = testMode;
	}

	// disable new best popup in safe mode
	void showNewBest(bool p0, int p1, int p2, bool p3, bool p4, bool p5) {
		if (!g_safeMode || g_softToggle) PlayLayer::showNewBest(p0, p1, p2, p3, p4, p5);
	}
};

class $modify(GJGameLevel) {
	void savePercentage(int percent, bool practice, int clicks, int attempts, bool valid) {
		if (g_safeMode && !g_softToggle) return;

		// rated levels are not valid for leaderboards when CBF is active
		if (!g_softToggle && this->m_stars.value() > 0) valid = false;

		GJGameLevel::savePercentage(percent, practice, clicks, attempts, valid);
	}
};

$on_mod(Loaded) {
	g_softToggle = Mod::get()->getSettingValue<bool>("soft-toggle");
	listenForSettingChanges<bool>("soft-toggle", +[](bool v) {
		g_softToggle = v;
		resetState();
	});

	g_safeMode = Mod::get()->getSettingValue<bool>("safe-mode");
	listenForSettingChanges<bool>("safe-mode", +[](bool v) {
		g_safeMode = v;
	});

#ifdef GEODE_IS_WINDOWS
	// Must run now, while nobody else has patched these functions yet.
	g_directPush = resolveDirect(0x1f4e40);    // PlayerObject::pushButton
	g_directRelease = resolveDirect(0x1f4f70); // PlayerObject::releaseButton
	log::info("CBF direct replay: push={} release={}", (void*)g_directPush, (void*)g_directRelease);
#endif

	rawInputStart();
}
