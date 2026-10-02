# Network

SRT is UDP. It does not pass an HTTP proxy. Each channel is either a caller
(outbound UDP) or a listener (inbound UDP).

## What to ask the network team for

The UI shows a ready-to-copy line per channel.

Internet **listener** channel:

- a public address and UDP port
- a NAT or firewall rule to the node IP (`SRTGW_PUBLIC_IP`, the pod `status.hostIP`
  when Kubernetes downward API is used) and the channel's listener port
- the rule limited to known peer ranges when those ranges are known
- the gateway already requires AES-256 and a stream-id allow-list for
  `exposure=internet`

Internet **caller** channel:

- outbound UDP from the node to the remote listener host and port
- no inbound hole is required

Internal channels use the same process. For a harder split, run a second
Deployment on a node or interface that only has the WAN address.

## Port range

Listener ports must sit inside `SRT_PORT_RANGE` (default `9000-9099`). Publish
each one as a UDP `hostPort` or a NodePort. Caller mode needs no Service port.

## Registry

`NMOS_REGISTRY_ADDRESS` is the registry the node can route to. DNS-SD is off
unless `NMOS_DNS_SD=true`. The node API is `NMOS_PORT` (default 3272) and the
events WebSocket is that port plus one.
