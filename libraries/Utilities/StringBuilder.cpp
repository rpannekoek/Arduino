#include <Arduino.h>
#include <PSRAM.h>
#include "StringBuilder.h"


#ifdef ESP32
    // Small String Optimization is implemented in recent versions of the SDK
    #define SSO true
#endif

#ifdef SSO
    #define CAPACITY capacity()
#else
    #define CAPACITY capacity
#endif

// Constructor
StringBuilder::StringBuilder(size_t size)
    : String((const char*)nullptr) // Minimal String init
{
#ifdef SSO
    setBuffer((char*) malloc(size));
    setCapacity(size - 1);
#else
    buffer = (char*) malloc(size);
    capacity = size - 1;
#endif
    _space = CAPACITY;
}


void StringBuilder::clear()
{
    set_length(0);
    _space = CAPACITY;
}


void StringBuilder::printf(const __FlashStringHelper* fformat, ...)
{
    if (_space == 0)
        return;

    va_list args;
    va_start(args, fformat);
    size_t additional = vsnprintf_P(end(), _space, (PGM_P) fformat, args);
    va_end(args);

    update_length(additional);
}


size_t StringBuilder::write(uint8_t data)
{
    return write(&data, 1);
}


size_t StringBuilder::write(const uint8_t* dataPtr, size_t size)
{
    if (_space <= 1) 
        return 0;
 
    if (size >= _space)
        size = (_space - 1);

    memcpy(end(), dataPtr, size);
    end()[size] = 0; 

    update_length(size);

    return size;
}


void StringBuilder::set_length(size_t value)
{
#ifdef SSO
    setLen(value);
#else
    len = value;
#endif
}


void StringBuilder::update_length(size_t additional)
{
    unsigned int newLength = length() + additional;  
    set_length(std::min(newLength, CAPACITY)); 
}
