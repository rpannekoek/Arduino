#include "OTGWClient.h"
#include <Tracer.h>

// Constructor
OTGWClient::OTGWClient(uint16_t timeout)
    : RESTClient(timeout, new DynamicJsonDocument(64))
{
}


bool OTGWClient::begin(const char* host)
{
    Tracer tracer(F("OTGWClient::begin"), host);

    String baseUrl = F("http://");
    baseUrl += host;

    return RESTClient::begin(baseUrl);
}


int OTGWClient::setPump(bool on, const String& reason)
{
    Tracer tracer(F("OTGWClient::setPump"));

    _urlSuffix = F("/pump?");
    _urlSuffix += on ? F("on") : F("off");
    if (reason.length() != 0)
    {
        String urlEncodedReason = reason;
        urlEncodedReason.replace(F(" "), F("+"));
        _urlSuffix += F("&reason=");
        _urlSuffix += urlEncodedReason;
    }

    return requestData(_urlSuffix);
}


int OTGWClient::retry()
{
    return requestData(_urlSuffix);
}


bool OTGWClient::parseResponse(const JsonDocument& response)
{
    boilerLevel = response.as<String>();
    return true;
}
