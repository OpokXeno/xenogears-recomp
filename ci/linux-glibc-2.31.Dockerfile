FROM docker.io/library/python:3.11-slim-bullseye AS base

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils build-essential bzip2 ca-certificates curl git libgl1-mesa-dev \
        libsdl2-dev libxtst-dev ninja-build pkg-config tar xvfb xz-utils \
    && python -m pip install --no-cache-dir "cmake>=3.20,<4" \
    && test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.31" \
    && cmake --version \
    && rm -rf /var/lib/apt/lists/*

# Vulkan headers and glslc for the runtime's Vulkan backend (runtime.cmake),
# built from pinned tags because bullseye ships neither.
FROM base AS vulkan-tools
COPY provision-vulkan-tools.sh /xgr-ci/provision-vulkan-tools.sh
RUN bash /xgr-ci/provision-vulkan-tools.sh /opt/vulkan \
    && rm -rf /xgr-ci

FROM base
COPY --from=vulkan-tools /opt/vulkan /opt/vulkan
ENV VULKAN_SDK=/opt/vulkan
