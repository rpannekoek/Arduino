#include "PersistentDataBase.h"
#include "PrintHex.h"
#include "Tracer.h"
#include <EEPROM.h>

#define INITIALIZED_MAGIC 0xCAFEBABE
#define DATA_OFFSET sizeof(Magic) + 4

// Constructor
PersistentDataBase::PersistentDataBase(size_t dataSize)
    : _dataSize(dataSize)
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

    TRACE("Writing %d + %d bytes to EEPROM...\n", _dataSize, sizeof(Magic));
    printData();
 
    // Write Magic
    Magic = INITIALIZED_MAGIC;
    byte* bytePtr = (byte*) &Magic;
    for (size_t i = 0; i < sizeof(Magic); i++)
        EEPROM.write(i, *bytePtr++);

    // Write actual data
    bytePtr = ((byte*) this) + DATA_OFFSET ;
    for (size_t i = 0; i < _dataSize; i++)
        EEPROM.write(i + sizeof(Magic), *bytePtr++);

    EEPROM.commit();
}


bool PersistentDataBase::readFromEEPROM()
{
    Tracer tracer("PersistentDataBase::readFromEEPROM");

    TRACE("Reading %d + %d bytes from EEPROM...\n", _dataSize, sizeof(Magic)); 

    // Read Magic
    byte* bytePtr = (byte*) &Magic;
    for (size_t i = 0; i < sizeof(Magic); i++)
        *bytePtr++ = EEPROM.read(i);

    if (Magic != INITIALIZED_MAGIC)
        return false;

    // Read actual data
    bytePtr = ((byte*) this) + DATA_OFFSET ;
    for (size_t i = 0; i < _dataSize; i++)
        *bytePtr++ = EEPROM.read(i + sizeof(Magic));

    printData();

    return true;
}


void PersistentDataBase::printData()
{
    printHex((byte*) &Magic, sizeof(Magic));
    byte* dataPtr = ((byte*) this) + DATA_OFFSET;
    printHex(dataPtr, _dataSize);
}
