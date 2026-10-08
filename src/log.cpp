#include "log.h"

#include <Arduino.h>
#include <stdarg.h>

void logLine(const char *tag, const char *fmt, ...) {
  char line[256];
  int n = snprintf(line, sizeof(line), "[%s] ", tag);
  if (n < 0 || n >= (int)sizeof(line)) return;
  va_list ap;
  va_start(ap, fmt);
  int m = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  if (m < 0) return;
  n += m;
  if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';
  Serial.write((const uint8_t *)line, n);
}
