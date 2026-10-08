// The network task: WiFi, the CLASP and MQTT links, and status publishing.
// Everything that touches a socket runs in this one task, at a priority below
// the audio tasks, so network trouble can stall a link but never the voice.
#pragma once

#include <stddef.h>

void netBegin();                 // starts the task; does nothing in safe mode
void netReport(char *out, size_t cap);   // one paragraph for the /net command
