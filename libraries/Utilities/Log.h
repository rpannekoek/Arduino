#ifndef LOG_H
#define LOG_H

#include <c_types.h>

template <class T>
class Log
{
    public:
        // Constructor 
        Log(uint16_t size)
            : _size(size)
        {
            _entriesPtr = new T*[size];
            memset(_entriesPtr, 0, size * 4);
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        // Destructor 
        ~Log()
        {
            clear();
            delete[] _entriesPtr;
        }

        uint16_t count()
        {
            return _count;
        }

        void clear()
        {
            for (int i = _start; i < _end; i = (i + 1) % _size)
            {
                T* entry = _entriesPtr[i]; 
                delete entry;
                _entriesPtr[i] = nullptr;
            }

            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;

            memset(_entriesPtr, 0, _size * 4);
        }

        void add(T* entry)
        {
            if ((_end == _start) && (_entriesPtr[_end] != nullptr))
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

        T* getFirstEntry()
        {
            _iterator = _start;
            return _entriesPtr[_iterator];
        }

        T* getEntryFromEnd(uint16_t n)
        {
            if ((n == 0) || (n > _count))
                return nullptr;
            
            if (_end < n)
                _iterator = _end + _size - n;
            else
                _iterator = _end - n;

            return _entriesPtr[_iterator];
        }

        T* getNextEntry()
        {
            _iterator = (_iterator + 1) % _size;
            if (_iterator == _end)
                return nullptr;
            else 
                return _entriesPtr[_iterator];
        }

    protected:
        uint16_t _size;
        uint16_t _start;
        uint16_t _end;
        uint16_t _count;
        uint16_t _iterator; 
        T** _entriesPtr;
};
#endif