#include "ops/api.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <sstream>

namespace srtgw
{
namespace
{
HttpResponse text(int status, std::string body, std::string const& type = "application/json; charset=utf-8")
{
    HttpResponse response;
    response.status = status;
    response.contentType = type;
    response.body = std::move(body);
    return response;
}

std::vector<std::string> parts(std::string const& path)
{
    std::vector<std::string> out;
    std::stringstream stream(path);
    std::string item;
    while (std::getline(stream, item, '/'))
    {
        if (!item.empty())
        {
            out.push_back(item);
        }
    }
    return out;
}
} // namespace

Api::Api(ConfigStore& store, ChannelManager& channels, std::function<std::string()> nmosSummary)
    : store_(store)
    , channels_(channels)
    , nmosSummary_(std::move(nmosSummary))
{
    indexHtml_ = "<!doctype html><meta charset=utf-8><title>mxl-srt-gateway</title><body style=\"font-family:sans-serif;background:#111;color:#eee\"><h1>mxl-srt-gateway</h1><p>API is at <a href=\"/api/v1/status\">/api/v1/status</a>.</p></body>";
}

void Api::setIndex(std::string html)
{
    if (!html.empty())
    {
        indexHtml_ = std::move(html);
    }
}

HttpResponse Api::handle(HttpRequest const& request) const
{
    try
    {
        if (request.path == "/" || request.path == "/index.html")
        {
            return text(200, indexHtml_, "text/html; charset=utf-8");
        }
        if (request.path == "/livez")
        {
            return text(200, "ok\n", "text/plain");
        }
        if (request.path == "/readyz")
        {
            if (!channels_.ready())
            {
                return text(503, "{\"ready\":false}\n");
            }
            return text(200, "{\"ready\":true}\n");
        }
        if (request.path == "/statusz")
        {
            return text(200, channels_.statusJson());
        }
        if (request.path == "/metrics")
        {
            return text(200, renderMetrics(channels_.metrics()), "text/plain; version=0.0.4");
        }
        auto const path = parts(request.path);
        if (path.size() >= 2 && path[0] == "api" && path[1] == "v1")
        {
            if (path.size() == 3 && path[2] == "status" && request.method == "GET")
            {
                return text(200, channels_.statusJson());
            }
            if (path.size() == 3 && path[2] == "nmos" && request.method == "GET")
            {
                return text(200, nmosSummary_ ? nmosSummary_() : "{}");
            }
            if (path.size() == 3 && path[2] == "config")
            {
                if (request.method == "GET")
                {
                    return text(200, configToJson(store_.snapshot(), false));
                }
                if (request.method == "PUT")
                {
                    std::string error;
                    auto const root = json::parse(request.body, &error);
                    if (!error.empty() || !root.is<picojson::object>())
                    {
                        return text(400, "{\"error\":\"invalid json\"}");
                    }
                    std::map<std::string, std::string> values;
                    for (auto const& key : globalKeys())
                    {
                        if (json::has(root, key))
                        {
                            values[key] = json::fieldString(root, key, "");
                        }
                    }
                    store_.replaceGlobals(values);
                    if (values.count("LOG_LEVEL") != 0)
                    {
                        log::setLevel(values.at("LOG_LEVEL"));
                    }
                    return text(200, configToJson(store_.snapshot(), false));
                }
            }
            if (path.size() == 4 && path[2] == "config" && path[3] == "export" && request.method == "GET")
            {
                if (request.query.find("format=env") != std::string::npos)
                {
                    return text(200, configToEnv(store_.config()), "text/plain; charset=utf-8");
                }
                return text(200, configToJson(store_.snapshot(), false));
            }
            if (path.size() == 4 && path[2] == "config" && path[3] == "import" && request.method == "POST")
            {
                if (!request.body.empty() && request.body.front() != '{' && request.body.front() != '[')
                {
                    store_.importEnv(request.body);
                }
                else
                {
                    store_.importJson(request.body);
                }
                return text(200, configToJson(store_.snapshot(), false));
            }
            if (path.size() == 3 && path[2] == "channels" && request.method == "GET")
            {
                std::ostringstream out;
                out << "{\"channels\":[";
                bool first = true;
                for (auto const& channel : store_.config().channels)
                {
                    if (!first)
                    {
                        out << ',';
                    }
                    first = false;
                    out << channelToJson(channel, false);
                }
                out << "]}";
                return text(200, out.str());
            }
            if (path.size() == 3 && path[2] == "channels" && request.method == "POST")
            {
                auto const channel = store_.upsertChannel(request.body);
                return text(200, channelToJson(channel, false));
            }
            if (path.size() >= 4 && path[2] == "channels")
            {
                auto const& id = path[3];
                if (path.size() == 4 && request.method == "GET")
                {
                    for (auto const& channel : store_.config().channels)
                    {
                        if (channel.id == id)
                        {
                            return text(200, channelToJson(channel, false));
                        }
                    }
                    return text(404, "{\"error\":\"not found\"}");
                }
                if (path.size() == 4 && request.method == "PUT")
                {
                    std::string body = request.body;
                    std::string error;
                    auto root = json::parse(body, &error);
                    if (error.empty() && root.is<picojson::object>() && !json::has(root, "id"))
                    {
                        root.get<picojson::object>()["id"] = picojson::value(id);
                        body = root.serialize();
                    }
                    auto const channel = store_.upsertChannel(body);
                    return text(200, channelToJson(channel, false));
                }
                if (path.size() == 4 && request.method == "DELETE")
                {
                    if (!store_.removeChannel(id))
                    {
                        return text(404, "{\"error\":\"not found\"}");
                    }
                    return text(200, "{\"deleted\":true}");
                }
                if (path.size() == 5 && path[4] == "status" && request.method == "GET")
                {
                    return text(200, channels_.channelJson(id, true));
                }
                if (path.size() == 5 && path[4] == "stats" && request.method == "GET")
                {
                    return text(200, channels_.channelJson(id, true));
                }
                if (path.size() == 5 && path[4] == "thumbnail" && request.method == "GET")
                {
                    auto const jpeg = channels_.thumbnail(id);
                    if (jpeg.empty())
                    {
                        return text(404, "no thumbnail", "text/plain");
                    }
                    return text(200, std::string(reinterpret_cast<char const*>(jpeg.data()), jpeg.size()), "image/jpeg");
                }
                if (path.size() == 5 && path[4] == "matrix" && request.method == "PUT")
                {
                    ChannelConfig existing;
                    bool have = false;
                    for (auto const& channel : store_.config().channels)
                    {
                        if (channel.id == id)
                        {
                            existing = channel;
                            have = true;
                        }
                    }
                    if (!have)
                    {
                        return text(404, "{\"error\":\"not found\"}");
                    }
                    auto body = json::parse(request.body, nullptr);
                    std::string patch = std::string("{\"id\":\"") + id + "\"";
                    if (json::has(body, "preset") || json::has(body, "audio_preset"))
                    {
                        patch += ",\"audio_preset\":\"" + json::fieldString(body, "preset", json::fieldString(body, "audio_preset", "sequential")) + "\"";
                    }
                    if (json::has(body, "audio_outputs"))
                    {
                        patch += ",\"audio_outputs\":" + json::field(body, "audio_outputs")->serialize();
                    }
                    if (json::has(body, "egress"))
                    {
                        patch += ",\"egress\":" + json::field(body, "egress")->serialize();
                    }
                    else if (json::has(body, "audio_tracks"))
                    {
                        patch += ",\"egress\":{\"audio_tracks\":" + json::field(body, "audio_tracks")->serialize() + "}";
                    }
                    else if (json::has(body, "egress_preset"))
                    {
                        patch += ",\"egress\":{\"preset\":\"" + json::fieldString(body, "egress_preset", "8x-stereo-aac") + "\"}";
                    }
                    patch += "}";
                    auto const channel = store_.upsertChannel(patch);
                    (void)existing;
                    return text(200, channelToJson(channel, false));
                }
                if (path.size() == 5 && path[4] == "route" && request.method == "POST")
                {
                    auto const body = json::parse(request.body, nullptr);
                    FlowRoute video;
                    video.active = json::fieldBool(body, "video_active", true);
                    video.domainId = json::fieldString(body, "video_domain", "");
                    video.flowId = json::fieldString(body, "video_flow", "");
                    FlowRoute audio;
                    audio.active = json::fieldBool(body, "audio_active", video.active);
                    audio.domainId = json::fieldString(body, "audio_domain", video.domainId);
                    audio.flowId = json::fieldString(body, "audio_flow", "");
                    if (!video.flowId.empty())
                    {
                        channels_.setRoute(id, true, video);
                    }
                    if (!audio.flowId.empty())
                    {
                        channels_.setRoute(id, false, audio);
                    }
                    return text(200, channels_.channelJson(id, true));
                }
            }
        }
        return text(404, "{\"error\":\"not found\"}");
    }
    catch (ConfigError const& ex)
    {
        return text(400, std::string("{\"error\":\"") + log::jsonEscape(ex.what()) + "\"}");
    }
    catch (std::exception const& ex)
    {
        return text(500, std::string("{\"error\":\"") + log::jsonEscape(ex.what()) + "\"}");
    }
    (void)kVersion;
}
} // namespace srtgw
