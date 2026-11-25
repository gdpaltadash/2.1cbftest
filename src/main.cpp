#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
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
2. GJBaseGameLayer::pushButton/releaseButton are intercepted while playing. The
   call is not forwarded to the game; it is stored together with the timestamp
   of the matching raw event.
3. PlayLayer::update(dt) is split. The part of the frame before the input runs
   with PlayLayer::update(dt * fraction), then the stored input is replayed,
   then the remaining part of the frame runs.

Frames without any input run exactly like vanilla (a single update call).
*/

namespace {
	struct PendingInput {
		double time;
		int button;
		bool isPlayer1;
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

	void resetState() {
		g_pending.clear();
		g_firstFrame = true;
		g_lastUpdateTime = 0.0;
		rawInputClear();
	}

	bool shouldDefer(GJBaseGameLayer* layer) {
		if (g_replaying || g_softToggle || g_firstFrame) return false;
		PlayLayer* pl = PlayLayer::get();
		if (!pl) return false;
		if (static_cast<GJBaseGameLayer*>(pl) != layer) return false; // editor, or some other layer
		if (pl->m_isPaused || pl->m_isDead || pl->m_hasCompletedLevel) return false;
		return true;
	}

	void capture(bool push, int button, bool isPlayer1) {
		double now = qpcNow();
		double t = now;
		if (!rawInputTake(push, now, MAX_RAW_AGE, t)) t = now; // no raw event (controller, Wine...): behave like vanilla
		if (!g_pending.empty() && t < g_pending.back().time) t = g_pending.back().time; // keep order
		g_pending.push_back({ t, button, isPlayer1, push });
	}

	// --- DIAGNOSTIC (temporary): logs every button call so we can see how many
	// times GD / our replay call each function per physical click.
	constexpr bool DIAG = true;
	int g_diagSeq = 0;
	template <class... Args>
	void diag(fmt::format_string<Args...> f, Args&&... args) {
		if (!DIAG) return;
		log::info("[diag {} t={}] {}", ++g_diagSeq, fmt::format("{:.4f}", qpcNow()), fmt::format(f, std::forward<Args>(args)...));
	}

	// --- MegaHack CPS fix -------------------------------------------------
	// Replaying through layer->pushButton() re-enters the hook chain from the
	// very top, so any inline hook installed after Geode (MegaHack's CPS
	// counter) sees the click twice: once when GD makes the original call
	// (which we swallow) and again on our replay.
	// Fix: at load time (before anybody else patches the function) remember
	// where the function's first jmp points (Geode's handler) and replay by
	// calling that directly. Our detour + the rest of the Geode chain still
	// run, but hooks placed on top of Geode's are skipped.
#ifdef GEODE_IS_WINDOWS
	using ButtonFn = void(__thiscall*)(GJBaseGameLayer*, int, bool);
	ButtonFn g_directPush = nullptr;
	ButtonFn g_directRelease = nullptr;

	ButtonFn resolveDirect(uintptr_t offset) {
		auto addr = reinterpret_cast<uint8_t*>(geode::base::get() + offset);
		if (addr[0] != 0xE9) return nullptr; // not a plain jmp: use the normal call
		int32_t rel;
		std::memcpy(&rel, addr + 1, sizeof(rel));
		return reinterpret_cast<ButtonFn>(addr + 5 + rel);
	}
#endif

	void apply(GJBaseGameLayer* layer, PendingInput const& in) {
		diag("replay {} btn={} p1={}", in.push ? "push" : "release", in.button, in.isPlayer1);
		g_replaying = true;
#ifdef GEODE_IS_WINDOWS
		if (in.push && g_directPush) g_directPush(layer, in.button, in.isPlayer1);
		else if (!in.push && g_directRelease) g_directRelease(layer, in.button, in.isPlayer1);
		else
#endif
		if (in.push) layer->pushButton(in.button, in.isPlayer1);
		else layer->releaseButton(in.button, in.isPlayer1);
		g_replaying = false;
	}

	void applyAll(GJBaseGameLayer* layer, std::vector<PendingInput> const& inputs, size_t from = 0) {
		for (size_t i = from; i < inputs.size(); ++i) apply(layer, inputs[i]);
	}
}

class $modify(GJBaseGameLayer) {
	void pushButton(int button, bool isPlayer1) {
		bool d = shouldDefer(this);
		diag("GJBGL::pushButton btn={} p1={} replaying={} defer={}", button, isPlayer1, g_replaying, d);
		if (d) {
			capture(true, button, isPlayer1);
			return;
		}
		GJBaseGameLayer::pushButton(button, isPlayer1);
	}

	void releaseButton(int button, bool isPlayer1) {
		bool d = shouldDefer(this);
		diag("GJBGL::releaseButton btn={} p1={} replaying={} defer={}", button, isPlayer1, g_replaying, d);
		if (d) {
			capture(false, button, isPlayer1);
			return;
		}
		GJBaseGameLayer::releaseButton(button, isPlayer1);
	}
};

// diagnostic only: shows how often the player object itself is pushed
class $modify(PlayerObject) {
	void pushButton(PlayerButton b) {
		diag("PlayerObject::pushButton btn={} this={} replaying={}", (int)b, (void*)this, g_replaying);
		PlayerObject::pushButton(b);
	}
	void releaseButton(PlayerButton b) {
		diag("PlayerObject::releaseButton btn={} this={} replaying={}", (int)b, (void*)this, g_replaying);
		PlayerObject::releaseButton(b);
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
		GJBaseGameLayer* base = this;

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
			applyAll(base, inputs);

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
		diag("PlayLayer::update SPLIT inputs={} dt={}", inputs.size(), dt);

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
			apply(base, inputs[i]);
		}
		applyAll(base, inputs, i); // anything left (death/complete) goes in immediately

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
	g_directPush = resolveDirect(0x111500);    // GJBaseGameLayer::pushButton
	g_directRelease = resolveDirect(0x111660); // GJBaseGameLayer::releaseButton
	log::info("CBF direct replay: push={} release={}", (void*)g_directPush, (void*)g_directRelease);
#endif

	rawInputStart();
}
