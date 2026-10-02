#pragma once

#include "config/config.hpp"

#include <mutex>
#include <string>

namespace srtgw
{
class ConfigStore
{
public:
    explicit ConfigStore(LoadedConfig loaded);

    [[nodiscard]] LoadedConfig snapshot() const;
    [[nodiscard]] Config config() const;
    void setRestartRequired(bool value);
    ChannelConfig upsertChannel(std::string const& jsonText);
    bool removeChannel(std::string const& id);
    void replaceGlobals(std::map<std::string, std::string> const& values);
    void importJson(std::string const& text);
    void importEnv(std::string const& text);
    void save() const;

private:
    void persistUnlocked() const;

    mutable std::mutex mutex_;
    LoadedConfig loaded_;
};
} // namespace srtgw
