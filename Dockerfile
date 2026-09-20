# 多阶段构建：builder 编译后端 + 前端，runtime 只保留部署产物
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN ln -fs /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libsqlite3-dev libspdlog-dev libfmt-dev libjsoncpp-dev nlohmann-json3-dev \
        zlib1g-dev libssl-dev uuid-dev libgif-dev libpng-dev \
        nodejs npm \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 仓库没有 drogon 包，从源码编译安装（静态库，OpenSSL 也用静态版）
ARG DROGON_VERSION=v1.9.10
RUN git clone --depth 1 --branch ${DROGON_VERSION} --recurse-submodules --shallow-submodules \
        https://github.com/drogonframework/drogon.git /tmp/drogon \
    && cmake -S /tmp/drogon -B /tmp/drogon/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DOPENSSL_USE_STATIC_LIBS=ON \
        -DBUILD_EXAMPLES=OFF \
        -DBUILD_CTL=OFF \
        -DBUILD_ORM=OFF \
        -DMYSQL_SUPPORT=OFF \
        -DLIBPQ_SUPPORT=OFF \
    && cmake --build /tmp/drogon/build \
    && cmake --install /tmp/drogon/build \
    && rm -rf /tmp/drogon

WORKDIR /src
COPY . .

# 构建后端；CMake 会自动触发前端 npm install + build
# 产物输出到 build/insoulforge/（exe/、public/）
RUN cmake -S . -B build-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build-cmake -j"$(nproc)"

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive TZ=Asia/Shanghai
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates tzdata libsqlite3-0 libjsoncpp25 libssl3t64 libpng16-16 libgif7 \
    && rm -rf /var/lib/apt/lists/* \
    && ln -fs /usr/share/zoneinfo/Asia/Shanghai /etc/localtime

WORKDIR /app
COPY --from=builder /src/build/insoulforge/ .
COPY --from=builder /src/agentTools ./agentTools

EXPOSE 7778

VOLUME ["/app/data", "/app/logs", "/app/uploads"]

ENTRYPOINT ["./exe/insoulforge"]
