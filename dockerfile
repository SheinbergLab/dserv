# Use latest Debian image (similar base to Raspberry Pi OS)
FROM debian:bookworm

# Set non-interactive mode for apt
ENV DEBIAN_FRONTEND=noninteractive

# Install build-time and run-time dependencies
RUN apt-get update && \
    apt-get full-upgrade -y && \
    apt-get install -y --no-install-recommends \
        build-essential cmake libevdev-dev libpq-dev \
        git wget ca-certificates pkg-config \
        tcl-dev libjansson-dev liblz4-dev libhpdf-dev libyajl2 \
    && apt-get clean && rm -rf /var/lib/apt/lists/*

# Clone, build, and install all components in a single layer.
# After installation, the source code is removed to keep the image small.
RUN git clone --recurse-submodules https://github.com/SheinbergLab/dserv.git /root/code/dserv && \
    # Build Tcl
    cd /root/code/dserv/deps/tcl/unix && \
    ./configure && make -j$(nproc) && make install && ldconfig && \
    # Build jansson
    cd /root/code/dserv/deps/jansson && \
    mkdir -p build && cd build && cmake .. && make && make install && \
    # Build dserv
    cd /root/code/dserv && \
    mkdir -p build && cd build && cmake .. -DCMAKE_INSTALL_PREFIX=/usr/local && make && make install && ldconfig && \
    # Clean up source code
    rm -rf /root/code

# Install dlsh and configure dserv/ess in a single layer.
RUN wget https://github.com/SheinbergLab/dlsh/releases/download/0.9.6/dlsh.zip -P /tmp && \
    mkdir -p /usr/local/dlsh && \
    cp /tmp/dlsh.zip /usr/local/dlsh/ && \
    rm /tmp/dlsh.zip && \
    # Configure dserv
    cp /usr/local/dserv/local/post-pins.tcl.EXAMPLE /usr/local/dserv/local/post-pins.tcl && \
    sed -i '/gpio/s/^/#/' /usr/local/dserv/local/post-pins.tcl && \
    # Install and configure ess
    mkdir -p /home/lab && \
    git clone https://github.com/homebase-sheinberg/ess.git /home/lab/ess && \
    git config --system --add safe.directory /home/lab/ess && \
    cp /usr/local/dserv/local/pre-systemdir.tcl.EXAMPLE /usr/local/dserv/local/pre-systemdir.tcl && \
    echo 'set env(ESS_SYSTEM_PATH) /home/lab/' >> /usr/local/dserv/local/pre-systemdir.tcl

# Default command to start dserv in the background and open a shell
CMD ["/bin/bash", "-c", "/usr/local/dserv/dserv -c /usr/local/dserv/config/dsconf.tcl -t /usr/local/dserv/config/triggers.tcl & exec bash"]
