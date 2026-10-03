#include "config/store.hpp"

#include "util/jsonutil.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

namespace srtgw
{
ConfigStore::ConfigStore(LoadedConfig loaded)
    : loaded_(std::move(loaded))
{
}

LoadedConfig ConfigStore::snapshot() const
{
    std::lock_guard const lock{mutex_};
    return loaded_;
}

Config ConfigStore::config() const
{
    std::lock_guard const lock{mutex_};
    return loaded_.config;
}

void ConfigStore::setRestartRequired(bool value)
{
    std::lock_guard const lock{mutex_};
    loaded_.restartRequired = value;
}

void ConfigStore::persistUnlocked() const
{
    if (loaded_.config.configFile.empty())
    {
        return;
    }
    auto const text = configToJson(loaded_, true);
    auto const path = loaded_.config.configFile;
    auto const temporary = path + ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out)
        {
            throw ConfigError("cannot write " + temporary);
        }
        out << text << '\n';
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        throw ConfigError("cannot replace " + path);
    }
}

ChannelConfig ConfigStore::upsertChannel(std::string const& jsonText)
{
    std::lock_guard const lock{mutex_};
    std::string error;
    auto const root = json::parse(jsonText, &error);
    if (!error.empty())
    {
        throw ConfigError(error);
    }
    auto id = json::fieldString(root, "id", "");
    ChannelConfig const* previous = nullptr;
    if (!id.empty())
    {
        for (auto const& channel : loaded_.config.channels)
        {
            if (channel.id == id)
            {
                previous = &channel;
                break;
            }
        }
    }
    auto updated = channelFromJson(jsonText, previous);
    if (updated.id.empty())
    {
        int n = 1;
        while (true)
        {
            auto const candidate = "ch" + std::to_string(n++);
            bool used = false;
            for (auto const& channel : loaded_.config.channels)
            {
                if (channel.id == candidate)
                {
                    used = true;
                    break;
                }
            }
            if (!used)
            {
                updated.id = candidate;
                if (updated.label.empty())
                {
                    updated.label = candidate;
                }
                break;
            }
        }
    }
    if (updated.srt.mode != "caller" && updated.srt.localPort == 0)
    {
        auto const assigned = assignListenerPort(loaded_.config, updated);
        if (assigned.empty())
        {
            throw ConfigError("no free port in SRT_PORT_RANGE");
        }
        updated.srt.localPort = std::stoi(assigned);
    }
    auto next = loaded_.config;
    bool replaced = false;
    for (auto& channel : next.channels)
    {
        if (channel.id == updated.id)
        {
            channel = updated;
            replaced = true;
            break;
        }
    }
    if (!replaced)
    {
        next.channels.push_back(updated);
    }
    validateConfig(next);
    loaded_.config = std::move(next);
    persistUnlocked();
    return updated;
}

bool ConfigStore::removeChannel(std::string const& id)
{
    std::lock_guard const lock{mutex_};
    auto& channels = loaded_.config.channels;
    auto const before = channels.size();
    channels.erase(std::remove_if(channels.begin(), channels.end(), [&](ChannelConfig const& channel) { return channel.id == id; }), channels.end());
    if (channels.size() == before)
    {
        return false;
    }
    persistUnlocked();
    return true;
}

