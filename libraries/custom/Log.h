#ifndef LOG_H
#define LOG_H

#include <stdint.h>

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
            for (int i = 0; i < _size; i++)
            {
                T* entry = _entriesPtr[i];
                if (entry == nullptr)
                    break;
                delete entry;
                _entriesPtr[i] = nullptr;
            }

            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
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


template <class T>
class StaticLog
{
    public:
        // Constructor 
        StaticLog(uint16_t size)
        {
            _size = size;
            _entries = new T[size];
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        // Destructor 
        ~StaticLog()
        {
            delete[] _entries;
        }

        uint16_t count()
        {
            return _count;
        }

        void clear()
        {
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        T* add(T* entry)
        {
            if ((_end == _start) && (_count != 0))
                _start = (_start + 1) % _size;
            else
                _count++;

            T* newEntryPtr = _entries + _end;
            _end = (_end + 1) % _size;

            memcpy(newEntryPtr, entry, sizeof(T));                

            return newEntryPtr;
        }

        T* getFirstEntry()
        {
            _iterator = _start;
            if (_count == 0)
                return nullptr;
            else
                return _entries + _iterator;
        }

        T* getEntryFromEnd(uint16_t n)
        {
            if ((n == 0) || (n > _count))
                return nullptr;
            
            if (_end < n)
                _iterator = _end + _size - n;
            else
                _iterator = _end - n;

            return _entries + _iterator;
        }

        T* getNextEntry()
        {
            _iterator = (_iterator + 1) % _size;
            if (_iterator == _end)
                return nullptr;
            else 
                return _entries + _iterator;
        }

    protected:
        uint16_t _size;
        uint16_t _start;
        uint16_t _end;
        uint16_t _count;
        uint16_t _iterator; 
        T* _entries;
};

class StringLog
{
    public:
        // Constructor 
        StringLog(uint16_t size, uint16_t entrySize)
        {
            _size = size;
            _entrySize = entrySize;
            _entries = new char[entrySize * size];
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        // Destructor 
        ~StringLog()
        {
            delete[] _entries;
        }

        uint16_t count()
        {
            return _count;
        }

        void clear()
        {
            _start = 0;
            _end = 0;
            _count = 0;
            _iterator = 0;
        }

        const char* add(const char* entry)
        {
            if ((_end == _start) && (_count != 0))
                _start = (_start + 1) % _size;
            else
                _count++;

            char* newEntryPtr = _entries + _end * _entrySize;
            _end = (_end + 1) % _size;

            strncpy(newEntryPtr, entry, _entrySize);
            newEntryPtr[_entrySize - 1] = 0;               

            return newEntryPtr;
        }

        const char* getFirstEntry()
        {
            _iterator = _start;
            if (_count == 0)
                return nullptr;
            else
                return _entries + _iterator * _entrySize;
        }

        const char* getEntryFromEnd(uint16_t n)
        {
            if ((n == 0) || (n > _count))
                return nullptr;
            
            if (_end < n)
                _iterator = _end + _size - n;
            else
                _iterator = _end - n;

            return _entries + _iterator * _entrySize;
        }

        const char* getNextEntry()
        {
            _iterator = (_iterator + 1) % _size;
            if (_iterator == _end)
                return nullptr;
            else 
                return _entries + _iterator * _entrySize;
        }

    protected:
        uint16_t _size;
        uint16_t _entrySize;
        uint16_t _start;
        uint16_t _end;
        uint16_t _count;
        uint16_t _iterator; 
        char* _entries;
};
#endif