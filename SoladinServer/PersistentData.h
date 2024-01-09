#include <PersistentDataBase.h>

struct Settings : public WiFiSettingsWithFTP
{
    Settings() : WiFiSettingsWithFTP(PSTR("SoladinServer"))
    {
    }
};

Settings PersistentData;
