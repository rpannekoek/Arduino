#ifndef TRACER_H
#define TRACER_H

#include <Arduino.h>
#include <WString.h>

#define TRACE(...) Tracer::trace(__VA_ARGS__)


class Tracer
{
  public:
    Tracer(String name, const char* arg = nullptr);
    ~Tracer();

    static void traceTo(Print& dest);
    static void trace(String format, ...);
    static void traceFreeHeap();

  private:
    String _name;
    unsigned long _startMicros;

    static void traceHeapStats(const char* heapName, uint32_t total, uint32_t free, uint32_t minFree, uint32_t largest);
};

#endif