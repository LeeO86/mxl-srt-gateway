# syntax=docker/dockerfile:1
# Pins are the single source of truth shared with CI and IMPLEMENTATION_PLAN.md.
# FFmpeg is built in-tree with NVDEC, NVENC and the CUDA filters (nvcc).
# libcudart is copied into the runtime image so the process starts with no GPU.
# libcuda, libnvidia-encode and libnvcuvid come from the NVIDIA Container Toolkit
# when the container is started with a GPU (NVIDIA_DRIVER_CAPABILITIES below).
ARG CUDA_IMAGE=nvidia/cuda:12.8.2-devel-ubuntu24.04

FROM node:22-bookworm AS webui
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-fund --no-audit
COPY web/ ./
RUN npm run build

FROM ${CUDA_IMAGE} AS build
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
ARG NMOS_CPP_REF=fe303849527394b03bdedc8f161f377fe458bb62
ARG SRT_REF=v1.5.4
ARG FFMPEG_REF=n7.1.5
ARG NVCODEC_REF=n12.2.72.0

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git pkg-config curl zip unzip tar ca-certificates \
        nasm yasm \
        bison flex autoconf automake libtool \
        libboost-system-dev libboost-thread-dev libboost-chrono-dev \
        libboost-date-time-dev libboost-regex-dev libboost-random-dev \
        libboost-filesystem-dev libcpprest-dev nlohmann-json3-dev \
        libssl-dev libavahi-compat-libdnssd-dev uuid-dev \
        libx264-dev libx265-dev libsoxr-dev \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/microsoft/vcpkg /opt/vcpkg \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg
ENV PATH="/usr/local/cuda/bin:${PATH}"

RUN git init /tmp/mxl \
    && git -C /tmp/mxl remote add origin https://github.com/dmf-mxl/mxl.git \
    && git -C /tmp/mxl fetch --depth 1 origin "${MXL_REF}" \
    && git -C /tmp/mxl checkout FETCH_HEAD
COPY docker/mxl-vcpkg.json /tmp/mxl-manifest/vcpkg.json
RUN BASELINE="$(git -C /opt/vcpkg rev-parse HEAD)" \
    && sed -i "1a\\  \"builtin-baseline\": \"${BASELINE}\"," /tmp/mxl-manifest/vcpkg.json
RUN cmake -S /tmp/mxl -B /tmp/mxl/build -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_MANIFEST_DIR=/tmp/mxl-manifest \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=OFF -DBUILD_UTILS=OFF -DBUILD_DOCS=OFF \
        -DMXL_ENABLE_FABRICS_OFI=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/mxl \
    && cmake --build /tmp/mxl/build -j"$(nproc)" \
    && cmake --install /tmp/mxl/build

RUN git init /opt/nmos-cpp \
    && git -C /opt/nmos-cpp remote add origin https://github.com/sony/nmos-cpp.git \
    && git -C /opt/nmos-cpp fetch --depth 1 origin "${NMOS_CPP_REF}" \
    && git -C /opt/nmos-cpp checkout FETCH_HEAD

RUN git init /tmp/srt \
    && git -C /tmp/srt remote add origin https://github.com/Haivision/srt.git \
    && git -C /tmp/srt fetch --depth 1 origin "${SRT_REF}" \
    && git -C /tmp/srt checkout FETCH_HEAD \
    && cmake -S /tmp/srt -B /tmp/srt/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_ENCRYPTION=ON -DUSE_ENCLIB=openssl -DENABLE_SHARED=ON -DENABLE_STATIC=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/srt/build -j"$(nproc)" \
    && cmake --install /tmp/srt/build

RUN git clone --depth 1 --branch "${NVCODEC_REF}" https://github.com/FFmpeg/nv-codec-headers.git /tmp/nv-codec-headers \
    && make -C /tmp/nv-codec-headers install PREFIX=/usr/local
RUN git init /tmp/ffmpeg \
    && git -C /tmp/ffmpeg remote add origin https://github.com/FFmpeg/FFmpeg.git \
    && git -C /tmp/ffmpeg fetch --depth 1 origin "${FFMPEG_REF}" \
    && git -C /tmp/ffmpeg checkout FETCH_HEAD \
    && cd /tmp/ffmpeg && ./configure \
        --prefix=/usr/local \
        --enable-gpl --enable-version3 \
        --enable-shared --disable-static \
        --disable-doc --disable-programs \
        --enable-libx264 --enable-libx265 --enable-libsoxr \
        --enable-openssl \
        --enable-ffnvcodec --enable-nvdec --enable-nvenc \
        --enable-cuda-nvcc \
        --nvccflags="-gencode arch=compute_75,code=sm_75 -O2" \
        --extra-cflags="-I/usr/local/cuda/include -I/usr/local/include" \
        --extra-ldflags="-L/usr/local/cuda/lib64 -L/usr/local/lib" \
    && make -j"$(nproc)" && make install \
    && cp -aL /usr/local/cuda/lib64/libcudart.so* /usr/local/lib/ \
    && ldconfig

COPY . /src
COPY --from=webui /web/dist /src/web/dist
RUN cmake -S /src -B /src/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="/opt/mxl;/tmp/mxl/build/vcpkg_installed/x64-linux;/usr/local" \
        -DSRTGW_WITH_NMOS=ON -DNMOS_CPP_DIR=/opt/nmos-cpp/Development \
        -DSRTGW_USE_PREBUILT_WEBUI=ON \
    && cmake --build /src/build -j"$(nproc)" \
    && /src/build/unit-tests

FROM ubuntu:24.04 AS runtime
ARG MXL_REF=218ddaa0a08c12ffe75fc475ae65aa3d9eef16d7
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates curl \
        libcpprest2.10 libboost-chrono1.83.0t64 libboost-thread1.83.0 \
        libssl3t64 libavahi-compat-libdnssd1 \
        libx264-164 libx265-199 libsoxr0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /opt/mxl/lib/ /usr/local/lib/
COPY --from=build /tmp/mxl/build/vcpkg_installed/x64-linux/lib/ /usr/local/lib/
COPY --from=build /usr/local/lib/ /usr/local/lib/
COPY --from=build /src/build/mxl-srt-gateway /usr/local/bin/mxl-srt-gateway
COPY --from=build /src/build/mxl-srt-flow-writer /usr/local/bin/mxl-srt-flow-writer
COPY --from=build /src/build/mxl-srt-flow-verify /usr/local/bin/mxl-srt-flow-verify
ENV NVIDIA_DRIVER_CAPABILITIES=compute,utility,video
LABEL io.dmf.mxl.revision="${MXL_REF}"
LABEL org.opencontainers.image.title="mxl-srt-gateway"
LABEL org.opencontainers.image.licenses="GPL-2.0-or-later"
RUN ldconfig \
    && if ! getent passwd 1000 >/dev/null; then \
         useradd --uid 1000 --no-create-home --shell /usr/sbin/nologin srtgw; \
       fi
USER 1000:1000
EXPOSE 8120/tcp 3272/tcp 3273/tcp
ENTRYPOINT ["/usr/local/bin/mxl-srt-gateway"]
