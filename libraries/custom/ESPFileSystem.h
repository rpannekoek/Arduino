#ifdef ESP8266
    #include <FS.h>
    #define START_SPIFFS SPIFFS.begin()
#else
    #include <SPIFFS.h>
    #define START_SPIFFS SPIFFS.begin(false, "")
#endif