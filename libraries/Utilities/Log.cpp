#include "Log.h"

// Constructor
Log::Log(uint16_t size)
    : _size(size)
{
    _entriesPtr = new void*[size];
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
    }

    _start = 0;
    _end = 0;
    _count = 0;
    _iterator = 0;
}


void Log::add(void* entry)
{
    _entriesPtr[_end] = entry;

    _end = (_end + 1) % _size;
    if (_end == _start)
    {
        // Log is full; drop oldest entry.
        delete _entriesPtr[_start];
        _start = (_start + 1) % _size;
    }
    else
        _count++;
}


void* Log::getFirstEntry()
{
    _iterator = _start;
    if (_iterator == _end)
        return NULL;
    else 
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
