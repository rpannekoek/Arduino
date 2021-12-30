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

    operator const char*() const
    {
        return _buffer;
    }

  protected:
    char* _buffer;
    size_t _capacity;
    size_t _space;
    size_t _length;
    
    void update_length(size_t additional);
};

#endif