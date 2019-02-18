#include "PersistentDataBase.h"
#include "PrintHex.h"
#include "Tracer.h"
#include <EEPROM.h>

#define INITIALIZED_MAGIC 0xCAFEBABE
#define DATA_OFFSET sizeof(_magic) + 4

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


void PersistentDataBase::begin()
{
    Tracer tracer(F("PersistentDataBase::begin"));

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

    TRACE(F("Writing %d + %d bytes to EEPROM...\n"), _dataSize, sizeof(_magic));
    printData();
 
    // Write magic
    _magic = INITIALIZED_MAGIC;
    byte* bytePtr = (byte*) &_magic;
    for (size_t i = 0; i < sizeof(_magic); i++)
        EEPROM.write(i, *bytePtr++);

    // Write actual data
    bytePtr = ((byte*) this) + DATA_OFFSET ;
    for (size_t i = 0; i < _dataSize; i++)
        EEPROM.write(i + sizeof(_magic), *bytePtr++);
    EEPROM.commit();
}


bool PersistentDataBase::readFromEEPROM()
{
    Tracer tracer(F("PersistentDataBase::readFromEEPROM"));

    TRACE(F("Reading %d + %d bytes from EEPROM...\n"), _dataSize, sizeof(_magic)); 

    // Read magic
    byte* bytePtr = (byte*) &_magic;
    for (size_t i = 0; i < sizeof(_magic); i++)
        *bytePtr++ = EEPROM.read(i);

    if (_magic != INITIALIZED_MAGIC)
        return false;

    // Read actual data
    bytePtr = ((byte*) this) + DATA_OFFSET ;
    for (size_t i = 0; i < _dataSize; i++)
        *bytePtr++ = EEPROM.read(i + sizeof(_magic));

    printData();

    return true;
}


void PersistentDataBase::printData()
{
    TRACE(F("Magic: 0x%08X\n"), _magic);
    byte* dataPtr = ((byte*) this) + DATA_OFFSET;
    printHex(dataPtr, _dataSize);
}
