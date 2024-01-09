#include <PersistentDataBase.h>

struct PersistentSettings : WiFiSettingsWithFTP
{
    int ftpSyncEntries;
    char weatherApiKey[16];
    char weatherLocation[16];
    int maxTSet;
    int minTSet;
    char heatmonHost[32];
    bool usePumpModulation;
    int boilerOnDelay; // seconds

    PersistentSettings() : WiFiSettingsWithFTP(PSTR("OTGW"))
    {
        addIntegerField(ftpSyncEntries, PSTR("FTP sync entries"), 1, 250, 50);
        addStringField(heatmonHost, sizeof(heatmonHost), PSTR("Heatmon host"));
        addStringField(weatherApiKey, sizeof(weatherApiKey), PSTR("Weather API key"));
        addStringField(weatherLocation, sizeof(weatherLocation), PSTR("Weather location"));
        addIntegerField(maxTSet, PSTR("Max TSet"), 40, 80, 60);
        addIntegerField(minTSet, PSTR("Min TSet"), 20, 40, 40);
        addBooleanField(usePumpModulation, PSTR("Use pump modulation"), true, 4); // 4 bytes for alignment
        addTimeSpanField(boilerOnDelay, PSTR("Boiler on delay"), 0, 7200, 0);        
    }
};

PersistentSettings PersistentData;
