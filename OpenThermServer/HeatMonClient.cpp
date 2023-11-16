#include "HeatMonClient.h"
#include <Tracer.h>

// Constructor
HeatMonClient::HeatMonClient(uint16_t timeout)
    : RESTClient(timeout, new DynamicJsonDocument(256))
{
}


bool HeatMonClient::begin(const char* host)
{
    Tracer tracer(F("HeatMonClient::begin"), host);

    String url = F("http://");
    url += host;
    url += F("/json");

    return RESTClient::begin(url);
}


bool HeatMonClient::parseResponse(const JsonDocument& response)
{
    tIn = response["Tin"];
    tOut = response["Tout"];
    tBuffer = response["Tbuffer"];
    flowRate = response["Flow"];
    pIn = response["Pin"];
    valve = response["Valve"];

    TRACE(F("tIn: %0.1f, tOut: %0.1f, tBuffer: %0.1f\n"), tIn, tOut, tBuffer);
    return true;
}
