#include <ESPFileSystem.h>
#include <Tracer.h>
#include "Navigation.h"

void Navigation::registerHttpHandlers(ESPWebServer& webServer)
{
    // Static files
    for (PGM_P fileName : files)
    {
        String path = F("/");
        path += FPSTR(fileName);
        webServer.serveStatic(path.c_str(), SPIFFS, path.c_str(), "max-age=86400, public");
    }

    // Dynamic web requests
    for (MenuItem& menuItem : menuItems)
    {
        String urlPath = F("/");
        if (menuItem.urlPath != nullptr)
            urlPath += FPSTR(menuItem.urlPath);
        if (menuItem.postHandler == nullptr)
            webServer.on(urlPath, menuItem.handler);
        else
        {
            webServer.on(urlPath, HTTP_GET, menuItem.handler);
            webServer.on(urlPath, HTTP_POST, menuItem.postHandler);
        }
    }
}
