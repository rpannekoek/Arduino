#include <Arduino.h>
#include <PSRAM.h>
#include "StringBuilder.h"

// Constructor
StringBuilder::StringBuilder(size_t size)
    : String((const char*)nullptr) // Minimal String init
{
    setBuffer((char*) malloc(size));
    setCapacity(size - 1);
    _space = capacity();
}


void StringBuilder::clear()
{
    setLen(0);
    _space = capacity();
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


void StringBuilder::update_length(size_t additional)
{
    unsigned int newLength = length() + additional;  
    setLen(std::min(newLength, capacity())); 
}
