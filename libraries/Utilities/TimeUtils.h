#ifndef TIMEUTILS_H
#define TIMEUTILS_H

#include <time.h>

const char* formatTime(const char* format, time_t time)
{
    static char result[32];
    strftime(result, sizeof(result), format, localtime(&time));
    return result;
}

const char* formatTimeSpan(uint32_t seconds)
{
    static char result[16];
    snprintf(
        result,
        sizeof(result),
        "%02d:%02d:%02d",
        seconds / 3600,
        (seconds / 60) % 60,
        seconds % 60);
    return result;
}

#endif