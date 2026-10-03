#include "nmos/node.hpp"

#include "util/jsonutil.hpp"
#include "util/logging.hpp"
#include "util/net.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#ifndef SRTGW_WITH_NMOS
#define SRTGW_WITH_NMOS 0
#endif

#if SRTGW_WITH_NMOS
#include <fstream>
#include <iostream>
#include <limits>

#include "cpprest/host_utils.h"
#include "nmos/capabilities.h"
#include "nmos/channels.h"
#include "nmos/clock_name.h"
#include "nmos/colorspace.h"
#include "nmos/connection_api.h"
#include "nmos/connection_resources.h"
#include "nmos/format.h"
#include "nmos/group_hint.h"
#include "nmos/interlace_mode.h"
#include "nmos/log_gate.h"
#include "nmos/media_type.h"
#include "nmos/model.h"
#include "nmos/mxl.h"
#include "nmos/node_interfaces.h"
#include "nmos/node_resource.h"
#include "nmos/node_resources.h"
#include "nmos/node_server.h"
#include "nmos/resources.h"
#include "nmos/server.h"
#include "nmos/settings.h"
#include "nmos/transfer_characteristic.h"
#include "nmos/transport.h"
#include "slog/all_in_one.h"
#include "sdp/json.h"
#endif

namespace srtgw
{
struct NmosNode::Impl
{
    ConfigStore& store;
    Config config;
    ChannelManager& channels;
    NmosIds ids;
    std::atomic<bool> running{false};
    std::mutex mu;
    std::string error;
#if SRTGW_WITH_NMOS
    std::thread thread;
    nmos::node_model* model = nullptr;
#endif

