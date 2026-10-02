#pragma once

#include "channel/runtime.hpp"

#include <memory>
#include <string>

namespace srtgw
{
class NmosNode
{
public:
    NmosNode(ConfigStore& store, ChannelManager& channels);
    ~NmosNode();

    void start();
    void stop();
    [[nodiscard]] std::string summary() const;
    [[nodiscard]] bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace srtgw
