# 多阶段构建：builder 编译后端 + 前端，runtime 只保留部署产物
FROM ubuntu:26.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN ln -fs /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ninja-build git curl ca-certificates libarchive-tools \
        libsqlite3-dev \
        zlib1g-dev libssl-dev libc-ares-dev uuid-dev libgif-dev libpng-dev libjpeg-dev \
        nodejs npm \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
RUN cmake_version=4.3.6 \
    && case "$TARGETARCH" in \
        amd64) \
            cmake_arch=x86_64; \
            cmake_sha256=fba967b44c7b40f155f9492dacb5477aab8333699986e1e07a7d90803aba7219 ;; \
        arm64) \
            cmake_arch=aarch64; \
            cmake_sha256=310df7369f818779e269d900ae5a3ef02c6089fb7b1a49fc1a2aaf7e7275a2dc ;; \
        *) echo "Unsupported CMake architecture: $TARGETARCH" >&2; exit 1 ;; \
    esac \
    && cmake_archive="cmake-${cmake_version}-linux-${cmake_arch}.tar.gz" \
    && curl -fsSLo /tmp/cmake.tar.gz "https://cmake.org/files/v${cmake_version%.*}/${cmake_archive}" \
    && printf '%s  /tmp/cmake.tar.gz\n' "$cmake_sha256" | sha256sum -c - \
    && mkdir -p /opt/cmake \
    && bsdtar -xzf /tmp/cmake.tar.gz --strip-components=1 -C /opt/cmake \
    && rm /tmp/cmake.tar.gz
ENV PATH="/opt/cmake/bin:${PATH}"

WORKDIR /src
COPY . .

# 构建后端；CMake 会自动触发前端 npm install + build
# 产物输出到 build/insoulforge/（exe/、public/）
RUN cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DOPENSSL_USE_STATIC_LIBS=OFF \
    && cmake --build build-cmake -j"$(nproc)"

FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata libcares2 libsqlite3-0 libssl3t64 libpng16-16t64 libjpeg-turbo8 libgif7 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -fs /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

WORKDIR /app
COPY --from=builder /src/build/insoulforge/ .
COPY --from=builder /src/agentTools ./agentTools

EXPOSE 7778

VOLUME ["/app/data"]

ENTRYPOINT ["./exe/insoulforge"]
