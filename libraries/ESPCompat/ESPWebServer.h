#ifdef ESP8266
    #include <ESP8266WebServer.h>
    #define WebServer ESP8266WebServer
#else
    #include <WebServer.h>
#endif