// Host stand-in for the Arduino header SAM includes; flash and RAM are one thing here.
#pragma once
#define PROGMEM
#define pgm_read_byte(a) (*(const unsigned char *)(a))
