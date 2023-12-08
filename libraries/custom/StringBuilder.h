#ifndef STRINGBUILDER_H
#define STRINGBUILDER_H

#include <stdlib.h>
#include <WString.h>
#include <Print.h>

class StringBuilder : public Print
{
  public:
    // Constructor
    StringBuilder(size_t size);

    void clear();
    void printf(const __FlashStringHelper* fformat, ...);

    // Overrides for virtual Print methods
    virtual size_t write(uint8_t);
    virtual size_t write(const uint8_t *buffer, size_t size);

    inline size_t length()
    {
        return _length;
    }

    const char* c_str() const
    {
        return _buffer;
    }

    operator const char*() const
    {
        return _buffer;
    }

#ifdef ESP32
    // WebServer.send for ESP32 only accepts String references, so we provide a String cast operator here.
    // The String constructor copies the contents of the buffer, which partially defeats the purpose of StringBuilder.
    // However, an ESP32 typically has plenty of RAM, so this is not so much of an issue.
    operator String() const
    {
        return String(_buffer);
    }
#endif

  protected:
    char* _buffer;
    size_t _capacity;
    size_t _space;
    size_t _length;
    
    void update_length(size_t additional);
};

#endif