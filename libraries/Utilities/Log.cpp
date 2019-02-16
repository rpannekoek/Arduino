#include "Log.h"

// Constructor
Log::Log(uint16_t size)
    : _size(size)
{
    _entriesPtr = new void*[size];
    memset(_entriesPtr, 0, sizeof(_entriesPtr));
    _start = 0;
    _end = 0;
    _count = 0;
    _iterator = 0;
}

// Destructor
Log::~Log()
{
    clear();
    delete[] _entriesPtr;
}


uint16_t Log::count()
{
    return _count;
}


void Log::clear()
{
    for (int i = _start; i < _end; i = (i + 1) % _size)
    {
        void* entry = _entriesPtr[i]; 
        delete entry;
        _entriesPtr[i] = NULL;
    }

    _start = 0;
    _end = 0;
    _count = 0;
    _iterator = 0;
}


void Log::add(void* entry)
{
   if ((_end == _start) && (_entriesPtr[_end] != NULL))
    {
        // Log is full; drop oldest entry.
        delete _entriesPtr[_start];
        _start = (_start + 1) % _size;
    }
    else
        _count++;
        
    _entriesPtr[_end] = entry;

    _end = (_end + 1) % _size;
}


void* Log::getFirstEntry()
{
    _iterator = _start;
    return _entriesPtr[_iterator];
}


void* Log::getEntryFromEnd(uint16_t n)
{
    _iterator = _end - n - 1;
    if (_iterator < 0)
        _iterator += _size;
    return _entriesPtr[_iterator];
}


void* Log::getNextEntry()
{
    _iterator = (_iterator + 1) % _size;
    if (_iterator == _end)
        return NULL;
    else 
        return _entriesPtr[_iterator];
}
