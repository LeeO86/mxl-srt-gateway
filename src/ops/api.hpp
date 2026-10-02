#pragma once

#include "channel/runtime.hpp"
#include "ops/httpserver.hpp"

#include <functional>
#include <string>

namespace srtgw
{
class Api
{
public:
    Api(ConfigStore& store, ChannelManager& channels, std::function<std::string()> nmosSummary);

    void setIndex(std::string html);
    HttpResponse handle(HttpRequest const& request) const;

private:
    ConfigStore& store_;
    ChannelManager& channels_;
    std::function<std::string()> nmosSummary_;
    std::string indexHtml_;
};
} // namespace srtgw
