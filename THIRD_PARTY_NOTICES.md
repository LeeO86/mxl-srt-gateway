# Third-party notices

The sources of mxl-srt-gateway are MIT, as in `LICENSE`.

The container image produced by `docker/Dockerfile` is **GPL-2.0-or-later**.
It is linked with libx264 (GPL) and libx265 (GPL). Shipping that image is a
distribution of those libraries under the GPL. The MIT sources do not become
GPL merely by sitting next to the Dockerfile; the binary in the image does.

Other components, under their own licences:

| Component | Licence | How it is used |
| --- | --- | --- |
| FFmpeg `n7.1.5` | LGPL 2.1+ / GPL when `--enable-gpl` | demux, decode, filters, encode, mux. Built in the image. |
| libx264, libx265 | GPL-2.0-or-later | H.264 and HEVC encoders |
| libsoxr | LGPL-2.1+ | resampler, selected when FFmpeg was built with it; swresample remains the fallback |
| libsrt `v1.5.4` | MPL-2.0 | SRT sockets, encryption via OpenSSL in the image |
| OpenSSL | Apache-2.0 | libsrt encryption in the image |
| MXL `218ddaa` | Apache-2.0 | shared-memory flows |
| nmos-cpp `fe30384` | Apache-2.0 | IS-04 / IS-05 node |
| Vue 3, Vite | MIT | admin UI, compiled in, no CDN |
| doctest | MIT | `third_party/doctest` |
| picojson | BSD-2-Clause | `third_party/picojson`, config JSON |

GnuTLS builds of libsrt (the Ubuntu package used by CI) are also MPL-2.0.
