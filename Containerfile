# The container this repository defines for its own build.
# It installs a runner; the floor that runner must meet is declared by the workspace.
FROM docker.io/library/gcc:16-trixie
ARG JUST_VERSION=1.58.0
RUN curl -fsSL https://just.systems/install.sh | bash -s -- --tag "${JUST_VERSION}" --to /usr/local/bin
WORKDIR /src
COPY . .
