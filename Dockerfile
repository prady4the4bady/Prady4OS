# PRADYOS reproducible build environment (DDR-841, Group 1 item 2).
#
# Pins the toolchain the project builds with. Build the image once, then build
# the OS inside it, and the compiler/linker/assembler versions stop depending on
# whatever the host happens to have installed.
#
# SCOPE — read before claiming more than this delivers: this pins the build
# ENVIRONMENT. It does NOT make the output bit-for-bit reproducible; that needs
# SOURCE_DATE_EPOCH handling and a deterministic link-order audit, which is
# separate work and is not claimed here.
#
#   docker build -t pradyos-build .
#   docker run --rm -v "$PWD":/src -w /src pradyos-build make image
#
# The base is pinned to the same distro the project already builds on
# (Ubuntu 24.04, per CLAUDE.md), so container and WSL builds agree.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# One layer, versions pinned by the distro's 24.04 snapshot. No PPAs: an
# unpinned third-party archive would reintroduce exactly the drift this image
# exists to remove.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        clang \
        lld \
        llvm \
        nasm \
        make \
        git \
        python3 \
        dosfstools \
        mtools \
        qemu-system-x86 \
        xorriso \
        grub-pc-bin \
        grub-efi-amd64-bin \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Record what we actually got, so a build log can be compared against another
# machine's without re-deriving it.
RUN clang --version | head -1 > /toolchain-versions.txt \
    && ld.lld --version >> /toolchain-versions.txt \
    && nasm -v >> /toolchain-versions.txt \
    && qemu-system-x86_64 --version | head -1 >> /toolchain-versions.txt \
    && cat /toolchain-versions.txt

WORKDIR /src
CMD ["make", "image"]
