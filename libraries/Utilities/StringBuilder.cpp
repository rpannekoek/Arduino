#include <Arduino.h>
#include <StringBuilder.h>

// Constructor
StringBuilder::StringBuilder(size_t size)
{
    buffer = (char*) malloc(size);
    capacity = size - 1;
    _space = capacity;
}


void StringBuilder::clear()
{
    len = 0;
    _space = capacity;
}


void StringBuilder::println(const char* str)
{
    if (_space == 0) return;
    
    strncpy(end(), str, _space);
    update_length(strlen(str));

    strncpy(end(), "\r\n", _space);
    update_length(2);
}


void StringBuilder::println(const __FlashStringHelper* fstr)
{
    if (_space == 0) return;

    PGM_P pstr = (PGM_P) fstr;
    strncpy_P(end(), pstr, _space);
    update_length(strlen_P(pstr));

    strncpy(end(), "\r\n", _space);
    update_length(2);
}


void StringBuilder::printf(const char* format, ...)
{
    if (_space == 0) return;

    va_list args;
    va_start(args, format);
    size_t additional = vsnprintf(end(), _space, format, args);
    va_end(args);

    update_length(additional);
}


void StringBuilder::printf(const __FlashStringHelper* fformat, ...)
{
    if (_space == 0) return;

    va_list args;
    va_start(args, fformat);
    size_t additional = vsnprintf_P(end(), _space, (PGM_P) fformat, args);
    va_end(args);

    update_length(additional);
}


void StringBuilder::update_length(size_t additional)
{
    if (additional <= _space)
    {
    len += additional;
    _space -= additional;
    }
    else
    {
    Serial.println("StringBuilder buffer is full!");
    len += _space;
    _space = 0;
    }
}
