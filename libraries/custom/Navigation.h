#ifndef NAVIGATION_H
#define NAVIGATION_H

#include <ESPWebServer.h>
#include <vector>

struct MenuItem
{
    String icon;
    String label;
    String urlPath;
    std::function<void(void)> handler;
    std::function<void(void)> postHandler;
};

struct Navigation
{
    String width;
    std::vector<const char*> files;
    std::vector<MenuItem> menuItems;

    void registerHttpHandlers(ESPWebServer& webServer);
};

#endif