# The container this repository defines for its own build.
# It installs a runner; the floor that runner must meet is declared by the workspace.
FROM docker.io/library/gcc:16-trixie
ARG JUST_VERSION=1.58.0
# The headers the generated definitions compile against.
RUN apt-get update && apt-get install -y --no-install-recommends libprotobuf-c-dev && rm -rf /var/lib/apt/lists/*
RUN curl -fsSL https://just.systems/install.sh | bash -s -- --tag "${JUST_VERSION}" --to /usr/local/bin
WORKDIR /src
COPY . .
