#pragma once

// High resolution clock in seconds (QueryPerformanceCounter on Windows).
double qpcNow();

// Starts the raw input thread (Windows only, no-op elsewhere). Safe to call more than once.
void rawInputStart();

// Takes the oldest raw press (press = true) or release (press = false) event
// that is not older than maxAge seconds before `now`.
// Returns false if there is none, in which case the caller should use `now`.
bool rawInputTake(bool press, double now, double maxAge, double& outTime);

// Forget every pending raw event.
void rawInputClear();
