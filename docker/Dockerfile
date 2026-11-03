# Ambiente de build do Pingo OS (isola o Buildroot do toolchain do host)
FROM debian:trixie-slim
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential bc bison flex file cpio unzip rsync wget curl ca-certificates \
      git python3 python3-dev python3-setuptools perl libncurses-dev libssl-dev \
      xz-utils bzip2 gzip patch which locales swig \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
