FROM fedora:42

ARG NFPM_VERSION=2.43.1

RUN dnf install -y \
        ca-certificates \
        curl \
        gcc-c++ \
        git \
        gtk4-devel \
        libadwaita-devel \
        json-glib-devel \
        meson \
        ninja-build \
        tar \
        webkitgtk6.0-devel \
        which \
    && curl -fsSL "https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION}/nfpm_${NFPM_VERSION}_Linux_x86_64.tar.gz" \
        | tar -xz -C /usr/local/bin nfpm \
    && chmod +x /usr/local/bin/nfpm \
    && dnf clean all
