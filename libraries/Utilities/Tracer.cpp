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
    traceHeapStats(
        "Internal",
        ESP.getHeapSize(),
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        ESP.getMaxAllocHeap()
        );

#ifdef ESP32
    traceHeapStats(
        "PSRAM",
        ESP.getPsramSize(),
        ESP.getFreePsram(),
        ESP.getMinFreePsram(),
        ESP.getMaxAllocPsram()
        );
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


//Constructor
Tracer::Tracer(String name, const char* arg)
{
    _name = name;
    if (arg == nullptr)
        trace(F("%s() entry\n"), _name.c_str());
    else
        trace(F("%s(\"%s\") entry\n"), _name.c_str(), arg);
    _startMicros = micros();
}

//Destructor
Tracer::~Tracer()
{
    float duration = float(micros() - _startMicros) / 1000;
    trace(F("%s exit. Duration: %0.1f ms.\n"), _name.c_str(), duration);
}
