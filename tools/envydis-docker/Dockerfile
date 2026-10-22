# Builds envydis (envytools, MIT) for Maxwell (gm107) SASS disassembly.
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      git ca-certificates build-essential cmake flex bison libxml2-dev pkg-config python3 \
    && rm -rf /var/lib/apt/lists/*
RUN git clone https://github.com/envytools/envytools /src && cd /src && git checkout f102b823 \
    && mkdir build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j"$(nproc)" envydis \
    && cp envydis/envydis /usr/local/bin/
ENTRYPOINT ["envydis"]
