#include "channel/runtime.hpp"
#include "config/store.hpp"
#include "mxl/domain.hpp"
#include "nmos/node.hpp"
#include "ops/api.hpp"
#include "srt/socket.hpp"
#include "ops/httpserver.hpp"
#include "util/logging.hpp"
#include "version.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include <pthread.h>
#include <sys/resource.h>

#if defined(SRTGW_HAS_UI)
#include "ops/webui_generated.hpp"
#endif

namespace
{
std::atomic<int> gSignal{0};
} // namespace

int main()
{
    using namespace srtgw;
    // Block termination signals here so every later thread inherits the mask,
    // then wait for one of them. Libraries (nmos-cpp, libsrt) must not be able
    // to swallow SIGTERM on whichever thread happens to receive it.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    // Every MXL flow keeps one descriptor per grain (50 for 1 s at 50p) and every
    // NVDEC channel opens a CUDA device. Docker's default soft limit of 1024 runs
    // out at about 16 ingest channels, and the CUDA device then fails to open.
    rlimit files{};
    if (getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur < files.rlim_max)
    {
        files.rlim_cur = files.rlim_max;
        setrlimit(RLIMIT_NOFILE, &files);
    }
    std::thread([&signals] {
        int number = 0;
        if (sigwait(&signals, &number) == 0)
        {
            gSignal.store(number);
        }
    }).detach();
    try
    {
        auto env = environmentValues();
        std::map<std::string, std::string> fileValues;
        std::string channelsJson;
        std::string configPath;
        if (env.count("SRTGW_CONFIG_FILE") != 0)
        {
            configPath = env.at("SRTGW_CONFIG_FILE");
            readConfigFile(configPath, &fileValues, &channelsJson);
        }
        auto loaded = loadFromSources(fileValues, channelsJson, env);
        loaded.config.configFile = configPath;
        log::setLevel(loaded.config.logLevel);
        log::info("starting", {{"version", kVersion}, {"mxl", kMxlRef}, {"web_port", std::to_string(loaded.config.webPort)}});
        ConfigStore store(std::move(loaded));
        ChannelManager channels(store);
        channels.start();
        NmosNode nmos(store, channels);
        try
        {
            nmos.start();
        }
        catch (std::exception const& ex)
        {
            log::error("nmos_start_failed", {{"error", ex.what()}});
        }
        Api api(store, channels, [&nmos] { return nmos.summary(); }, [&nmos] { return nmos.registered(); });
#if defined(SRTGW_HAS_UI)
        api.setIndex(std::string(webui::indexHtml()));
#endif
        HttpServer server;
        server.setHandler([&api](HttpRequest const& request) { return api.handle(request); });
        if (!server.start(store.config().webPort))
        {
            log::error("web_bind_failed", {{"port", std::to_string(store.config().webPort)}});
            return 75;
        }
        log::info("web_listening", {{"port", std::to_string(server.port())}});
        std::atomic<bool> run{true};
        std::thread events([&] {
            while (run.load())
            {
                server.broadcast(channels.statusJson());
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        while (gSignal.load() == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        run.store(false);
        events.join();
        int const signal = gSignal.load();
        log::info("stopping", {{"signal", std::to_string(signal)}});
        auto const budget = std::chrono::seconds(std::max(1, store.config().shutdownTimeoutS));
        auto const deadline = std::chrono::steady_clock::now() + budget;
        interruptSrt();
        channels.stop();
        nmos.stop();
        if (store.config().cleanupOnExit)
        {
            removeOwnDomain(store.config().mxlOutputDomainDir, store.config().mxlOutputDomainId);
        }
        server.stop();
        if (std::chrono::steady_clock::now() > deadline)
        {
            log::warn("shutdown_timeout", {{"seconds", std::to_string(store.config().shutdownTimeoutS)}});
        }
        if (signal == SIGTERM)
        {
            return 143;
        }
        return signal == 0 ? 0 : 128 + signal;
    }
    catch (ConfigError const& ex)
    {
        log::error("config_invalid", {{"error", ex.what()}});
        std::cerr << "{\"level\":\"error\",\"event\":\"config_invalid\",\"error\":\"" << log::jsonEscape(ex.what()) << "\"}\n";
        return 78;
    }
    catch (std::exception const& ex)
    {
        log::error("startup_failed", {{"error", ex.what()}});
        return 75;
    }
}
