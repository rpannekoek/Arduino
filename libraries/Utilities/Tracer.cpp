#include "Tracer.h"
#include <Arduino.h>

Print* _traceToPtr = NULL;
char _traceMsg[128];


void Tracer::traceTo(Print& dest)
{
    _traceToPtr = &dest;
}


void Tracer::trace(String format, ...)
{
    if (_traceToPtr == NULL)
        return;

    va_list args;
    va_start(args, format);
    vsnprintf(_traceMsg, sizeof(_traceMsg), format.c_str(), args);
    va_end(args);

    _traceToPtr->print(_traceMsg);
}


//Constructor
Tracer::Tracer(String name, const char* arg)
{
    _name = name;
    if (arg == NULL)
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
