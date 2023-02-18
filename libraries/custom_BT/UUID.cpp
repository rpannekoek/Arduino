#include <Tracer.h>
#include "UUID.h"


UUID128::UUID128()
{
    memset(data, 0, sizeof(data));
}


UUID128::UUID128(const UUID128& uuid)
{
    memcpy(data, uuid.data, sizeof(data));
}


UUID128::UUID128(const uuid128_t& uuid)
{
    memcpy(data, uuid, sizeof(data));
}


UUID128::UUID128(const String& uuid)
{
    if (uuid.length() != 36)
    {
        TRACE(F("Invalid UUID: '%s'\n"), uuid.c_str());
        return;
    }

    int k = 0;
    for (int i = 0; i < sizeof(data); i++)
    {
        String hexByte = uuid.substring(k, k + 2);
        data[i] = static_cast<uint8_t>(strtol(hexByte.c_str(), nullptr, 16));
        TRACE(F("%d = '%s' -> %02x\n"), k, hexByte.c_str(), data[i]);
        k += 2;
        if (i == 3 || i == 5 || i == 7 || i == 9) k++;
    }
}


String UUID128::toString() const
{
    String result;
    for (int i = 0; i < sizeof(data); i++)
    {
        char hexByte[4];
        snprintf(hexByte, sizeof(hexByte), "%02X", static_cast<int>(data[i]));
        result += hexByte;
        if (i == 3 || i == 5 || i == 7 || i == 9) result += "-";
    }
    return result;
}


bool UUID128::equals(const UUID128& other) const
{
    return memcmp(data, other.data, sizeof(data)) == 0;
}


bool UUID128::equals(const uuid128_t& other) const
{
    return memcmp(data, other, sizeof(data)) == 0;
}
