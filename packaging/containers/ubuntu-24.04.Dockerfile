FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
ARG NFPM_VERSION=2.43.1

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        g++ \
        git \
        libadwaita-1-dev \
        libgtk-4-dev \
        libjson-glib-dev \
        libwebkitgtk-6.0-dev \
        meson \
        ninja-build \
        pkg-config \
        xz-utils \
    && curl -fsSL "https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION}/nfpm_${NFPM_VERSION}_Linux_x86_64.tar.gz" \
        | tar -xz -C /usr/local/bin nfpm \
    && chmod +x /usr/local/bin/nfpm \
    && rm -rf /var/lib/apt/lists/*
