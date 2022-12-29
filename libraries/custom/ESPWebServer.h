#ifdef ESP8266
    #include <ESP8266WebServer.h>
    #define ESPWebServer ESP8266WebServer
#else
    #include <WebServer.h>
    #define ESPWebServer WebServer
#endif