void ConfigStore::replaceGlobals(std::map<std::string, std::string> const& values)
{
    std::lock_guard const lock{mutex_};
    std::map<std::string, std::string> fileValues = {
        {"HOST_ID", loaded_.config.hostId},
        {"MXL_DOMAIN_SCAN_PATH", loaded_.config.mxlDomainScanPath},
        {"MXL_OUTPUT_DOMAIN_DIR", loaded_.config.mxlOutputDomainDir},
        {"MXL_OUTPUT_DOMAIN_ID", loaded_.config.mxlOutputDomainId},
        {"DECODER", loaded_.config.decoder},
        {"ENCODER", loaded_.config.encoder},
        {"SRT_PORT_RANGE", std::to_string(loaded_.config.srtPortMin) + "-" + std::to_string(loaded_.config.srtPortMax)},
        {"NMOS_REGISTRY_ADDRESS", loaded_.config.nmosRegistryAddress},
        {"NMOS_REGISTRY_PORT", std::to_string(loaded_.config.nmosRegistryPort)},
        {"NMOS_QUERY_ADDRESS", loaded_.config.nmosQueryAddress},
        {"NMOS_QUERY_PORT", std::to_string(loaded_.config.nmosQueryPort)},
        {"NMOS_DNS_SD", loaded_.config.nmosDnsSd ? "true" : "false"},
        {"NMOS_PORT", std::to_string(loaded_.config.nmosPort)},
        {"NMOS_SEED", loaded_.config.nmosSeed},
        {"NMOS_LABEL", loaded_.config.nmosLabel},
        {"NMOS_TAGS", loaded_.config.nmosTagsJson},
        {"WEB_PORT", std::to_string(loaded_.config.webPort)},
        {"LOG_LEVEL", loaded_.config.logLevel},
        {"NMOS_HOST_ADDRESS", loaded_.config.hostAddress},
        {"SRTGW_PUBLIC_IP", loaded_.config.hostAddress},
        {"MXL_HISTORY_DURATION_MS", std::to_string(loaded_.config.historyDurationNs / 1000000)},
        {"SRTGW_HISTORY_DURATION_NS", std::to_string(loaded_.config.historyDurationNs)},
        {"STATE_DIR", loaded_.config.stateDir},
        {"SHUTDOWN_TIMEOUT_S", std::to_string(loaded_.config.shutdownTimeoutS)},
        {"MXL_CLEANUP_ON_EXIT", loaded_.config.cleanupOnExit ? "true" : "false"},
    };
    for (auto const& key : globalKeys())
    {
        auto const it = values.find(key);
        if (it != values.end())
        {
            fileValues[key] = it->second;
            if (isRestartKey(key) && loaded_.origin[key] != ValueOrigin::Default)
            {
                loaded_.restartRequired = true;
            }
            if (isRestartKey(key))
            {
                loaded_.restartRequired = true;
            }
        }
    }
    std::string channels = "[";
    bool first = true;
    for (auto const& channel : loaded_.config.channels)
    {
        if (!first)
        {
            channels += ",";
        }
        first = false;
        channels += channelToJson(channel, true);
    }
    channels += "]";
    auto const filePath = loaded_.config.configFile;
    auto reloaded = loadFromSources(fileValues, channels, {});
    reloaded.config.configFile = filePath;
    reloaded.restartRequired = loaded_.restartRequired;
    if (values.count("LOG_LEVEL") != 0)
    {
        reloaded.config.logLevel = values.at("LOG_LEVEL");
    }
    loaded_ = std::move(reloaded);
    persistUnlocked();
}

void ConfigStore::importJson(std::string const& text)
{
    std::map<std::string, std::string> globals;
    std::string channels;
    std::string error;
    auto const root = json::parse(text, &error);
    if (!error.empty() || !root.is<picojson::object>())
    {
        throw ConfigError("import is not a JSON object");
    }
    for (auto const& key : globalKeys())
    {
        if (!json::has(root, key))
        {
            continue;
        }
        auto const value = json::field(root, key);
        if (value && (value->is<picojson::object>() || value->is<picojson::array>()))
        {
            globals[key] = value->serialize();
        }
        else
        {
            globals[key] = json::fieldString(root, key, "");
        }
    }
    if (json::has(root, "channels"))
    {
        channels = json::field(root, "channels")->serialize();
    }
    auto const filePath = loaded_.config.configFile;
    std::lock_guard const lock{mutex_};
    auto const previous = loaded_.config.channels;
    auto reloaded = loadFromSources(globals, channels, {});
    for (auto& channel : reloaded.config.channels)
    {
        bool passphraseSent = false;
        bool backupPassphraseSent = false;
        if (json::has(root, "channels") && json::field(root, "channels")->is<picojson::array>())
        {
            for (auto const& item : json::field(root, "channels")->get<picojson::array>())
            {
                if (json::fieldString(item, "id", "") != channel.id)
                {
                    continue;
                }
                if (json::has(item, "srt") && json::has(*json::field(item, "srt"), "passphrase"))
                {
                    passphraseSent = true;
                }
                if (json::has(item, "backup") && json::has(*json::field(item, "backup"), "endpoint") &&
                    json::has(*json::field(*json::field(item, "backup"), "endpoint"), "passphrase"))
                {
                    backupPassphraseSent = true;
                }
            }
        }
        for (auto const& old : previous)
        {
            if (old.id != channel.id)
            {
                continue;
            }
            if (!passphraseSent)
            {
                channel.srt.passphrase = old.srt.passphrase;
            }
            if (!backupPassphraseSent)
            {
                channel.backup.endpoint.passphrase = old.backup.endpoint.passphrase;
            }
        }
    }
    reloaded.config.configFile = filePath;
    reloaded.restartRequired = true;
    loaded_ = std::move(reloaded);
    persistUnlocked();
}

void ConfigStore::importEnv(std::string const& text)
{
    std::map<std::string, std::string> values;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        auto const eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        values[line.substr(0, eq)] = line.substr(eq + 1);
    }
    replaceGlobals(values);
}

void ConfigStore::save() const
{
    std::lock_guard const lock{mutex_};
    persistUnlocked();
}
} // namespace srtgw
