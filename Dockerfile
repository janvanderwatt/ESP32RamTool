# Reproducible build for the RamTool RAM payload.
#
# Two things this exists to prove:
#
#   1. The repository has no machine-specific dependencies left in it. A green
#      build of this image means build.ps1 found everything it needed from the
#      environment alone.
#   2. Anyone can reproduce the build without installing an ESP-IDF toolchain.
#
# The payload is built with the same build.ps1 the host uses, so this is a real
# test of that script rather than a parallel one that could drift from it.
#
#   docker build -t ramtool .
#   docker run --rm ramtool
#
# The container pins ESP-IDF v5.5.4, which ships the xtensa-esp-elf 14.2.0
# toolchain this project was developed against.

FROM espressif/idf:v5.5.4 AS build

# PowerShell so the real build.ps1 can run unmodified. The self-contained Linux
# tarball avoids needing the Microsoft apt repository.
ARG PWSH_VERSION=7.4.6
ARG PWSH_URL=https://github.com/PowerShell/PowerShell/releases/download/v${PWSH_VERSION}/powershell-${PWSH_VERSION}-linux-x64.tar.gz

USER root
RUN apt-get update \
 && apt-get install -y --no-install-recommends curl ca-certificates \
 && curl -fsSL "$PWSH_URL" -o /tmp/pwsh.tar.gz \
 && mkdir -p /opt/microsoft/powershell/7 \
 && tar zxf /tmp/pwsh.tar.gz -C /opt/microsoft/powershell/7 \
 && chmod +x /opt/microsoft/powershell/7/pwsh \
 && ln -s /opt/microsoft/powershell/7/pwsh /usr/bin/pwsh \
 && rm /tmp/pwsh.tar.gz \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

# No esptool here on purpose: this stage only runs build.ps1, which needs the
# toolchain; ROM addresses are pinned in /src/rom/esp32.ld. esptool is a runtime
# concern and is installed in the final stage.

# IDF_TOOLS_PATH already points at the installed toolchain in this image.
# build.ps1 uses it for compiler discovery; it does not read IDF_PATH or any
# installed SDK linker scripts.
ENV PATH="/opt/esp/idf/tools:${PATH}"

WORKDIR /src
COPY . /src

# Build here, at image build time, so a successful build is the real signal.
RUN pwsh -NoProfile -File ./build.ps1

# Fail loudly if the artifacts are not where the host script expects them.
# manifest.txt matters as much as the binaries: without it run.py has no way to
# know the load addresses unless it can also parse the ELF.
RUN test -f build/iram.bin && test -f build/dram.bin && test -f build/ramtool.elf \
 && test -f build/manifest.txt \
 && cat build/manifest.txt \
 && ls -l build/iram.bin build/dram.bin build/manifest.txt


# ---------------------------------------------------------------------------

FROM debian:bookworm-slim

# run.py needs python3 and pyserial. It talks to a serial device, so the runtime
# image carries no toolchain at all -- only what is needed to drive the chip.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        python3 python3-pip python-is-python3 \
 && pip3 install --no-cache-dir --break-system-packages "esptool>=4.8" \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY --from=build /src/build/iram.bin   build/iram.bin
COPY --from=build /src/build/dram.bin   build/dram.bin
COPY --from=build /src/build/manifest.txt build/manifest.txt
COPY --from=build /src/build/ramtool.elf build/ramtool.elf
COPY run.py run.py

# The notes travel with the image, so they are readable from it without a clone.
COPY --from=build /src/docs        docs/
COPY --from=build /src/rom         rom/
COPY --from=build /src/README.md   README.md
COPY --from=build /src/LICENSE     LICENSE
COPY --from=build /src/littlefs/ORIGIN.md    littlefs/ORIGIN.md
COPY --from=build /src/littlefs/LICENSE.md    littlefs/LICENSE.md
COPY --from=build /src/fatfs/ORIGIN.md        fatfs/ORIGIN.md
COPY --from=build /src/fatfs/LICENSE.txt      fatfs/LICENSE.txt

# No CMD: the image is a vehicle for either
#   docker run --rm ramtool ls -l build
# or, with a device passed through,
#   docker run --rm -it --device /dev/ttyUSB0 ramtool python3 run.py --port /dev/ttyUSB0
