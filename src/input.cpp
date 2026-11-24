#include "input.hpp"

#include <deque>
#include <mutex>
#include <chrono>

#ifdef _WIN32

#include <Windows.h>

namespace {
	struct RawEvent {
		double time;
		bool press;
	};

	std::once_flag g_startOnce;
	LARGE_INTEGER g_freq = {};
	std::mutex g_mutex;
	std::deque<RawEvent> g_events;

	// only touched by the raw input thread
	bool g_keyHeld[256] = {};
	bool g_leftHeld = false;

	void pushEvent(bool press) {
		double t = qpcNow(); // timestamp as early as possible
		std::lock_guard<std::mutex> lock(g_mutex);
		g_events.push_back({ t, press });
		if (g_events.size() > 64) g_events.pop_front();
	}

	// keys that make P1/P2 jump in GD 2.1: space, up arrow, W
	bool isJumpKey(USHORT vk) {
		return vk == VK_SPACE || vk == VK_UP || vk == 'W';
	}

	LRESULT CALLBACK rawWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		if (msg == WM_INPUT) {
			alignas(8) BYTE buffer[256];
			UINT size = sizeof(buffer);
			UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));
			if (got != static_cast<UINT>(-1)) {
				const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
				if (raw->header.dwType == RIM_TYPEKEYBOARD) {
					const RAWKEYBOARD& kb = raw->data.keyboard;
					USHORT vk = kb.VKey;
					if (vk < 256 && isJumpKey(vk)) {
						bool isUp = (kb.Flags & RI_KEY_BREAK) != 0;
						if (!isUp && !g_keyHeld[vk]) {
							g_keyHeld[vk] = true;
							pushEvent(true);
						}
						else if (isUp && g_keyHeld[vk]) {
							g_keyHeld[vk] = false;
							pushEvent(false);
						}
					}
				}
				else if (raw->header.dwType == RIM_TYPEMOUSE) {
					USHORT flags = raw->data.mouse.usButtonFlags;
					if ((flags & RI_MOUSE_LEFT_BUTTON_DOWN) && !g_leftHeld) {
						g_leftHeld = true;
						pushEvent(true);
					}
					if ((flags & RI_MOUSE_LEFT_BUTTON_UP) && g_leftHeld) {
						g_leftHeld = false;
						pushEvent(false);
					}
				}
			}
			return 0;
		}
		return DefWindowProcA(hwnd, msg, wParam, lParam);
	}

	DWORD WINAPI rawThread(LPVOID) {
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

		WNDCLASSA wc = {};
		wc.lpfnWndProc = rawWndProc;
		wc.hInstance = GetModuleHandleA(nullptr);
		wc.lpszClassName = "CBF2113RawInput";
		if (!RegisterClassA(&wc)) return 1;

		HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
		if (!hwnd) return 1;

		RAWINPUTDEVICE devices[2] = {};
		devices[0].usUsagePage = 0x01; // generic desktop
		devices[0].usUsage = 0x06;     // keyboard
		devices[0].dwFlags = RIDEV_INPUTSINK;
		devices[0].hwndTarget = hwnd;
		devices[1].usUsagePage = 0x01;
		devices[1].usUsage = 0x02;     // mouse
		devices[1].dwFlags = RIDEV_INPUTSINK;
		devices[1].hwndTarget = hwnd;
		if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) return 1;

		MSG msg;
		while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageA(&msg);
		}
		return 0;
	}
}

double qpcNow() {
	if (g_freq.QuadPart == 0) QueryPerformanceFrequency(&g_freq);
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

void rawInputStart() {
	std::call_once(g_startOnce, [] {
		QueryPerformanceFrequency(&g_freq);
		HANDLE h = CreateThread(nullptr, 0, rawThread, nullptr, 0, nullptr);
		if (h) CloseHandle(h);
	});
}

#else

namespace {
	std::mutex g_mutex;
	std::deque<int> g_events;
}

double qpcNow() {
	using namespace std::chrono;
	return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void rawInputStart() {}

#endif

bool rawInputTake(bool press, double now, double maxAge, double& outTime) {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(g_mutex);
	while (!g_events.empty() && now - g_events.front().time > maxAge) g_events.pop_front();
	for (auto it = g_events.begin(); it != g_events.end(); ++it) {
		if (it->press == press) {
			outTime = it->time;
			g_events.erase(it);
			return true;
		}
	}
#else
	(void)press; (void)now; (void)maxAge; (void)outTime;
#endif
	return false;
}

void rawInputClear() {
#ifdef _WIN32
	std::lock_guard<std::mutex> lock(g_mutex);
	g_events.clear();
#endif
}
