#include "Tracer.h"
#include <Arduino.h>

Print* _traceToPtr = nullptr;
char _traceMsg[128];


void Tracer::traceTo(Print& dest)
{
    _traceToPtr = &dest;
}


void Tracer::trace(String format, ...)
{
    if (_traceToPtr == nullptr)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(_traceMsg, sizeof(_traceMsg), format.c_str(), args);
    va_end(args);

    _traceToPtr->print(_traceMsg);
}


void Tracer::traceFreeHeap()
{
#ifdef ESP32
    traceHeapStats(
        "Internal",
        ESP.getHeapSize(),
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        ESP.getMaxAllocHeap()
        );

    traceHeapStats(
        "PSRAM",
        ESP.getPsramSize(),
        ESP.getFreePsram(),
        ESP.getMinFreePsram(),
        ESP.getMaxAllocPsram()
        );
#else
    trace(F("Heap statistics:\n"));
    trace(F("\t%u bytes free\n"), ESP.getFreeHeap());
    trace(F("\tLargest free block: %u\n"), ESP.getMaxFreeBlockSize());
#endif
}


void Tracer::traceHeapStats(const char* heapName, uint32_t total, uint32_t free, uint32_t minFree, uint32_t largest)
{
    total /= 100; // Calculate percentages
    if (total == 0) return;

    trace(F("%s heap statistics:\n"), heapName);
    trace(F("\t%u bytes free (%d %%)\n"), free, free / total);
    trace(F("\t%u bytes free minimal (%d %%)\n"), minFree, minFree / total);
    trace(F("\tLargest free block: %u\n"), largest);
}


void Tracer::hexDump(uint8_t* data, size_t length)
{
    if (_traceToPtr == nullptr) return;
    
    for (int row = 0; row < length; row += 16)
    {
        // Output hex values
        for (int col = 0; col < 16; col++ )
        {
            int index = row + col;
            if (index < length)
                _traceToPtr->printf("%02X ", data[index]);
            else
                _traceToPtr->print("   ");
            if (col == 7)
                _traceToPtr->print(" ");
        }

        // Output ASCII representation
        for (int col = 0; col < 16; col++ )
        {
            int index = row + col;
            char ascii = ' ';
            if (index < length)
                ascii = data[index];
            if ((ascii < 32) || (ascii > 127)) ascii = '.';
            _traceToPtr->printf("%c ", ascii);
            if (col == 7)
                _traceToPtr->print(" ");
        }

        _traceToPtr->println();
    }
}

//Constructor
Tracer::Tracer(String name, const char* arg)
{
    static char coreID[16];
#ifdef ESP32    
    snprintf(coreID, sizeof(coreID), "[Core #%d]", xPortGetCoreID());
#else
    coreID[0] = 0;
#endif 

    _name = name;
    if (arg == nullptr)
        trace(F("%s() entry %s\n"), _name.c_str(), coreID);
    else
        trace(F("%s(\"%s\") entry %s\n"), _name.c_str(), arg, coreID);
    _startMicros = micros();
}

//Destructor
Tracer::~Tracer()
{
    float duration = float(micros() - _startMicros) / 1000;
    trace(F("%s exit. Duration: %0.1f ms.\n"), _name.c_str(), duration);
}
