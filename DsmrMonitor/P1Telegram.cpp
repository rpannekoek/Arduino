#include "P1Telegram.h"
#include <Tracer.h>

// Constructor
P1Telegram::P1Telegram()
{
    addProperty(PropertyId::PowerDeliveredTotal, F("1-0:1.7.0"), F("Total delivered power"));
    addProperty(PropertyId::PowerReturnedTotal, F("1-0:2.7.0"), F("Total returned power"));
    addProperty(PropertyId::VoltageL1, F("1-0:32.7.0"), F("Voltage L1"));
    addProperty(PropertyId::VoltageL2, F("1-0:52.7.0"), F("Voltage L2"));
    addProperty(PropertyId::VoltageL3, F("1-0:72.7.0"), F("Voltage L3"));
    addProperty(PropertyId::CurrentL1, F("1-0:31.7.0"), F("Current L1"));
    addProperty(PropertyId::CurrentL2, F("1-0:51.7.0"), F("Current L2"));
    addProperty(PropertyId::CurrentL3, F("1-0:71.7.0"), F("Current L3"));
    addProperty(PropertyId::PowerDeliveredL1, F("1-0:21.7.0"), F("Power delivered L1"));
    addProperty(PropertyId::PowerDeliveredL2, F("1-0:41.7.0"), F("Power delivered L2"));
    addProperty(PropertyId::PowerDeliveredL3, F("1-0:61.7.0"), F("Power delivered L3"));
    addProperty(PropertyId::PowerReturnedL1, F("1-0:22.7.0"), F("Power returned L1"));
    addProperty(PropertyId::PowerReturnedL2, F("1-0:42.7.0"), F("Power returned L2"));
    addProperty(PropertyId::PowerReturnedL3, F("1-0:62.7.0"), F("Power returned L3"));
    addProperty(PropertyId::Gas, F("0-1:24.2.1"), F("Gas"));

    _numDataLines = 0;
}

static float _testGasKWh = 0;

void P1Telegram::populateTestData()
{
    char gasDataLine[64];
    snprintf(gasDataLine, sizeof(gasDataLine), "0-1:24.2.1(201205%dW)(%0.3f*m3)\r\n", millis(), _testGasKWh);
    _testGasKWh += 0.123;

    _dataLines[0] = F("1-0:32.7.0(233.1*V)\r\n");
    _dataLines[1] = F("1-0:31.7.0(025*A)\r\n");
    _dataLines[2] = F("1-0:21.7.0(05.828*kW)\r\n");
    _dataLines[3] = F("1-0:22.7.0(01.234*kW)\r\n");
    _dataLines[4] = F("1-0:52.7.0(232.6*V)\r\n");
    _dataLines[5] = F("1-0:51.7.0(015*A)\r\n");
    _dataLines[6] = F("1-0:41.7.0(03.489*kW)\r\n");
    _dataLines[7] = F("1-0:42.7.0(00.001*kW)\r\n");
    _dataLines[8] = gasDataLine;
    _numDataLines = 9;
}

void P1Telegram::addProperty(PropertyId propertyId, String obisId, String label)
{
    int propertyIndex = static_cast<int>(propertyId);
    _obisIds[propertyIndex] = obisId;
    _labels[propertyIndex] = label;
}

const char* P1Telegram::readDataLine(Stream& stream)
{
    static char dataLine[64];
    size_t bytesRead = stream.readBytesUntil('\n', dataLine, sizeof(dataLine) - 1);
    dataLine[bytesRead] = 0;
    return dataLine;
}


String P1Telegram::readFrom(Stream& stream)
{
    Tracer tracer(F("P1Telegram::readFrom"));

    _numDataLines = 0;

    // Find telegram header
    const char* dataLine;
    do
    {
        dataLine = readDataLine(stream);
        if (dataLine[0] == 0)
        {
            return F("ERROR: No P1 header found.");
        }
    } while (dataLine[0] != '/');
    
    if (strncmp(dataLine, "/test", 5) == 0)
    {
        populateTestData();
        return String(dataLine);
    }

    do
    {
        dataLine = readDataLine(stream);
        if (dataLine[0] == 0)
        {
            return F("ERROR: P1 Timeout.");
        }

        if (dataLine[0] == '!')
        {
            // TODO: verify CRC
            return String();
        }

        if (strlen(dataLine) > 2)
        {
            _dataLines[_numDataLines++] = dataLine;
        }

    } while (_numDataLines < MAX_DATA_LINES);
    
    return F("ERROR: Too many data lines received.");
}

String P1Telegram::getPropertyValue(PropertyId id, String* timestampPtr)
{
    String obisId = _obisIds[static_cast<int>(id)];
    String label = _labels[static_cast<int>(id)];

    for (int i = 0; i < _numDataLines; i++)
    {
        if (_dataLines[i].startsWith(obisId))
        {
            String dataLine = _dataLines[i];

            int valueStartIndex = dataLine.indexOf('(');
            if (valueStartIndex < 0)
            {
                TRACE(F("ERROR: No value start marker: %s"), dataLine.c_str());
                break;
            }

            if (timestampPtr != nullptr)
            {
                int timestampEndIndex = dataLine.indexOf(')', valueStartIndex);
                if (timestampEndIndex < 0)
                {
                    TRACE(F("ERROR: No timestamp end marker: %s\n"), dataLine.c_str());
                    break;
                }
                *timestampPtr = dataLine.substring(valueStartIndex + 1, timestampEndIndex);
                valueStartIndex = timestampEndIndex + 1;
            }

            int valueEndIndex = dataLine.indexOf('*', valueStartIndex);
            if (valueEndIndex < 0)
            {
                valueEndIndex = dataLine.indexOf(')', valueStartIndex);
                if (valueEndIndex < 0)
                {
                    TRACE(F("ERROR: No value end marker: %s\n"), dataLine.c_str());
                    break;
                }
            }

            String value = dataLine.substring(valueStartIndex + 1, valueEndIndex);
            TRACE(F("'%s' = '%s'\n"), label.c_str() ,value.c_str()); 
            return value;
        }
    }

    TRACE(F("ERROR: No value found for '%s' (%s)\n"), label.c_str(), obisId.c_str());
    return String();
}

float P1Telegram::getFloatValue(PropertyId id, String* timestampPtr)
{
    String propertyValue = getPropertyValue(id, timestampPtr);
    return (propertyValue.length() == 0) ? 0.0 : propertyValue.toFloat(); 
}
