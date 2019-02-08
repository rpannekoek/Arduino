#include "WeatherAPI.h"
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <Tracer.h>

char _buffer[96];


WeatherAPI::WeatherAPI(int timeout)
    : _timeout(timeout)
{
}


int WeatherAPI::requestData(const char* apiKey, const char* location)
{
    Tracer tracer(F("WeatherAPI::requestData"));

    snprintf(
        _buffer,
        sizeof(_buffer),
        "http://weerlive.nl/api/json-data-10min.php?key=%s&locatie=%s",
        apiKey,
        location
        );
    TRACE(F("URL: %s\n"), _buffer);

    HTTPClient httpClient;
    httpClient.setTimeout(_timeout);
    if (!httpClient.begin(_buffer))
    {
        TRACE(F("Error parsing URL: %s\n"), _buffer);
        return WEATHER_ERROR_URL;
    }

    int result = httpClient.GET();
    TRACE(F("HTTPClient.GET() returned %d\n"), result);
    if (result != HTTP_CODE_OK)
    {
        httpClient.end();
        return result;
    }

    WiFiClient wifiClient = httpClient.getStream();

    size_t bytesRead;
    do
    {
        bytesRead = wifiClient.readBytesUntil(',',  _buffer, sizeof(_buffer));
        _buffer[bytesRead] = 0;
        TRACE(F("%s\n"), _buffer);

        const char* temp = strstr(_buffer, "\"temp\":");
        if (temp != NULL)
        {
            temp += 9; // "temp": "
            if (sscanf(temp, "%f", &temperature) == 1)
                TRACE(F("Temperature: %0.1f\n"), temperature);
            else
            {
                TRACE(F("Unable to parse temperature\n"));
                result = WEATHER_ERROR_TEMPERATURE;
            }
            break;
        }

    } while (bytesRead > 0);

    if (bytesRead == 0)
    {
        TRACE(F("Temperature not found\n"));
        result = WEATHER_ERROR_NO_TEMPERATURE;
    }

    httpClient.end();

    return result;
}
