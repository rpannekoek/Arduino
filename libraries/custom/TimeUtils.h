#ifndef TIMEUTILS_H
#define TIMEUTILS_H

#include <time.h>

constexpr int SECONDS_PER_MINUTE = 60;
constexpr int SECONDS_PER_HOUR = 60 * SECONDS_PER_MINUTE;
constexpr int SECONDS_PER_DAY = 24 * SECONDS_PER_HOUR;
constexpr int SECONDS_PER_WEEK = 7 * SECONDS_PER_DAY;

const char* formatTime(const char* format, time_t time)
{
    static char result[32];
    strftime(result, sizeof(result), format, localtime(&time));
    return result;
}

const char* formatTimeSpan(uint32_t seconds, bool includeHours = true)
{
    static char result[16];
    if (includeHours)
    {
        snprintf(
            result,
            sizeof(result),
            "%02d:%02d:%02d",
            seconds / 3600,
            (seconds / 60) % 60,
            seconds % 60);
    }
    else
    {
        snprintf(
            result,
            sizeof(result),
            "%02d:%02d",
            seconds / 60,
            seconds % 60);
    }
    return result;
}


time_t getStartOfDay(time_t time)
{
    tm* tmPtr = localtime(&time);
    tmPtr->tm_hour = 0;
    tmPtr->tm_min = 0;
    tmPtr->tm_sec = 0;
    return mktime(tmPtr);
}

#endif