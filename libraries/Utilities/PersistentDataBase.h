#ifndef PERSISTENT_DATA_BASE_H
#define PERSISTENT_DATA_BASE_H

#include <c_types.h>

struct PersistentDataBase
{
    public:
        PersistentDataBase(size_t dataSize);
        ~PersistentDataBase();

        void begin();
        void writeToEEPROM();
        bool readFromEEPROM();
        void printData();

        virtual void initialize() = 0;
        virtual void validate() = 0;

    private:
        size_t _dataSize;
        uint32_t _magic;
};

#endif