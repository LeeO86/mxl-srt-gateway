#pragma once

#include "config/store.hpp"
#include "media/engine.hpp"
#include "nmos/ids.hpp"
#include "ops/metrics.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace srtgw
{
class ChannelManager
{
public:
    explicit ChannelManager(ConfigStore& store);
    ~ChannelManager();

    void start();
    void stop();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::string statusJson() const;
    [[nodiscard]] std::string channelJson(std::string const& id, bool found) const;
    [[nodiscard]] std::vector<std::uint8_t> thumbnail(std::string const& id) const;
    [[nodiscard]] ProcessMetrics metrics() const;
    void setRoute(std::string const& id, bool video, FlowRoute route);
    [[nodiscard]] FlowRoute route(std::string const& id, bool video) const;
    [[nodiscard]] std::shared_ptr<MxlDomain> outputDomain() const;
    [[nodiscard]] NmosIds ids() const;

private:
    void supervise();
    std::shared_ptr<MxlDomain> openInput(std::string const& domainId, bool* mirror);
    void launch(ChannelConfig const& channel);
    void loadRoutes();
    void saveRoutes() const;

    ConfigStore& store_;
    std::shared_ptr<MxlDomain> output_;
    NmosIds ids_;
    std::atomic<bool> stop_{true};
    std::atomic<bool> ready_{false};
    std::thread supervisor_;
    mutable std::mutex mu_;
    struct Slot
    {
        std::string signature;
        std::unique_ptr<IngestPipeline> ingest;
        std::unique_ptr<EgressPipeline> egress;
    };
    std::map<std::string, Slot> slots_;
    std::map<std::string, FlowRoute> videoRoutes_;
    std::map<std::string, FlowRoute> audioRoutes_;
    std::map<std::string, std::shared_ptr<MxlDomain>> inputs_;
};
} // namespace srtgw
