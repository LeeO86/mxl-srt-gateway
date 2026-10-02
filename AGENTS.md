# AGENTS.md

`mxl-srt-gateway` is one C++20 process. The Vue UI under `web/` is built with
npm and embedded in the binary. Canonical commands are in `README.md`,
`.github/workflows/ci.yaml` and `tests/integration/ci.sh`.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=g++-13 \
  -DSRTGW_WITH_NMOS=ON -DNMOS_CPP_DIR=/tmp/nmos-cpp/Development \
  -DCMAKE_PREFIX_PATH="/opt/mxl;$HOME/mxl/build/vcpkg_installed/x64-linux"
cmake --build build -j"$(nproc)"
./build/unit-tests
SRTGW_TEST_SECONDS=8 tests/integration/ci.sh ./build/mxl-srt-gateway
```

Lint is `-Wall -Wextra` on the compile. A clean build is the lint signal.

`/dev/shm` on a small VM can be tight for 1080p50 v210 rings. The default
history is 1 s. Integration tests use 720p25. A domain that is not tmpfs logs
`mxl_domain_not_tmpfs` and still runs.

MXL is optional at configure time (`find_package(mxl)`). Without it the SRT
pipelines still run and the writers are no-ops. The container and CI link it.

Passphrases are never logged and never returned by the API. Do not add a debug
print of `SrtEndpointConfig::passphrase`.
