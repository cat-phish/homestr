# syntax=docker/dockerfile:1

# ---------------------------------------------------------------------------
# toolchain -- compiler, CMake and the MariaDB Connector/C++.
# The connector is not packaged for Debian, so it is built from source here and
# cached as its own layer: the application stages below never rebuild it.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS toolchain

ARG MARIADB_CPP_VERSION=1.1.5
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install --no-install-recommends -y \
      build-essential \
      ca-certificates \
      cmake \
      git \
      libasio-dev \
      libmariadb-dev \
      libssl-dev \
      ninja-build \
      pkg-config \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --recurse-submodules --shallow-submodules \
      --branch "${MARIADB_CPP_VERSION}" \
      https://github.com/mariadb-corporation/mariadb-connector-cpp.git /tmp/conncpp \
    && cmake -S /tmp/conncpp -B /tmp/conncpp/build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /tmp/conncpp/build \
    && cmake --install /tmp/conncpp/build \
    && ldconfig \
    && rm -rf /tmp/conncpp

# ---------------------------------------------------------------------------
# dev -- target used by docker-compose. Sources are bind-mounted, so the image
# carries only the toolchain; the entrypoint configures, builds and runs.
# ---------------------------------------------------------------------------
FROM toolchain AS dev

# Formatter and static analysis live in the dev image only, so the cached
# toolchain layer stays shared with the release build.
#
# The clang-format major version is pinned because successive releases format
# the same file differently; CI installs the same one. Bumping it means
# reformatting the tree in one commit.
RUN apt-get update && apt-get install --no-install-recommends -y \
      clang-format-16 \
      clang-tidy-16 \
      gdb \
    && rm -rf /var/lib/apt/lists/* \
    && ln -s /usr/bin/clang-format-16 /usr/local/bin/clang-format \
    && ln -s /usr/bin/clang-tidy-16 /usr/local/bin/clang-tidy

WORKDIR /app
COPY scripts/dev-entrypoint.sh /usr/local/bin/dev-entrypoint.sh
RUN chmod +x /usr/local/bin/dev-entrypoint.sh

EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/dev-entrypoint.sh"]

# ---------------------------------------------------------------------------
# builder -- release build plus the GoogleTest suites.
# ---------------------------------------------------------------------------
FROM toolchain AS builder

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DHOMESTR_BUILD_TESTS=ON \
    && cmake --build build

# Fails the image build when a test fails.
RUN ctest --test-dir build --output-on-failure

# ---------------------------------------------------------------------------
# runtime -- minimal image with just the server and its shared libraries.
# ---------------------------------------------------------------------------
FROM debian:bookworm-slim AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install --no-install-recommends -y \
      ca-certificates \
      libmariadb3 \
      libssl3 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --shell /usr/sbin/nologin homestr

COPY --from=builder /usr/local/lib/mariadb/ /usr/local/lib/mariadb/
COPY --from=builder /src/build/homestr_server /usr/local/bin/homestr_server

RUN printf '/usr/local/lib/mariadb\n' > /etc/ld.so.conf.d/mariadb-cpp.conf && ldconfig

USER homestr
EXPOSE 8080
ENV HOMESTR_HTTP_PORT=8080

ENTRYPOINT ["/usr/local/bin/homestr_server"]
