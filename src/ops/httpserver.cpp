#include "ops/httpserver.hpp"

#include "util/sha1.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace srtgw
{
namespace
{
bool sendAll(int fd, std::string const& data)
{
    std::size_t sent = 0;
    while (sent < data.size())
    {
        auto const n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0)
        {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string readHeaders(int fd)
{
    std::string data;
    char buf[2048];
    while (data.find("\r\n\r\n") == std::string::npos && data.size() < 1024 * 1024)
    {
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        data.append(buf, buf + n);
    }
    return data;
}

void wsSend(int fd, std::string const& text)
{
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    if (text.size() < 126)
    {
        frame.push_back(static_cast<char>(text.size()));
    }
    else if (text.size() < 65536)
    {
        frame.push_back(126);
        frame.push_back(static_cast<char>((text.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(text.size() & 0xff));
    }
    else
    {
        return;
    }
    frame += text;
    sendAll(fd, frame);
}
} // namespace

std::string HttpRequest::header(std::string const& name) const
{
    for (auto const& item : headers)
    {
        if (item.first.size() == name.size())
        {
            bool same = true;
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                char a = item.first[i];
                char b = name[i];
                if (a >= 'A' && a <= 'Z')
                {
                    a = static_cast<char>(a - 'A' + 'a');
                }
                if (b >= 'A' && b <= 'Z')
                {
                    b = static_cast<char>(b - 'A' + 'a');
                }
                if (a != b)
                {
                    same = false;
                    break;
                }
            }
            if (same)
            {
                return item.second;
            }
        }
    }
    return {};
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer()
{
    stop();
}

void HttpServer::setHandler(HttpHandler handler)
{
    handler_ = std::move(handler);
}

bool HttpServer::start(int port)
{
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
    {
        return false;
    }
    int const yes = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(listenFd_, 64) != 0)
    {
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &length);
    port_ = ntohs(bound.sin_port);
    running_.store(true);
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop()
{
    running_.store(false);
    if (listenFd_ >= 0)
    {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (thread_.joinable())
    {
        thread_.join();
    }
    std::lock_guard const lock{clientsMu_};
    for (int fd : clients_)
    {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    clients_.clear();
}

int HttpServer::port() const
{
    return port_;
}

void HttpServer::broadcast(std::string const& text)
{
    std::lock_guard const lock{clientsMu_};
    std::vector<int> alive;
    for (int fd : clients_)
    {
        wsSend(fd, text);
        alive.push_back(fd);
    }
    clients_.swap(alive);
}

void HttpServer::acceptLoop()
{
    while (running_.load())
    {
        pollfd ready{};
        ready.fd = listenFd_;
        ready.events = POLLIN;
        if (::poll(&ready, 1, 200) <= 0)
        {
            continue;
        }
        int const fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0)
        {
            continue;
        }
        std::thread(&HttpServer::handle, this, fd).detach();
    }
}

void HttpServer::handle(int fd)
{
    auto const raw = readHeaders(fd);
    auto const split = raw.find("\r\n\r\n");
    if (split == std::string::npos)
    {
        ::close(fd);
        return;
    }
    std::string const head = raw.substr(0, split);
    std::string body = raw.substr(split + 4);
    std::istringstream lines(head);
    std::string requestLine;
    std::getline(lines, requestLine);
    if (!requestLine.empty() && requestLine.back() == '\r')
    {
        requestLine.pop_back();
    }
    HttpRequest request;
    std::istringstream requestStream(requestLine);
    std::string target;
    requestStream >> request.method >> target;
    auto const query = target.find('?');
    if (query == std::string::npos)
    {
        request.path = target;
    }
    else
    {
        request.path = target.substr(0, query);
        request.query = target.substr(query + 1);
    }
    std::string line;
    int contentLength = 0;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        auto const colon = line.find(':');
        if (colon == std::string::npos)
        {
            continue;
        }
        auto name = line.substr(0, colon);
        auto value = line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ')
        {
            value.erase(value.begin());
        }
        request.headers.emplace_back(name, value);
        if (name == "Content-Length" || name == "content-length")
        {
            contentLength = std::stoi(value);
        }
    }
    while (static_cast<int>(body.size()) < contentLength)
    {
        char buf[4096];
        auto const n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
        {
            break;
        }
        body.append(buf, buf + n);
    }
    if (contentLength > 0 && static_cast<int>(body.size()) > contentLength)
    {
        body.resize(static_cast<std::size_t>(contentLength));
    }
    request.body = std::move(body);

    auto const upgrade = request.header("Upgrade");
    if (request.path == "/api/v1/events" && (upgrade == "websocket" || upgrade == "WebSocket"))
    {
        auto const key = request.header("Sec-WebSocket-Key");
        auto const accept = base64Encode(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").data(), 20);
        std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + "\r\n\r\n";
        if (!sendAll(fd, response))
        {
            ::close(fd);
            return;
        }
        {
            std::lock_guard const lock{clientsMu_};
            clients_.push_back(fd);
        }
        while (running_.load())
        {
            char buf[64];
            auto const n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                break;
            }
        }
        std::lock_guard const lock{clientsMu_};
        clients_.erase(std::remove(clients_.begin(), clients_.end(), fd), clients_.end());
        ::close(fd);
        return;
    }

    HttpResponse response = handler_ ? handler_(request) : HttpResponse{404, "text/plain", "not found", {}};
    std::ostringstream out;
    char const* reason = "OK";
    if (response.status == 400)
    {
        reason = "Bad Request";
    }
    else if (response.status == 404)
    {
        reason = "Not Found";
    }
    else if (response.status == 500)
    {
        reason = "Internal Server Error";
    }
    else if (response.status == 503)
    {
        reason = "Service Unavailable";
    }
    out << "HTTP/1.1 " << response.status << " " << reason << "\r\n";
    out << "Content-Type: " << response.contentType << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    for (auto const& header : response.headers)
    {
        out << header.first << ": " << header.second << "\r\n";
    }
    out << "\r\n";
    sendAll(fd, out.str());
    sendAll(fd, response.body);
    ::close(fd);
}
} // namespace srtgw
