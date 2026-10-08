// One line of serial log: "[tag] message". Safe from any task. Never blocks when
// no computer is listening, because main.cpp sets the USB CDC TX timeout to zero.
#pragma once

void logLine(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
