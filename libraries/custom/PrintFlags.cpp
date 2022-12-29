#include "PrintFlags.h"
#include <string.h>

char _printedFlags[64];


const char* printFlags(unsigned int flags, const char** flagNames, int numFlags, const char* separator)
{
    _printedFlags[0] = 0;

    unsigned int bit = 1;
    for (int i = 0; i < numFlags; i++)
    {
        if (flags & bit)
        {
            if (strlen(_printedFlags) > 0)
                strcat(_printedFlags, separator);
            strcat(_printedFlags, flagNames[i]);
        }
        bit <<= 1;
    }

    return _printedFlags;
}
