#include <ESPFileSystem.h>
#include <Tracer.h>
#include "Navigation.h"

void Navigation::registerHttpHandlers(ESPWebServer& webServer)
{
    // Static files
    for (const char* fileName : files)
    {
        String path = F("/");
        path += fileName;
        webServer.serveStatic(path.c_str(), SPIFFS, path.c_str(), "max-age=86400, public");
    }

    // Dynamic web requests
    for (MenuItem& menuItem : menuItems)
    {
        String urlPath = F("/");
        urlPath += menuItem.urlPath;
        if (menuItem.postHandler == nullptr)
            webServer.on(urlPath, menuItem.handler);
        else
        {
            webServer.on(urlPath, HTTP_GET, menuItem.handler);
            webServer.on(urlPath, HTTP_POST, menuItem.postHandler);
        }
    }
}
