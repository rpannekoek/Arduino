#ifndef TRACER_H
#define TRACER_H

#include <Arduino.h>
#include <WString.h>

class Tracer
{
  public:
    //Constructor
    Tracer(const char* name, const char* arg = NULL)
    {
      _name = name;
      if (arg == NULL)
        Serial.printf("%s() entry\n", _name);
      else
        Serial.printf("%s(%s) entry\n", _name, arg);
      _startMicros = micros();
    }

    //Destructor
    ~Tracer()
    {
      float duration = float(micros() - _startMicros) / 1000;
      Serial.printf("%s exit. Duration: %0.1f ms.\n", _name, duration);
    }

  private:
    const char* _name;
    unsigned long _startMicros;
};

#endif