#pragma once

#include <cstdarg>
#include <syslog.h>

// syslog wrapper. Messages appear in `log stream` via:
//   log stream --predicate 'senderImagePath CONTAINS "GLAInjector" AND message CONTAINS "GLA:"' --level info
static inline void glaLog (int priority, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static inline void glaLog (int priority, const char* fmt, ...)
{
    va_list args;
    va_start (args, fmt);
    vsyslog (priority, fmt, args);
    va_end (args);
}
