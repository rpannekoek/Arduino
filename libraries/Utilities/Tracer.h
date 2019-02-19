#ifndef TRACER_H
#define TRACER_H

#include <Arduino.h>
#include <WString.h>

#define TRACE(...) Tracer::trace(__VA_ARGS__)


class Tracer
{
  public:
    Tracer(String name, const char* arg = NULL);
    ~Tracer();

    static void traceTo(Print& dest);
    static void trace(String format, ...);
    static void traceFreeHeap();

  private:
    String _name;
    unsigned long _startMicros;
};

#endif