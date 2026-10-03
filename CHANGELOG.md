# Changelog

## 1.0.0

Stable settings and API contract for the MXL platform. A later breaking change
needs 2.0.0.

- `NMOS_HOST_ADDRESS` is the address announced to NMOS, IS-05 and the UI.
  `SRTGW_PUBLIC_IP` is the same setting.
- `NMOS_LABEL` and `NMOS_TAGS` set the node and device label and tags.
  `NMOS_SEED` still derives every id.
- `NMOS_QUERY_ADDRESS` defaults to the registry address.
  `NMOS_QUERY_PORT` defaults to the registration port plus one.
  `NMOS_DNS_SD=false` (the default) does not browse or advertise with mDNS.
- `/readyz` stays 503 until the Query API lists the node when a registry is set.
- SIGTERM deletes the node from the Registration API, and with
  `MXL_CLEANUP_ON_EXIT=true` removes only this function's output domain.
  The exit code is 143. `SHUTDOWN_TIMEOUT_S` defaults to 10.
- `MXL_HISTORY_DURATION_MS` sets the domain history. `SRTGW_HISTORY_DURATION_NS`
  still works.
- IS-05 activations are stored in `STATE_DIR/routes.json` (default `/config`).
- `GET /api/v1/config/export` omits passphrases unless `secrets=1`.
  An import that omits a passphrase keeps the one already stored.
  Passphrases live in the config file and are not logged.
- An existing `domain_def.json` with a different id is not overwritten.
- The image is published as `git-<sha7>` and `nightly-dev` from `main`, and as
  `X.Y.Z`, `X.Y` and `X` from a `vX.Y.Z` tag. Version tags are not moved.
