#include "PersistentDataBase.h"
#include "PrintHex.h"
#include "Tracer.h"
#include <EEPROM.h>

#define INITIALIZED_MAGIC 0xCAFEBABE

// Constructor
PersistentDataBase::PersistentDataBase(size_t dataSize)
    : _dataSize(dataSize + sizeof(Magic))
{
    EEPROM.begin(512);
}


// Destructor
PersistentDataBase::~PersistentDataBase()
{
    EEPROM.end();
}


void PersistentDataBase::writeToEEPROM()
{
    Tracer tracer("PersistentDataBase::writeToEEPROM");

    byte* bytePtr = (byte*) this;
    TRACE("Writing %d bytes to EEPROM from %p ...\n", _dataSize, bytePtr);
    printHex(bytePtr, _dataSize);

    Magic = INITIALIZED_MAGIC;
    for (size_t i = 0; i < _dataSize; i++)
        EEPROM.write(i, *bytePtr++);

    EEPROM.commit();
}


bool PersistentDataBase::readFromEEPROM()
{
    Tracer tracer("PersistentDataBase::readFromEEPROM");

    byte* bytePtr = (byte*) this;
    TRACE("Reading %d bytes from EEPROM to %p ...\n", _dataSize, bytePtr); 

    for (size_t i = 0; i < _dataSize; i++)
        *bytePtr++ = EEPROM.read(i);

    printHex((byte*) this, _dataSize);

    return (Magic == INITIALIZED_MAGIC);
}
