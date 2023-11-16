#include "WeatherAPI.h"
#include <WiFiClient.h>
#include <Tracer.h>

// Constructor
WeatherAPI::WeatherAPI(uint16_t timeout)
    : RESTClient(timeout, new DynamicJsonDocument(128))
{
    isInitialized = false;
}


bool WeatherAPI::begin(const char* apiKey, const char* location)
{
    String url = F("http://weerlive.nl/api/json-data-10min.php?key=");
    url += apiKey;
    url += F("&locatie=");
    url += location;

    isInitialized = true;
    return RESTClient::begin(url);
}


DeserializationError WeatherAPI::parseJson(const String& json)
{
    StaticJsonDocument<64> filterDoc;
    filterDoc["liveweer"][0]["temp"] = true;
    return deserializeJson(_responseDoc, json, DeserializationOption::Filter(filterDoc));
}


bool WeatherAPI::parseResponse(const JsonDocument& response)
{
    temperature = response["liveweer"][0]["temp"];
    TRACE(F("\ntemperature: %0.1f\n"), temperature);
    return true;
}