    Impl(ConfigStore& storeIn, ChannelManager& manager)
        : store(storeIn)
        , config(storeIn.config())
        , channels(manager)
        , ids(manager.ids())
    {
    }
};

NmosNode::NmosNode(ConfigStore& store, ChannelManager& channels)
    : impl_(std::make_unique<Impl>(store, channels))
{
}

NmosNode::~NmosNode()
{
    stop();
}

bool NmosNode::running() const
{
    return impl_->running.load();
}

bool NmosNode::registered() const
{
    auto const config = impl_->config;
    if (config.nmosRegistryAddress.empty())
    {
        return true;
    }
#if !SRTGW_WITH_NMOS
    return false;
#else
    auto const path = "/x-nmos/query/v1.3/nodes/" + impl_->ids.node;
    return httpGetStatus(config.nmosQueryAddress, config.nmosQueryPort, path, 800) == 200;
#endif
}

std::string NmosNode::summary() const
{
    auto const config = impl_->store.config();
    auto const ids = impl_->channels.ids();
    std::string out = std::string("{\"enabled\":true,\"built\":") + (SRTGW_WITH_NMOS ? "true" : "false") + ",\"running\":" +
                      (impl_->running.load() ? "true" : "false") + ",\"node_id\":\"" + ids.node + "\",\"device_id\":\"" + ids.device +
                      "\",\"device_label\":\"MXL SRT Gateway\",\"domain_id\":\"" + config.mxlOutputDomainId + "\",\"registry\":\"" + config.nmosRegistryAddress +
                      "\",\"registry_port\":" + std::to_string(config.nmosRegistryPort) + ",\"port\":" + std::to_string(config.nmosPort) + ",\"dns_sd\":" +
                      (config.nmosDnsSd ? "true" : "false") + ",\"resources\":[";
    bool first = true;
    for (auto const& channel : config.channels)
    {
        auto add = [&](std::string const& kind, std::string const& id, std::string const& label) {
            if (!first)
            {
                out += ',';
            }
            first = false;
            out += "{\"kind\":\"" + kind + "\",\"id\":\"" + id + "\",\"label\":\"" + log::jsonEscape(label) + "\",\"channel\":\"" + channel.id + "\"}";
        };
        if (channel.direction == "ingest")
        {
            add("sender", ids.videoSender(channel), channel.label + " video");
            add("flow", ids.videoFlow(channel), channel.label + " video flow");
            for (std::size_t i = 0; i < channel.audioOutputs.size(); ++i)
            {
                add("sender", ids.audioSender(channel, static_cast<int>(i)), channel.label + " audio " + std::to_string(i + 1));
            }
        }
        else
        {
            add("receiver", ids.videoReceiver(channel), channel.label + " video");
            add("receiver", ids.audioReceiver(channel), channel.label + " audio");
        }
    }
    out += "],\"error\":\"" + log::jsonEscape(impl_->error) + "\"}";
    return out;
}

#if !SRTGW_WITH_NMOS
void NmosNode::start()
{
    impl_->running.store(false);
    log::warn("nmos_not_linked", {{"detail", "built without nmos-cpp; IS-04/IS-05 node is inactive"}});
}

void NmosNode::stop() {}
#else
namespace
{
utility::string_t us(std::string const& text)
{
    return utility::conversions::to_string_t(text);
}

std::string su(utility::string_t const& text)
{
    return utility::conversions::to_utf8string(text);
}

void tagGroup(nmos::resource& resource, std::string const& group, std::string const& role)
{
    if (!resource.data.has_field(nmos::fields::tags))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    web::json::push_back(resource.data[U("tags")][U("urn:x-nmos:tag:grouphint/v1.0")], nmos::make_group_hint({us(group), us(role)}));
}

void applyPlatformTags(nmos::resource& resource, std::string const& tagsJson)
{
    if (tagsJson.empty())
    {
        return;
    }
    std::string error;
    auto const parsed = json::parse(tagsJson, &error);
    if (!error.empty() || !parsed.is<picojson::object>())
    {
        return;
    }
    if (!resource.data.has_field(nmos::fields::tags))
    {
        resource.data[U("tags")] = web::json::value::object();
    }
    for (auto const& item : parsed.get<picojson::object>())
    {
        if (!item.second.is<picojson::array>())
        {
            continue;
        }
        web::json::value values = web::json::value::array();
        for (auto const& entry : item.second.get<picojson::array>())
        {
            if (entry.is<std::string>())
            {
                web::json::push_back(values, web::json::value::string(us(entry.get<std::string>())));
            }
        }
        resource.data[U("tags")][us(item.first)] = values;
    }
}

} // namespace

void NmosNode::start()
{
    impl_->thread = std::thread([this] {
        nmos::experimental::log_model logModel;
        std::ostream errorLog(std::cerr.rdbuf());
        std::filebuf discarded;
        std::ostream accessLog(&discarded);
        nmos::experimental::log_gate gate(errorLog, accessLog, logModel);
        try
        {
            nmos::node_model nodeModel;
            impl_->model = &nodeModel;
            web::json::value settings = web::json::value::object();
            auto const nodeLabel = impl_->config.nmosLabel.empty() ? impl_->config.hostId : impl_->config.nmosLabel;
            settings[U("http_port")] = impl_->config.nmosPort;
            settings[U("label")] = web::json::value::string(us(nodeLabel));
            settings[U("description")] = web::json::value::string(U("mxl-srt-gateway"));
            settings[U("seed_id")] = web::json::value::string(us(impl_->ids.node));
            settings[U("service_name_prefix")] = web::json::value::string(U("mxl-srt-gateway"));
            settings[U("logging_level")] = 20;
            settings[U("control_protocol_ws_port")] = -1;
            settings[U("registration_request_max")] = 2;
            settings[U("registration_heartbeat_max")] = 2;
            auto const host = impl_->config.hostAddress.empty() ? primaryIpv4() : impl_->config.hostAddress;
            settings[U("host_address")] = web::json::value::string(us(host));
            auto hosts = web::json::value::array();
            hosts[0] = web::json::value::string(us(host));
            settings[U("host_addresses")] = hosts;
            if (!impl_->config.nmosDnsSd)
            {
                settings[U("pri")] = std::numeric_limits<int>::max();
                settings[U("highest_pri")] = std::numeric_limits<int>::max();
                settings[U("authorization_highest_pri")] = std::numeric_limits<int>::max();
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                settings[U("registry_address")] = web::json::value::string(us(impl_->config.nmosRegistryAddress));
                settings[U("registration_port")] = impl_->config.nmosRegistryPort;
                settings[U("query_port")] = impl_->config.nmosQueryPort;
            }
            nodeModel.settings = settings;
            nmos::insert_node_default_settings(nodeModel.settings);
            logModel.settings = nodeModel.settings;
            logModel.level = nmos::fields::logging_level(logModel.settings);

            auto implementation =
                nmos::experimental::node_implementation()
                    .on_parse_transport_file([](nmos::resource const&, nmos::resource const&, utility::string_t const&, utility::string_t const&,
                                                 slog::base_gate&) -> web::json::value { throw std::runtime_error("MXL does not use a transport file"); })
                    .on_resolve_auto([this](nmos::resource const&, nmos::resource const&, web::json::value& params) {
                        if (!params.is_array() || params.size() == 0)
                        {
                            return;
                        }
                        auto& leg = params.at(0);
                        nmos::details::resolve_auto(leg, U("mxl_domain_id"), [this] { return web::json::value::string(us(impl_->config.mxlOutputDomainId)); });
                        nmos::details::resolve_auto(leg, U("mxl_flow_id"), [] { return web::json::value::null(); });
                    })
                    .on_set_transportfile([](nmos::resource const&, nmos::resource const&, web::json::value& transportFile) { transportFile = web::json::value::null(); })
                    .on_connection_activated([this](nmos::resource const&, nmos::resource const& connection) {
                        if (!connection.data.has_field(U("active")))
                        {
                            return;
                        }
                        auto const id = su(connection.id);
                        auto const& active = connection.data.at(U("active"));
                        bool const enable = active.has_field(U("master_enable")) && active.at(U("master_enable")).is_boolean() && active.at(U("master_enable")).as_bool();
                        std::string domain;
                        std::string flow;
                        if (active.has_field(U("transport_params")) && active.at(U("transport_params")).is_array() && active.at(U("transport_params")).size() > 0)
                        {
                            auto const& params = active.at(U("transport_params")).at(0);
                            if (params.has_field(U("mxl_domain_id")) && params.at(U("mxl_domain_id")).is_string())
                            {
                                domain = su(params.at(U("mxl_domain_id")).as_string());
                            }
                            if (params.has_field(U("mxl_flow_id")) && params.at(U("mxl_flow_id")).is_string())
                            {
                                flow = su(params.at(U("mxl_flow_id")).as_string());
                            }
                        }
                        auto const cfg = impl_->store.config();
                        auto const ids = impl_->channels.ids();
                        for (auto const& channel : cfg.channels)
                        {
                            if (channel.direction != "egress")
                            {
                                continue;
                            }
                            FlowRoute route;
                            route.active = enable;
                            route.domainId = domain;
                            route.flowId = flow;
                            if (id == ids.videoReceiver(channel))
                            {
                                impl_->channels.setRoute(channel.id, true, route);
                            }
                            else if (id == ids.audioReceiver(channel))
                            {
                                impl_->channels.setRoute(channel.id, false, route);
                            }
                        }
                        log::info("nmos_activation", {{"id", id}, {"domain", domain}, {"flow", flow}, {"enable", enable ? "true" : "false"}});
                    });

            auto server = nmos::experimental::make_node_server(nodeModel, implementation, logModel, gate);
            server.thread_functions.push_back([this, &nodeModel] {
                auto lock = nodeModel.write_lock();
                auto const clocks = web::json::value_of({nmos::make_internal_clock(nmos::clock_names::clk0)});
                auto const interfaces = nmos::experimental::node_interfaces(nmos::get_host_interfaces(nodeModel.settings));
                auto const nodeLabel = impl_->config.nmosLabel.empty() ? impl_->config.hostId : impl_->config.nmosLabel;
                auto const deviceLabel = impl_->config.nmosLabel.empty() ? std::string("MXL SRT Gateway") : impl_->config.nmosLabel;
                auto node = nmos::make_node(us(impl_->ids.node), clocks, nmos::make_node_interfaces(interfaces), nodeModel.settings);
                node.data[U("label")] = web::json::value::string(us(nodeLabel));
                node.data[U("description")] = web::json::value::string(U("MXL SRT Gateway"));
                applyPlatformTags(node, impl_->config.nmosTagsJson);
                nmos::insert_resource(nodeModel.node_resources, std::move(node));
                auto device = nmos::make_device(us(impl_->ids.device), us(impl_->ids.node), {}, {}, nodeModel.settings);
                device.data[U("label")] = web::json::value::string(us(deviceLabel));
                applyPlatformTags(device, impl_->config.nmosTagsJson);
                nmos::insert_resource(nodeModel.node_resources, std::move(device));

                auto ensure = [&](nmos::resources& resources, nmos::resource&& resource) {
                    if (nmos::find_resource(resources, resource.id) == resources.end())
                    {
                        nmos::insert_resource(resources, std::move(resource));
                    }
                };
                auto sync = [&] {
                    auto const cfg = impl_->store.config();
                    auto const ids = impl_->channels.ids();
                    std::vector<nmos::id> senderIds;
                    std::vector<nmos::id> receiverIds;
                    for (auto const& channel : cfg.channels)
                    {
                        if (channel.direction == "ingest")
                        {
                            nmos::rational const rate{channel.target.rate.num, channel.target.rate.den};
                            auto const interlace = channel.target.interlaced
                                                        ? (channel.target.fieldOrder == "bff" ? nmos::interlace_modes::interlaced_bff : nmos::interlace_modes::interlaced_tff)
                                                        : nmos::interlace_modes::progressive;
                            auto source = nmos::make_video_source(us(ids.videoSource(channel)), us(ids.device), rate, nodeModel.settings);
                            source.data[U("label")] = web::json::value::string(us(channel.label + " video"));
                            tagGroup(source, channel.label, "Video");
                            ensure(nodeModel.node_resources, std::move(source));
                            auto flow = nmos::make_coded_video_flow(us(ids.videoFlow(channel)), us(ids.videoSource(channel)), us(ids.device), rate,
                                static_cast<unsigned>(channel.target.width), static_cast<unsigned>(channel.target.height), interlace, nmos::colorspaces::BT709,
                                nmos::transfer_characteristics::SDR, sdp::samplings::YCbCr_4_2_2, 10, nmos::media_types::video_v210, nodeModel.settings);
                            flow.data[U("label")] = web::json::value::string(us(channel.label + " video"));
                            tagGroup(flow, channel.label, "Video");
                            ensure(nodeModel.node_resources, std::move(flow));
                            auto sender = nmos::make_sender(us(ids.videoSender(channel)), us(ids.videoFlow(channel)), nmos::transports::mxl, us(ids.device), utility::string_t{}, std::vector<utility::string_t>{}, nodeModel.settings);
                            sender.data[U("label")] = web::json::value::string(us(channel.label + " video"));
                            tagGroup(sender, channel.label, "Video");
                            ensure(nodeModel.node_resources, std::move(sender));
                            auto connection = nmos::make_connection_mxl_sender(us(ids.videoSender(channel)), us(cfg.mxlOutputDomainId), us(ids.videoFlow(channel)));
                            connection.data[U("active")][U("master_enable")] = web::json::value::boolean(true);
                            connection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = web::json::value::string(us(cfg.mxlOutputDomainId));
                            connection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(us(ids.videoFlow(channel)));
                            ensure(nodeModel.connection_resources, std::move(connection));
                            senderIds.push_back(us(ids.videoSender(channel)));
                            int audioIndex = 0;
                            for (auto const& output : channel.audioOutputs)
                            {
                                std::vector<nmos::channel> audioChannels;
                                for (int i = 0; i < output.channels; ++i)
                                {
                                    audioChannels.push_back({us("Ch" + std::to_string(i + 1)), nmos::channel_symbols::Undefined(static_cast<unsigned>(i + 1))});
                                }
                                auto audioSource = nmos::make_audio_source(us(ids.audioSource(channel, audioIndex)), us(ids.device), nmos::rational{48000, 1}, audioChannels, nodeModel.settings);
                                audioSource.data[U("label")] = web::json::value::string(us(channel.label + " audio " + std::to_string(audioIndex + 1)));
                                tagGroup(audioSource, channel.label, "Audio " + std::to_string(audioIndex + 1));
                                ensure(nodeModel.node_resources, std::move(audioSource));
                                auto audioFlow = nmos::make_raw_audio_flow(us(ids.audioFlow(channel, audioIndex)), us(ids.audioSource(channel, audioIndex)), us(ids.device),
                                    nmos::rational{48000, 1}, nmos::media_types::audio_float32, 32, nodeModel.settings);
                                audioFlow.data[U("label")] = web::json::value::string(us(channel.label + " audio"));
                                audioFlow.data[U("channel_count")] = output.channels;
                                tagGroup(audioFlow, channel.label, "Audio " + std::to_string(audioIndex + 1));
                                ensure(nodeModel.node_resources, std::move(audioFlow));
                                auto audioSender = nmos::make_sender(us(ids.audioSender(channel, audioIndex)), us(ids.audioFlow(channel, audioIndex)), nmos::transports::mxl, us(ids.device), utility::string_t{}, std::vector<utility::string_t>{}, nodeModel.settings);
                                audioSender.data[U("label")] = web::json::value::string(us(channel.label + " audio"));
                                tagGroup(audioSender, channel.label, "Audio " + std::to_string(audioIndex + 1));
                                ensure(nodeModel.node_resources, std::move(audioSender));
                                auto audioConnection = nmos::make_connection_mxl_sender(us(ids.audioSender(channel, audioIndex)), us(cfg.mxlOutputDomainId), us(ids.audioFlow(channel, audioIndex)));
                                audioConnection.data[U("active")][U("master_enable")] = web::json::value::boolean(true);
                                audioConnection.data[U("active")][U("transport_params")][0][U("mxl_domain_id")] = web::json::value::string(us(cfg.mxlOutputDomainId));
                                audioConnection.data[U("active")][U("transport_params")][0][U("mxl_flow_id")] = web::json::value::string(us(ids.audioFlow(channel, audioIndex)));
                                ensure(nodeModel.connection_resources, std::move(audioConnection));
                                senderIds.push_back(us(ids.audioSender(channel, audioIndex)));
                                ++audioIndex;
                            }
                        }
                        else
                        {
                            auto video = nmos::make_receiver(us(ids.videoReceiver(channel)), us(ids.device), nmos::transports::mxl, {}, nmos::formats::video,
                                {nmos::media_types::video_v210}, nodeModel.settings);
                            video.data[U("label")] = web::json::value::string(us(channel.label + " video"));
                            tagGroup(video, channel.label, "Video");
                            ensure(nodeModel.node_resources, std::move(video));
                            auto audio = nmos::make_receiver(us(ids.audioReceiver(channel)), us(ids.device), nmos::transports::mxl, {}, nmos::formats::audio,
                                {nmos::media_types::audio_float32}, nodeModel.settings);
                            audio.data[U("label")] = web::json::value::string(us(channel.label + " audio"));
                            tagGroup(audio, channel.label, "Audio");
                            ensure(nodeModel.node_resources, std::move(audio));
                            for (auto const& receiverId : {ids.videoReceiver(channel), ids.audioReceiver(channel)})
                            {
                                auto connection = nmos::make_connection_mxl_receiver(us(receiverId), {});
                                connection.data[U("active")][U("master_enable")] = web::json::value::boolean(false);
                                ensure(nodeModel.connection_resources, std::move(connection));
                                receiverIds.push_back(us(receiverId));
                            }
                        }
                    }
                    auto senders = web::json::value::array();
                    auto receivers = web::json::value::array();
                    for (auto const& id : senderIds)
                    {
                        web::json::push_back(senders, web::json::value::string(id));
                    }
                    for (auto const& id : receiverIds)
                    {
                        web::json::push_back(receivers, web::json::value::string(id));
                    }
                    // Touch the device only when its lists change, with a new version: the
                    // registry rejects an update whose version is not newer (400 every 500 ms).
                    auto const device = nmos::find_resource(nodeModel.node_resources, us(ids.device));
                    if (device != nodeModel.node_resources.end() &&
                        (device->data.at(U("senders")) != senders || device->data.at(U("receivers")) != receivers))
                    {
                        nmos::modify_resource(nodeModel.node_resources, us(ids.device), [&](nmos::resource& resource) {
                            resource.data[U("senders")] = senders;
                            resource.data[U("receivers")] = receivers;
                            resource.data[U("version")] = web::json::value::string(nmos::make_version());
                        });
                    }
                };
                sync();
                nodeModel.notify();
                impl_->running.store(true);
                while (!nodeModel.shutdown)
                {
                    nodeModel.wait_for(lock, bst::chrono::milliseconds(500), [&] { return nodeModel.shutdown; });
                    if (nodeModel.shutdown)
                    {
                        break;
                    }
                    sync();
                    nodeModel.notify();
                }
            });
            nmos::server_guard guard(server);
            while (!impl_->running.load() && impl_->error.empty())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            while (impl_->running.load())
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            if (!impl_->config.nmosRegistryAddress.empty())
            {
                auto const path = "/x-nmos/registration/v1.3/resource/node/" + impl_->ids.node;
                auto const status = httpDelete(impl_->config.nmosRegistryAddress, impl_->config.nmosRegistryPort, path, 2000);
                log::info("nmos_deregister", {{"status", std::to_string(status)}});
            }
            {
                auto lock = nodeModel.write_lock();
                nodeModel.shutdown = true;
                nodeModel.notify();
            }
            impl_->model = nullptr;
        }
        catch (std::exception const& ex)
        {
            impl_->error = ex.what();
            impl_->running.store(false);
            log::error("nmos_node_failed", {{"error", ex.what()}});
        }
    });
    for (int i = 0; i < 100 && !impl_->running.load() && impl_->error.empty(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!impl_->error.empty())
    {
        throw std::runtime_error(impl_->error);
    }
}

void NmosNode::stop()
{
    impl_->running.store(false);
    if (impl_->thread.joinable())
    {
        impl_->thread.join();
    }
}
#endif
} // namespace srtgw
