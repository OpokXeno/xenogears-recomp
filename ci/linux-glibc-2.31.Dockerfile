FROM docker.io/library/python:3.11-slim-bullseye

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils build-essential ca-certificates git libgl1-mesa-dev \
        libsdl2-dev ninja-build pkg-config tar xvfb xz-utils \
    && python -m pip install --no-cache-dir "cmake>=3.20,<4" \
    && test "$(getconf GNU_LIBC_VERSION)" = "glibc 2.31" \
    && cmake --version \
    && rm -rf /var/lib/apt/lists/*
