#ifndef STRINGBUILDER_H
#define STRINGBUILDER_H

#include <stdlib.h>
#include <WString.h>

class StringBuilder : public String
{
  public:
    // Constructor
    StringBuilder(size_t size);

    void clear();
    void println(const char* str);
    void println(const __FlashStringHelper* fstr);
    void printf(const char* format, ...);
    void printf(const __FlashStringHelper* fformat, ...);

  protected:
    size_t _space;
    
    void update_length(size_t additional);
};

#endif