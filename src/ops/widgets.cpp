#include "ops/widgets.hpp"

#include "util/jsonutil.hpp"
#include "version.hpp"

#include <cctype>
#include <sstream>

namespace srtgw
{
namespace
{
HttpResponse jsonAnswer(int status, std::string body)
{
    HttpResponse response;
    response.status = status;
    response.body = std::move(body);
    return response;
}

HttpResponse error(int status, std::string const& message)
{
    picojson::object obj;
    obj["error"] = picojson::value(message);
    return jsonAnswer(status, picojson::value(obj).serialize());
}

// One query parameter, percent-decoded ('+' is a space). Empty when it is missing.
std::string queryValue(std::string const& query, std::string const& name)
{
    std::istringstream items(query);
    std::string item;
    while (std::getline(items, item, '&'))
    {
        auto const eq = item.find('=');
        if (item.substr(0, eq) != name)
        {
            continue;
        }
        std::string out;
        auto const raw = eq == std::string::npos ? std::string() : item.substr(eq + 1);
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (raw[i] == '%' && i + 2 < raw.size() && std::isxdigit(static_cast<unsigned char>(raw[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(raw[i + 2])))
            {
                out.push_back(static_cast<char>(std::stoi(raw.substr(i + 1, 2), nullptr, 16)));
                i += 2;
            }
            else
            {
                out.push_back(raw[i] == '+' ? ' ' : raw[i]);
            }
        }
        return out;
    }
    return {};
}

// True when the WIDGET_FRAME_ANCESTORS source list names this Origin (or holds `*`).
bool listedOrigin(std::string const& sources, std::string origin)
{
    auto const normal = [](std::string text) {
        for (char& c : text)
        {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        while (!text.empty() && text.back() == '/')
        {
            text.pop_back();
        }
        return text;
    };
    origin = normal(origin);
    if (origin.empty() || origin == "null")
    {
        return false;
    }
    std::istringstream tokens(sources);
    std::string token;
    while (tokens >> token)
    {
        if (token == "*" || normal(token) == origin)
        {
            return true;
        }
    }
    return false;
}

std::string widgetsJson(Config const& config)
{
    picojson::array ids;
    for (auto const& channel : config.channels)
    {
        ids.emplace_back(channel.id);
    }
    picojson::object channel;
    channel["type"] = picojson::value("string");
    channel["title"] = picojson::value("Channel");
    channel["description"] = picojson::value("Channel id (ingest or egress)");
    channel["enum"] = picojson::value(ids);
    picojson::object properties;
    properties["channel"] = picojson::value(channel);
    picojson::object params;
    params["type"] = picojson::value("object");
    params["properties"] = picojson::value(properties);
    params["required"] = picojson::value(picojson::array{picojson::value("channel")});
    picojson::object size;
    size["w"] = picojson::value(360.0);
    size["h"] = picojson::value(200.0);
    picojson::object item;
    item["id"] = picojson::value("channel-status");
    item["title"] = picojson::value("SRT channel status");
    item["params"] = picojson::value(params);
    item["min_size"] = picojson::value(size);
    item["version"] = picojson::value(kVersion);
    return picojson::value(picojson::array{picojson::value(item)}).serialize();
}
} // namespace

bool isWidgetPath(std::string const& path)
{
    return path == "/widgets" || path.rfind("/widget/", 0) == 0;
}

HttpResponse widgetResponse(Config const& config, HttpRequest const& request, std::string const& origin, std::string const& page)
{
    if (request.method != "GET")
    {
        auto response = error(405, "widgets answer GET only");
        response.headers.emplace_back("Allow", "GET");
        return response;
    }
    if (request.path == "/widgets")
    {
        auto response = jsonAnswer(200, widgetsJson(config));
        if (listedOrigin(config.widgetFrameAncestors, origin))
        {
            response.headers.emplace_back("Access-Control-Allow-Origin", origin);
        }
        response.headers.emplace_back("Vary", "Origin");
        return response;
    }
    if (request.path != "/widget/channel-status")
    {
        return error(404, "widget not found");
    }
    auto const id = queryValue(request.query, "channel");
    bool known = false;
    for (auto const& channel : config.channels)
    {
        known = known || channel.id == id;
    }
    if (!known)
    {
        return error(400, "channel must be the id of a configured channel");
    }
    auto const theme = queryValue(request.query, "theme");
    if (!theme.empty() && theme != "dark" && theme != "light" && theme != "transparent")
    {
        return error(400, "theme must be dark, light or transparent");
    }
    // The page picks the widget from its URL. Only these routes may be framed.
    HttpResponse response;
    response.contentType = "text/html; charset=utf-8";
    response.body = page;
    response.headers.emplace_back("Content-Security-Policy", "frame-ancestors " + config.widgetFrameAncestors);
    return response;
}
} // namespace srtgw
