#ifndef LOG_H
#define LOG_H

#include <c_types.h>

class Log
{
    public:
        // Constructor 
        Log(uint16_t size);

        // Destructor 
        ~Log();

        uint16_t count();
        void clear();
        void add(void* entry);
        void* getFirstEntry();
        void* getEntryFromEnd(uint16_t n);
        void* getNextEntry();

    protected:
        uint16_t _size;
        uint16_t _start;
        uint16_t _end;
        uint16_t _count;
        uint16_t _iterator; 
        void** _entriesPtr;
};

#endif