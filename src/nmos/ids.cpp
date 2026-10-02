#include "nmos/ids.hpp"

#include "util/uuid.hpp"

namespace srtgw
{
namespace
{
std::string child(std::string const& node, std::string const& name)
{
    return uuidV5(kUuidNamespaceUrl, node + "/" + name);
}
} // namespace

std::string NmosIds::videoSource(ChannelConfig const& channel) const
{
    return child(node, "ch/" + channel.id + "/video/source");
}

std::string NmosIds::videoFlow(ChannelConfig const& channel) const
{
    return child(node, "ch/" + channel.id + "/video/flow/" + channel.target.signature());
}

std::string NmosIds::videoSender(ChannelConfig const& channel) const
{
    return child(node, "ch/" + channel.id + "/video/sender");
}

std::string NmosIds::audioSource(ChannelConfig const& channel, int index) const
{
    return child(node, "ch/" + channel.id + "/audio/" + std::to_string(index) + "/source");
}

std::string NmosIds::audioFlow(ChannelConfig const& channel, int index) const
{
    int const channels = index < static_cast<int>(channel.audioOutputs.size()) ? channel.audioOutputs[static_cast<std::size_t>(index)].channels : 16;
    return child(node, "ch/" + channel.id + "/audio/" + std::to_string(index) + "/flow/" + std::to_string(channels));
}

std::string NmosIds::audioSender(ChannelConfig const& channel, int index) const
{
    return child(node, "ch/" + channel.id + "/audio/" + std::to_string(index) + "/sender");
}

std::string NmosIds::videoReceiver(ChannelConfig const& channel) const
{
    return child(node, "ch/" + channel.id + "/video/receiver");
}

std::string NmosIds::audioReceiver(ChannelConfig const& channel) const
{
    return child(node, "ch/" + channel.id + "/audio/receiver");
}

NmosIds makeNmosIds(std::string const& seed)
{
    NmosIds ids;
    ids.node = uuidV5(kUuidNamespaceUrl, "mxl-srt-gateway/" + seed + "/node");
    ids.device = uuidV5(kUuidNamespaceUrl, "mxl-srt-gateway/" + seed + "/device");
    ids.domain = uuidV5(kUuidNamespaceUrl, seed + "/domain");
    return ids;
}
} // namespace srtgw
