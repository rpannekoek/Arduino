#ifndef WEATHER_API_H
#define WEATHER_API_H

#include <WiFiClient.h>


#define WEATHER_ERROR_HTTP_REQUEST -1
#define WEATHER_ERROR_HTTP_RESPONSE -2
#define WEATHER_ERROR_TEMPERATURE -3
#define WEATHER_ERROR_NO_TEMPERATURE -4
#define WEATHER_ERROR_TIMEOUT -5


class WeatherAPI
{
    public:
        float temperature;

        WeatherAPI(int timeout);

        bool beginRequestData(const char* apiKey, const char* location);
        int endRequestData();
        int requestData(const char* apiKey, const char* location);
        void close();
    
    protected:
        int _timeout;
        WiFiClient _wifiClient;
        char _buffer[128];
};

#endif