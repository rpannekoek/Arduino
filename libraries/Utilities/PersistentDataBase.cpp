#include "PersistentDataBase.h"
#include "Tracer.h"
#include <EEPROM.h>

#define INITIALIZED_MAGIC 0xCAFEBABE

// Constructor
PersistentDataBase::PersistentDataBase(size_t dataSize)
    : _dataSize(dataSize)
{
}


// Destructor
PersistentDataBase::~PersistentDataBase()
{
    EEPROM.end();
}


void PersistentDataBase::begin()
{
    Tracer tracer(F("PersistentDataBase::begin"));

    EEPROM.begin(512);

    if (readFromEEPROM())
    {
        validate();
        return;
    }

    TRACE(F("EEPROM not initialized; initializing PersistentData with defaults.\n"));
    initialize();
}


void PersistentDataBase::writeToEEPROM()
{
    Tracer tracer(F("PersistentDataBase::writeToEEPROM"));

    uint32_t magic = INITIALIZED_MAGIC;

    TRACE(F("Writing %u + %u bytes to EEPROM...\n"), _dataSize, sizeof(magic));
    printData();
 
    // Write magic
    uint8_t* bytePtr = (uint8_t*) &magic;
    for (size_t i = 0; i < sizeof(magic); i++)
        EEPROM.write(i, *bytePtr++);

    // Write actual data
    bytePtr = ((uint8_t*) &_dataSize) + sizeof(_dataSize);
    for (size_t i = 0; i < _dataSize; i++)
        EEPROM.write(i + sizeof(magic), *bytePtr++);
    EEPROM.commit();
}


bool PersistentDataBase::readFromEEPROM()
{
    Tracer tracer(F("PersistentDataBase::readFromEEPROM"));

    uint32_t magic;
    TRACE(F("Reading %u + %u bytes from EEPROM...\n"), _dataSize, sizeof(magic)); 

    // Read magic
    uint8_t* bytePtr = (uint8_t*) &magic;
    for (size_t i = 0; i < sizeof(magic); i++)
        *bytePtr++ = EEPROM.read(i);

    TRACE(F("Magic: %08X\n"), magic);
    if (magic != INITIALIZED_MAGIC)
        return false;

    // Read actual data
    bytePtr = ((uint8_t*) &_dataSize) + sizeof(_dataSize);
    for (size_t i = 0; i < _dataSize; i++)
        *bytePtr++ = EEPROM.read(i + sizeof(magic));

    printData();

    return true;
}


void PersistentDataBase::printData()
{
    uint8_t* dataPtr = ((uint8_t*) &_dataSize) + sizeof(_dataSize);
    Tracer::hexDump(dataPtr, _dataSize);
}
