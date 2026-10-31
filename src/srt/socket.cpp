#include "srt/socket.hpp"

#include "util/logging.hpp"
#include "util/net.hpp"

#include <srt/srt.h>

#include <arpa/inet.h>
#include <netdb.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace srtgw
{
namespace
{
// srt_cleanup() runs from cleanupSrt() at the end of a shutdown. From this static's destructor
// (exit, __cxa_finalize) it raced libsrt's own static teardown and crashed the exit (SIGSEGV
// instead of 143) once SRT had been used.
std::atomic<bool> gStarted{false};

struct Startup
{
    Startup()
    {
        srt_startup();
        gStarted.store(true);
    }
};

Startup& startup()
{
    static Startup instance;
    return instance;
}

bool setOpt(int fd, SRT_SOCKOPT key, void const* value, int size)
{
    if (srt_setsockopt(fd, 0, key, value, size) == SRT_ERROR)
    {
        log::warn("srt_setopt_failed", {{"option", std::to_string(static_cast<int>(key))}, {"error", srt_getlasterror_str()}});
        return false;
    }
    return true;
}

std::string peerOf(int fd)
{
    sockaddr_storage storage{};
    int length = sizeof(storage);
    if (srt_getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) == SRT_ERROR)
    {
        return {};
    }
    char host[NI_MAXHOST] = {};
    char serv[NI_MAXSERV] = {};
    if (getnameinfo(reinterpret_cast<sockaddr*>(&storage), static_cast<socklen_t>(length), host, sizeof(host), serv, sizeof(serv),
            NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    {
        return {};
    }
    return std::string(host) + ":" + serv;
}

std::string ipOf(sockaddr_storage const& storage)
{
    char host[NI_MAXHOST] = {};
    if (getnameinfo(reinterpret_cast<sockaddr const*>(&storage), sizeof(storage), host, sizeof(host), nullptr, 0, NI_NUMERICHOST) != 0)
    {
        return {};
    }
    return host;
}

bool resolve(std::string const& host, int port, sockaddr_storage* out, int* length)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    auto const portText = std::to_string(port);
    if (getaddrinfo(host.empty() ? nullptr : host.c_str(), portText.c_str(), &hints, &result) != 0 || result == nullptr)
    {
        return false;
    }
    std::memcpy(out, result->ai_addr, result->ai_addrlen);
    *length = static_cast<int>(result->ai_addrlen);
    freeaddrinfo(result);
    return true;
}

std::string streamIdOf(int fd)
{
    std::vector<char> buffer(512, 0);
    int length = static_cast<int>(buffer.size());
    if (srt_getsockopt(fd, 0, SRTO_STREAMID, buffer.data(), &length) == SRT_ERROR || length <= 0)
    {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(length));
}
} // namespace

namespace
{
std::mutex gLinksMu;
std::set<SrtLink*> gLinks;
std::atomic<bool> gInterrupted{false};
} // namespace

void interruptSrt()
{
    gInterrupted.store(true);
    std::lock_guard const lock{gLinksMu};
    for (auto* link : gLinks)
    {
        link->closeAll();
    }
}

void cleanupSrt()
{
    if (gStarted.exchange(false))
    {
        srt_cleanup();
    }
}

bool srtInterrupted()
{
    return gInterrupted.load();
}

SrtLink::SrtLink()
{
    startup();
    std::lock_guard const lock{gLinksMu};
    gLinks.insert(this);
}

SrtLink::~SrtLink()
{
    {
        std::lock_guard const lock{gLinksMu};
        gLinks.erase(this);
    }
    closeAll();
}

void SrtLink::configure(SrtEndpointConfig config, std::string channelId)
{
    std::lock_guard const lock{mutex_};
    config_ = std::move(config);
    channelId_ = std::move(channelId);
}

void SrtLink::applyOptions(int fd, bool caller) const
{
    int const yes = 1;
    SRT_TRANSTYPE const live = SRTT_LIVE;
    setOpt(fd, SRTO_TRANSTYPE, &live, sizeof(live));
    int const latency = config_.latencyMs > 0 ? config_.latencyMs : 200;
    setOpt(fd, SRTO_LATENCY, &latency, sizeof(latency));
    int const payload = config_.payloadSize > 0 ? config_.payloadSize : 1316;
    setOpt(fd, SRTO_PAYLOADSIZE, &payload, sizeof(payload));
    int const overhead = config_.overhead > 0 ? config_.overhead : 25;
    setOpt(fd, SRTO_OHEADBW, &overhead, sizeof(overhead));
    if (config_.maxBandwidth > 0)
    {
        std::int64_t const maxbw = config_.maxBandwidth;
        setOpt(fd, SRTO_MAXBW, &maxbw, sizeof(maxbw));
    }
    int const timeout = config_.connectTimeoutMs > 0 ? config_.connectTimeoutMs : 3000;
    setOpt(fd, SRTO_CONNTIMEO, &timeout, sizeof(timeout));
    int const recvTimeout = 200;
    setOpt(fd, SRTO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
    int const sendTimeout = 500;
    setOpt(fd, SRTO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    if (!config_.passphrase.empty())
    {
        int key = effectiveKeyLength(policyFor(config_));
        if (key > 0)
        {
            setOpt(fd, SRTO_PBKEYLEN, &key, sizeof(key));
        }
        // libsrt wants the byte count of the passphrase, not a trailing NUL.
        setOpt(fd, SRTO_PASSPHRASE, config_.passphrase.c_str(), static_cast<int>(config_.passphrase.size()));
    }
    if (caller && !config_.streamId.empty())
    {
        setOpt(fd, SRTO_STREAMID, config_.streamId.c_str(), static_cast<int>(config_.streamId.size()));
    }
    if (config_.mode == "rendezvous")
    {
        setOpt(fd, SRTO_RENDEZVOUS, &yes, sizeof(yes));
    }
    (void)yes;
}

bool SrtLink::openSocket(bool listener)
{
    int const fd = srt_create_socket();
    if (fd == SRT_INVALID_SOCK)
    {
        return false;
    }
    applyOptions(fd, !listener);
    if (listener)
    {
        listenFd_ = fd;
        listenEid_ = srt_epoll_create();
        int const events = SRT_EPOLL_IN;
        srt_epoll_add_usock(listenEid_, listenFd_, &events);
    }
    else
    {
        dataFd_ = fd;
    }
    return true;
}

void SrtLink::logReject(std::string const& peer, std::string const& reason, std::string const& check) const
{
    auto const colon = peer.find(':');
    std::vector<log::Field> fields{{"channel", channelId_}, {"peer", colon == std::string::npos ? peer : peer.substr(0, colon)}, {"reason", reason}};
    if (!check.empty())
    {
        // A second peer turned away by an access check: which one.
        fields.push_back({"check", check});
    }
    log::warn("srt_rejected", fields);
}

bool SrtLink::acceptOne(std::atomic<bool> const& stop, AttemptLimiter& limiter)
{
    if (listenFd_ < 0 && !openSocket(true))
    {
        return false;
    }
    sockaddr_in local4{};
    sockaddr_storage local{};
    int localLength = 0;
    sockaddr const* bindAddr = nullptr;
    std::string const bindHost = config_.localAddress.empty() ? "0.0.0.0" : config_.localAddress;
    local4.sin_family = AF_INET;
    local4.sin_port = htons(static_cast<uint16_t>(config_.localPort));
    if (inet_pton(AF_INET, bindHost.c_str(), &local4.sin_addr) == 1)
    {
        bindAddr = reinterpret_cast<sockaddr const*>(&local4);
        localLength = static_cast<int>(sizeof(local4));
    }
    else if (resolve(bindHost, config_.localPort, &local, &localLength))
    {
        bindAddr = reinterpret_cast<sockaddr const*>(&local);
    }
    else
    {
        log::error("srt_bind_failed", {{"channel", channelId_}, {"address", bindHost}, {"port", std::to_string(config_.localPort)}});
        return false;
    }
    srt_clearlasterror();
    if (srt_bind(listenFd_, bindAddr, localLength) == SRT_ERROR)
    {
        int code = 0;
        std::string const message = srt_getlasterror_str();
        code = srt_getlasterror(nullptr);
        bool const already = message.find("bound") != std::string::npos || message.find("not supported") != std::string::npos || code == SRT_EBOUNDSOCK;
        if (!already)
        {
            log::error("srt_bind_failed", {{"channel", channelId_}, {"error", message}, {"code", std::to_string(code)}, {"port", std::to_string(config_.localPort)}});
            srt_epoll_release(listenEid_);
            listenEid_ = -1;
            srt_close(listenFd_);
            listenFd_ = -1;
            return false;
        }
    }
    if (srt_listen(listenFd_, 2) == SRT_ERROR && srt_getlasterror(nullptr) != SRT_EBOUNDSOCK)
    {
        // srt_listen on an already listening socket returns an error; ignore that case.
    }
    int const eid = listenEid_;
    while (!stop.load() && !gInterrupted.load())
    {
        SRTSOCKET ready[4] = {};
        int readyCount = 4;
        int writeCount = 0;
        int const waited = srt_epoll_wait(eid, ready, &readyCount, nullptr, &writeCount, 200, nullptr, nullptr, nullptr, nullptr);
        if (waited == SRT_ERROR)
        {
            // Anything but the 200 ms timeout: the epoll is gone (interruptSrt() closed this link
            // at shutdown). Polling it again spun on "EID INVALID" until the channel stopped.
            if (srt_getlasterror(nullptr) != SRT_ETIMEOUT)
            {
                return false;
            }
            continue;
        }
        std::string peerText;
        std::string streamId;
        int const client = acceptChecked(limiter, nullptr, &peerText, &streamId);
        if (client < 0)
        {
            continue;
        }
        dataFd_ = client;
        std::lock_guard const lock{mutex_};
        stats_.connected = true;
        stats_.peer = peerText;
        stats_.streamId = streamId;
        log::info("srt_connected", {{"channel", channelId_}, {"peer", stats_.peer}, {"mode", config_.mode}});
        return true;
    }
    return false;
}

int SrtLink::acceptChecked(AttemptLimiter& limiter, char const* rejectAs, std::string* peerText, std::string* streamId)
{
    sockaddr_storage peer{};
    int peerLength = sizeof(peer);
    int const client = srt_accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (client == SRT_INVALID_SOCK)
    {
        std::string const message = srt_getlasterror_str();
        if (message.find("password") != std::string::npos || message.find("reject") != std::string::npos || message.find("Passphrase") != std::string::npos)
        {
            logReject(ipOf(peer), "passphrase");
        }
        return -1;
    }
    *peerText = peerOf(client);
    auto const ip = ipOf(peer);
    AccessInput input;
    input.peerIp = ip;
    input.streamId = streamIdOf(client);
    input.nowMs = monoNowMs();
    auto const decision = evaluateAccess(policyFor(config_), input, &limiter);
    if (!decision.accept)
    {
        limiter.record(ip, input.nowMs);
        logReject(peerText->empty() ? ip : *peerText, rejectAs != nullptr ? rejectAs : decision.reason, rejectAs != nullptr ? decision.reason : "");
        srt_close(client);
        return -1;
    }
    int const recvTimeout = 200;
    int const sendTimeout = 500;
    setOpt(client, SRTO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));
    setOpt(client, SRTO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    *streamId = input.streamId;
    return client;
}

bool SrtLink::takeOverPending(AttemptLimiter& limiter)
{
    if (!hasPendingCaller())
    {
        return false;
    }
    std::string peerText;
    std::string streamId;
    int const client = acceptChecked(limiter, "second_peer", &peerText, &streamId);
    if (client < 0)
    {
        return false;
    }
    int old = -1;
    std::string oldPeer;
    {
        std::lock_guard const lock{mutex_};
        old = dataFd_;
        oldPeer = stats_.peer;
        dataFd_ = client;
        stats_ = SrtStats{};
        stats_.connected = true;
        stats_.peer = peerText;
        stats_.streamId = streamId;
    }
    if (old >= 0)
    {
        srt_close(old);
    }
    log::warn("srt_peer_replaced", {{"channel", channelId_}, {"old", oldPeer}, {"peer", peerText}});
    log::info("srt_connected", {{"channel", channelId_}, {"peer", peerText}, {"mode", config_.mode}});
    return true;
}

bool SrtLink::establish(std::atomic<bool> const& stop, AttemptLimiter& limiter)
{
    if (stop.load() || gInterrupted.load())
    {
        return false;
    }
    closeData();
    if (config_.mode == "listener")
    {
        return acceptOne(stop, limiter);
    }
    if (!openSocket(false))
    {
        return false;
    }
    if (!config_.localAddress.empty() || config_.localPort != 0 || config_.mode == "rendezvous")
    {
        sockaddr_storage local{};
        int localLength = 0;
        std::string const host = config_.localAddress.empty() ? "0.0.0.0" : config_.localAddress;
        int const port = config_.localPort == 0 ? 0 : config_.localPort;
        if (resolve(host, port, &local, &localLength))
        {
            srt_bind(dataFd_, reinterpret_cast<sockaddr*>(&local), localLength);
        }
    }
    sockaddr_storage remote{};
    int remoteLength = 0;
    if (!resolve(config_.remoteHost, config_.remotePort, &remote, &remoteLength))
    {
        log::warn("srt_resolve_failed", {{"channel", channelId_}, {"host", config_.remoteHost}});
        closeData();
        return false;
    }
    // Connects without blocking, so a stop or a shutdown does not wait out the connect timeout
    // (3 s by default, which a caller backing off spent in srt_connect): the state is polled
    // every 20 ms until SRT connects or gives up (it still applies SRTO_CONNTIMEO).
    bool const blocking = false;
    setOpt(dataFd_, SRTO_RCVSYN, &blocking, sizeof(blocking));
    bool connected = srt_connect(dataFd_, reinterpret_cast<sockaddr*>(&remote), remoteLength) != SRT_ERROR;
    std::string message = connected ? "" : srt_getlasterror_str();
    while (connected)
    {
        SRT_SOCKSTATUS const state = srt_getsockstate(dataFd_);
        if (state == SRTS_CONNECTED)
        {
            break;
        }
        if ((state != SRTS_CONNECTING && state != SRTS_OPENED) || stop.load() || gInterrupted.load())
        {
            connected = false;
            message = srt_rejectreason_str(srt_getrejectreason(dataFd_));
            if (srt_getrejectreason(dataFd_) == SRT_REJ_BADSECRET)
            {
                message = "Passphrase";
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!connected)
    {
        if (message.find("password") != std::string::npos || message.find("Passphrase") != std::string::npos)
        {
            logReject(config_.remoteHost, "passphrase");
        }
        else
        {
            log::debug("srt_connect_failed", {{"channel", channelId_}, {"error", message}});
        }
        closeData();
        return false;
    }
    // Back to blocking reads (with SRTO_RCVTIMEO) for the data.
    bool const sync = true;
    setOpt(dataFd_, SRTO_RCVSYN, &sync, sizeof(sync));
    std::lock_guard const lock{mutex_};
    stats_.connected = true;
    stats_.peer = peerOf(dataFd_);
    stats_.streamId = config_.streamId;
    log::info("srt_connected", {{"channel", channelId_}, {"peer", stats_.peer}, {"mode", config_.mode}});
    return !stop.load();
}

void SrtLink::closeData()
{
    std::lock_guard const lock{mutex_};
    if (dataFd_ >= 0)
    {
        srt_close(dataFd_);
        dataFd_ = -1;
    }
    stats_.connected = false;
}

void SrtLink::closeAll()
{
    int data = -1;
    int listen = -1;
    {
        std::lock_guard const lock{mutex_};
        data = dataFd_;
        listen = listenFd_;
        dataFd_ = -1;
        listenFd_ = -1;
        stats_.connected = false;
    }
    if (data >= 0)
    {
        srt_close(data);
    }
    if (listen >= 0)
    {
        srt_epoll_release(listenEid_);
        listenEid_ = -1;
        srt_close(listen);
    }
}

int SrtLink::recv(std::uint8_t* data, int size)
{
    int fd = -1;
    {
        std::lock_guard const lock{mutex_};
        fd = dataFd_;
    }
    if (fd < 0)
    {
        return -1;
    }
    int const n = srt_recv(fd, reinterpret_cast<char*>(data), size);
    if (n == SRT_ERROR)
    {
        if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
        {
            return 0;
        }
        closeData();
        return -1;
    }
    return n;
}

int SrtLink::send(std::uint8_t const* data, int size)
{
    int fd = -1;
    {
        std::lock_guard const lock{mutex_};
        fd = dataFd_;
    }
    if (fd < 0)
    {
        return -1;
    }
    int const chunk = config_.payloadSize > 0 ? config_.payloadSize : 1316;
    int sent = 0;
    while (sent < size)
    {
        int const piece = std::min(chunk, size - sent);
        int const n = srt_send(fd, reinterpret_cast<char const*>(data + sent), piece);
        if (n == SRT_ERROR)
        {
            if (srt_getlasterror(nullptr) == SRT_ETIMEOUT)
            {
                return sent > 0 ? sent : -1;
            }
            static int logged = 0;
            if (logged < 3)
            {
                ++logged;
                log::warn("srt_send_failed", {{"channel", channelId_}, {"error", srt_getlasterror_str()}, {"bytes", std::to_string(piece)}});
            }
            closeData();
            return -1;
        }
        sent += n;
    }
    return sent;
}

void SrtLink::rejectPending(AttemptLimiter& limiter)
{
    if (listenFd_ < 0 || dataFd_ < 0)
    {
        return;
    }
    SRTSOCKET ready[2] = {};
    int readyCount = 2;
    int writeCount = 0;
    int const waited = srt_epoll_wait(listenEid_, ready, &readyCount, nullptr, &writeCount, 0, nullptr, nullptr, nullptr, nullptr);
    if (waited == SRT_ERROR || readyCount <= 0)
    {
        return;
    }
    sockaddr_storage peer{};
    int peerLength = sizeof(peer);
    int const extra = srt_accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (extra == SRT_INVALID_SOCK)
    {
        return;
    }
    auto const ip = ipOf(peer);
    limiter.record(ip, monoNowMs());
    logReject(peerOf(extra).empty() ? ip : peerOf(extra), "second_peer");
    srt_close(extra);
}

bool SrtLink::hasPendingCaller() const
{
    if (listenFd_ < 0 || dataFd_ < 0)
    {
        return false;
    }
    SRTSOCKET ready[2] = {};
    int readyCount = 2;
    int writeCount = 0;
    int const waited = srt_epoll_wait(listenEid_, ready, &readyCount, nullptr, &writeCount, 0, nullptr, nullptr, nullptr, nullptr);
    return waited != SRT_ERROR && readyCount > 0;
}

SrtStats SrtLink::stats() const
{
    std::lock_guard const lock{mutex_};
    SrtStats copy = stats_;
    if (dataFd_ >= 0)
    {
        SRT_TRACEBSTATS perf{};
        if (srt_bstats(dataFd_, &perf, 0) != SRT_ERROR)
        {
            copy.rttMs = perf.msRTT;
            copy.pktLoss = static_cast<std::uint64_t>(std::max(0, perf.pktRcvLossTotal));
            copy.pktRetrans = static_cast<std::uint64_t>(std::max(0, perf.pktRetransTotal));
            copy.pktDrop = static_cast<std::uint64_t>(std::max(0, perf.pktRcvDropTotal));
            double const mbps = perf.mbpsRecvRate > 0 ? perf.mbpsRecvRate : perf.mbpsSendRate;
            copy.bitrateBps = mbps * 1000000.0;
            copy.bufferMs = perf.msRcvBuf;
            copy.connected = true;
        }
    }
    return copy;
}

bool SrtLink::connected() const
{
    std::lock_guard const lock{mutex_};
    return dataFd_ >= 0 && stats_.connected;
}
} // namespace srtgw
