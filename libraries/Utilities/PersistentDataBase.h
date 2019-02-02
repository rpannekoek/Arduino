#ifndef PERSISTENT_DATA_BASE_H
#define PERSISTENT_DATA_BASE_H

#include <c_types.h>

struct PersistentDataBase
{
    public:
        uint32_t Magic = 0x55AA55AA;

        PersistentDataBase(size_t dataSize);
        ~PersistentDataBase();

        void writeToEEPROM();
        bool readFromEEPROM();
        void printData();

    private:
         size_t _dataSize;
};

#endif