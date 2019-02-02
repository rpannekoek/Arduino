#include "Tracer.h"
#include <Arduino.h>

Print* _traceToPtr = NULL;
char _traceMsg[128];


void Tracer::traceTo(Print& dest)
{
    _traceToPtr = &dest;
}


void Tracer::trace(const char* format, ...)
{
    if (_traceToPtr == NULL)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(_traceMsg, sizeof(_traceMsg), format, args);
    va_end(args);

    _traceToPtr->print(_traceMsg);
}


//Constructor
Tracer::Tracer(const char* name, const char* arg)
{
    _name = name;
    if (arg == NULL)
        trace("%s() entry\n", _name);
    else
        trace("%s(\"%s\") entry\n", _name, arg);
    _startMicros = micros();
}

//Destructor
Tracer::~Tracer()
{
    float duration = float(micros() - _startMicros) / 1000;
    trace("%s exit. Duration: %0.1f ms.\n", _name, duration);
}
