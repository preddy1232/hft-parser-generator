# ==============================================================================
# Dockerfile — HFT C++20 Build Environment
# ==============================================================================
FROM ubuntu:24.04

# Prevent interactive prompts during apt installations
ENV DEBIAN_FRONTEND=noninteractive

# Install essential C++ build tools, CMake, Google Test, and Google Benchmark
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    g++-13 \
    clang-18 \
    libgtest-dev \
    libbenchmark-dev \
    python3 \
    linux-tools-common \
    linux-tools-generic \
    valgrind \
    && rm -rf /var/lib/apt/lists/*

# Set GCC 13 as default
RUN update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

WORKDIR /app
CMD ["/bin/bash"]
