#pragma once

#include "config/config.hpp"
#include "ops/httpserver.hpp"

#include <string>

namespace srtgw
{
// True for /widgets and /widget/<id>.
bool isWidgetPath(std::string const& path);

// GET /widgets (the list, CORS only for the WIDGET_FRAME_ANCESTORS origins) and
// GET /widget/<id>?<params> (`page`, framed only by WIDGET_FRAME_ANCESTORS). SPECIFICATION.md §7.1.
HttpResponse widgetResponse(Config const& config, HttpRequest const& request, std::string const& origin, std::string const& page);
} // namespace srtgw
