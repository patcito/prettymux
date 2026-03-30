FROM archlinux:latest

ARG NFPM_VERSION=2.43.1

RUN pacman -Syu --noconfirm \
    && pacman -S --noconfirm \
        base-devel \
        ca-certificates \
        curl \
        gcc \
        git \
        gtk4 \
        libadwaita \
        json-glib \
        meson \
        ninja \
        tar \
        webkitgtk-6.0 \
        zstd \
    && curl -fsSL "https://github.com/goreleaser/nfpm/releases/download/v${NFPM_VERSION}/nfpm_${NFPM_VERSION}_Linux_x86_64.tar.gz" \
        | tar -xz -C /usr/local/bin nfpm \
    && chmod +x /usr/local/bin/nfpm \
    && pacman -Scc --noconfirm
