#pragma once

#include "config/config.hpp"

#include <string>

namespace srtgw
{
// The srt:// URL the far end of a channel opens (README "Peer URL"): a player such as ffplay or
// VLC for an egress, a sender for an ingest.
// - listener: srt://<host>:<local_port>?mode=caller&latency=<ms>[&streamid=<first accepted id>].
//   <host> is local_address when it is a specific address, else `host` (the announced address).
// - caller: the far end must listen on remote_port: srt://0.0.0.0:<remote_port>?mode=listener&latency=<ms>.
// - rendezvous: srt://<host>:<local_port>?mode=rendezvous&latency=<ms>. The far end must bind
//   remote_port; there is no key for that which every player knows (srt-live-transmit `port`,
//   FFmpeg none and 8.1 rejects unknown keys, VLC no rendezvous), so the UI says it instead.
// An empty `host` is the first non-loopback IPv4. A port of 0 gives an empty URL.
// The passphrase (and pbkeylen when it is not 16) is added only with `withSecret`.
std::string peerUrl(SrtEndpointConfig const& srt, std::string const& host, bool withSecret);
} // namespace srtgw
