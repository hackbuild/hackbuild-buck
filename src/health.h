// Crash accounting, safe mode and the task watchdog.
//
// A crash counter lives in RTC memory, which survives a software reset but not a
// power cycle. Each boot after a panic or watchdog reset adds one; a power-on boot,
// /reboot, or five minutes of uptime clears it. At SAFE_MODE_CRASHES in a row BUCK
// boots with networking off, so a bad message or relay cannot hold it in a reset
// loop and serial stays usable. Five crash-free minutes in safe mode turn
// networking back on without anyone touching it.
#pragma once

#include <stdint.h>

void healthBegin();
bool healthSafeMode();
const char *healthResetReason();
uint32_t healthCrashCount();
// Call from loop(). Clears the crash count once uptime is long enough; returns true
// once, at the moment safe mode ends, so the caller can start networking.
bool healthTick();
void healthClearCrashes();  // before a deliberate restart, so it boots normally

// Task watchdog: a task that registers must call healthFeed() at least every
// WDT_TIMEOUT_MS or the chip resets with a backtrace on serial.
void healthWatchThisTask();
void healthFeed();
