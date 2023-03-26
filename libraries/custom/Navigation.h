#ifndef NAVIGATION_H
#define NAVIGATION_H

#include <ESPWebServer.h>
#include <vector>

struct MenuItem
{
    PGM_P icon;
    PGM_P label;
    PGM_P urlPath;
    std::function<void(void)> handler;
    std::function<void(void)> postHandler;
};

struct Navigation
{
    String width;
    std::vector<PGM_P> files;
    std::vector<MenuItem> menuItems;

    void registerHttpHandlers(ESPWebServer& webServer);
};

#endif