#include <Tracer.h>
#include "Navigation.h"


void Navigation::registerHttpHandlers(ESPWebServer& webServer)
{
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
