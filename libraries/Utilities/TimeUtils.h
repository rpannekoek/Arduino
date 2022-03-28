#ifndef TIMEUTILS_H
#define TIMEUTILS_H

#include <time.h>

const char* formatTime(const char* format, time_t time)
{
    static char timeString[32];
    strftime(timeString, sizeof(timeString), format, localtime(&time));
    return timeString;
}

#endif