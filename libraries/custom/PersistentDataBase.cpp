#include "PersistentDataBase.h"
#include "Tracer.h"
#include <EEPROM.h>

constexpr uint32_t INITIALIZED_MAGIC = 0xCAFEBABE;

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


void PersistentDataBase::addField(PersistentDataField* fieldPtr, size_t dataSize)
{
    _fields.push_back(fieldPtr);
    _dataSize += dataSize;
}


void PersistentDataBase::addStringField(char* value, size_t size, PGM_P label, PGM_P defaultValue)
{
    PersistentStringField* fieldPtr = new PersistentStringField;
    fieldPtr->value = value;
    fieldPtr->size = size;
    fieldPtr->label = label;
    fieldPtr->defaultValue = defaultValue;
    addField(fieldPtr, size);
}


void PersistentDataBase::addPasswordField(char* value, size_t size, PGM_P label)
{
    PersistentPasswordField* fieldPtr = new PersistentPasswordField;
    fieldPtr->value = value;
    fieldPtr->size = size;
    fieldPtr->label = label;
    fieldPtr->defaultValue = nullptr;
    addField(fieldPtr, size);
}


void PersistentDataBase::addIntegerField(int& value, PGM_P label, int minValue, int maxValue, int defaultValue)
{
    PersistentIntegerField* fieldPtr = new PersistentIntegerField;
    fieldPtr->valuePtr = &value;
    fieldPtr->label = label;
    fieldPtr->minValue = minValue;
    fieldPtr->maxValue = maxValue;
    fieldPtr->defaultValue = defaultValue;
    addField(fieldPtr, sizeof(int));
}


void PersistentDataBase::addTimeSpanField(int& value, PGM_P label, int minValue, int maxValue, int defaultValue)
{
    PersistentTimeSpanField* fieldPtr = new PersistentTimeSpanField;
    fieldPtr->valuePtr = &value;
    fieldPtr->label = label;
    fieldPtr->minValue = minValue;
    fieldPtr->maxValue = maxValue;
    fieldPtr->defaultValue = defaultValue;
    addField(fieldPtr, sizeof(int));
}


void PersistentDataBase::addFloatField(float& value, PGM_P label, int decimals, float minValue, float maxValue, float defaultValue)
{
    PersistentFloatField* fieldPtr = new PersistentFloatField;
    fieldPtr->valuePtr = &value;
    fieldPtr->label = label;
    fieldPtr->decimals = decimals;
    fieldPtr->minValue = minValue;
    fieldPtr->maxValue = maxValue;
    fieldPtr->defaultValue = defaultValue;
    addField(fieldPtr, sizeof(float));
}


void PersistentDataBase::addBooleanField(bool& value, PGM_P label, bool defaultValue, size_t dataSize)
{
    PersistentBooleanField* fieldPtr = new PersistentBooleanField;
    fieldPtr->valuePtr = &value;
    fieldPtr->label = label;
    fieldPtr->defaultValue = defaultValue;
    addField(fieldPtr, dataSize);
}


void PersistentDataBase::initialize()
{
    for (PersistentDataField* fieldPtr : _fields)
        fieldPtr->initialize();
}


void PersistentDataBase::validate()
{
    for (PersistentDataField* fieldPtr : _fields)
        fieldPtr->validate();
}


void PersistentDataBase::writeHtmlForm(HtmlWriter& html)
{
    Tracer tracer(F("PersistentDataBase::writeHtmlForm"));

    int i = 1;
    for (PersistentDataField* fieldPtr : _fields)
    {
        String fieldId = "f";
        fieldId += i++;
        fieldPtr->writeHtml(html, fieldId);
    }
}

void PersistentDataBase::parseHtmlFormData(std::function<String(const String&)> formDataById)
{
    Tracer tracer(F("PersistentDataBase::parseHtmlFormData"));

    int i = 1;
    for (PersistentDataField* fieldPtr : _fields)
    {
        String fieldLabel = FPSTR(fieldPtr->label);
        String fieldId = "f";
        fieldId += i++;
        String fieldValue = formDataById(fieldId);
        TRACE(F("'%s' = '%s'\n"), fieldLabel.c_str(), fieldValue.c_str());
        fieldPtr->parse(fieldValue);
    }
}


void PersistentStringField::initialize()
{
    if (defaultValue == nullptr)
        value[0] = 0;
    else
        strncpy_P(value, defaultValue, size);
}


void PersistentStringField::validate()
{
    // Ensure the string is null-terminated
    value[size - 1] = 0;
}


void PersistentStringField::writeHtml(HtmlWriter& html, const String& id)
{
    html.writeTextBox(id, label, value, size - 1);
}


void PersistentStringField::parse(const String& str)
{
    strncpy(value, str.c_str(), size);
    value[size - 1] = 0;
}


void PersistentPasswordField::writeHtml(HtmlWriter& html, const String& id)
{
    html.writeTextBox(id, label, value, size - 1, F("password"));
}


void PersistentIntegerField::initialize()
{
    *valuePtr = defaultValue;
}


void PersistentIntegerField::validate()
{
    *valuePtr = std::min(std::max(*valuePtr, minValue), maxValue);
}


void PersistentIntegerField::writeHtml(HtmlWriter& html, const String& id)
{
    html.writeNumberBox(id, label, *valuePtr, minValue, maxValue);
}


void PersistentIntegerField::parse(const String& str)
{
    *valuePtr = str.toInt();
}


void PersistentTimeSpanField::writeHtml(HtmlWriter& html, const String& id)
{
    int seconds = *valuePtr;
    char timeSpan[16];
    snprintf(
        timeSpan,
        sizeof(timeSpan),
        "%02d:%02d:%02d",
        seconds / 3600,
        (seconds / 60) % 60,
        seconds % 60);
    html.writeTextBox(id, label, timeSpan, 8);
}


void PersistentTimeSpanField::parse(const String& str)
{
    tm time;
    strptime(str.c_str(), "%H:%M:%S", &time);
    int seconds = time.tm_hour * 3600 + time.tm_min * 60 + time.tm_sec;
    *valuePtr = seconds;
}


void PersistentFloatField::initialize()
{
    *valuePtr = defaultValue;
}


void PersistentFloatField::validate()
{
    *valuePtr = std::min(std::max(*valuePtr, minValue), maxValue);
}


void PersistentFloatField::writeHtml(HtmlWriter& html, const String& id)
{
    html.writeNumberBox(id, label, *valuePtr, minValue, maxValue, decimals);
}


void PersistentFloatField::parse(const String& str)
{
    *valuePtr = str.toFloat();
}


void PersistentBooleanField::initialize()
{
    *valuePtr = defaultValue;
}


void PersistentBooleanField::validate()
{
    // Nothing to do
}


void PersistentBooleanField::writeHtml(HtmlWriter& html, const String& id)
{
    html.writeCheckbox(id, label, *valuePtr);
}


void PersistentBooleanField::parse(const String& str)
{
    *valuePtr = str.length() > 0;
}