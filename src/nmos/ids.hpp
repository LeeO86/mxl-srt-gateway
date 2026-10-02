#pragma once

#include "config/config.hpp"

#include <string>

namespace srtgw
{
struct NmosIds
{
    std::string node;
    std::string device;
    std::string domain;
    std::string videoSource(ChannelConfig const& channel) const;
    std::string videoFlow(ChannelConfig const& channel) const;
    std::string videoSender(ChannelConfig const& channel) const;
    std::string audioSource(ChannelConfig const& channel, int index) const;
    std::string audioFlow(ChannelConfig const& channel, int index) const;
    std::string audioSender(ChannelConfig const& channel, int index) const;
    std::string videoReceiver(ChannelConfig const& channel) const;
    std::string audioReceiver(ChannelConfig const& channel) const;
};

NmosIds makeNmosIds(std::string const& seed);
} // namespace srtgw
