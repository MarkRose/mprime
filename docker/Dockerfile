# Builds mprime from a checkout of this repository and packages it to run in Kubernetes.
#
# The source tree to build is expected at ./src (the build context is the repository
# root, so this Dockerfile can come from main while ./src is any branch or commit):
#
#   git worktree add src p1-proxy
#   podman build -f docker/Dockerfile -t mprime:p1-proxy .
#
# Both stages use the same Ubuntu release as the CI build, so the runtime libraries
# are the ones mprime was linked against.

FROM ubuntu:24.04 AS build

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        libboost-dev \
        libcurl4-openssl-dev \
        libgmp-dev \
        libhwloc-dev \
    && rm -rf /var/lib/apt/lists/*

COPY src/ /src/
WORKDIR /src

# Same recipe as .github/workflows/linux.yml: gwnum first, then mprime with hwloc and
# curl linked dynamically.  The -Wl,-M link map is dropped to keep the build log short.
RUN make -C gwnum -f make64 \
    && make -C linux64 \
        LFLAGS="-Wl,-L/usr/local/lib" \
        LIBS="../gwnum/gwnum.a ../gwnum/polymult.a -lm -lpthread -lhwloc -lcurl -lrt -lstdc++ -ldl -lgmp" \
    && test -x linux64/mprime


FROM ubuntu:24.04

# The program lives in /opt/mprime (read-only to the mprime user); everything it reads
# and writes goes in /mnt/mprime, which Kubernetes mounts as a persistent volume.
# Ubuntu's base image already has a uid 1000 user, so remove it first.
RUN DEBIAN_FRONTEND=noninteractive apt-get update && apt-get -y install --no-install-recommends \
        ca-certificates \
        libcurl4t64 \
        libgmp10 \
        libhwloc15 \
        libstdc++6 \
    && rm -rf /var/lib/apt/lists/* \
    && userdel -r ubuntu \
    && groupadd --gid 1000 mprime \
    && useradd --home-dir /opt/mprime --shell /bin/sh --uid 1000 --gid 1000 mprime \
    && mkdir -p /opt/mprime /mnt/mprime \
    && chown mprime:mprime /mnt/mprime

COPY --from=build /src/linux64/mprime /opt/mprime/mprime
# The Prime95 license travels with the binary.
COPY --from=build /src/license.txt /src/readme.txt /src/undoc.txt /src/stress.txt /opt/mprime/

ARG SOURCE_REPO=https://github.com/MarkRose/mprime
ARG SOURCE_REF=unknown
ARG SOURCE_SHA=unknown
LABEL org.opencontainers.image.title="mprime" \
      org.opencontainers.image.description="mprime built from source, for running GIMPS work in Kubernetes. Needs configuration from an init container." \
      org.opencontainers.image.source="${SOURCE_REPO}" \
      org.opencontainers.image.revision="${SOURCE_SHA}" \
      org.opencontainers.image.version="${SOURCE_REF}" \
      org.opencontainers.image.licenses="LicenseRef-Prime95"

ENV PATH=/opt/mprime:$PATH
USER mprime
WORKDIR /mnt/mprime

# -d: log to stdout (kubectl logs).  -w: working directory, which must be a persistent volume.
# mprime writes a checkpoint when it gets SIGTERM, so give pods a termination grace period.
CMD ["/opt/mprime/mprime", "-d", "-w/mnt/mprime"]
