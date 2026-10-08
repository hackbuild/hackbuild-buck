// The USB serial console. Lines starting with '/' are commands; any other line
// is spoken. Runs in the Arduino loop task.
#pragma once

void consolePoll();
void consoleBanner();
