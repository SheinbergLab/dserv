## This dockerfile is used to build a container that runs the latest version of dserv
# It installs the latest version of dserv, dlsh, and ess, as well as dependencies like Tcl9
# It can be accessed as normal by sending TCP commands to the dserv port (2570) and git port (2573)
# e.g., {
#           echo "ess::load_system search" | nc -N 127.0.0.1 2570
#           echo "ess::get_system_status" | nc -N 127.0.0.1 2570
#       }
# echo "git::switch_and_pull ryan" | nc -N 127.0.0.1 2573

# To use entirely self-contained including task code,
# wget https://raw.githubusercontent.com/SheinbergLab/dserv/main/dockerfile
# docker build -t dserv-img:latest .
# docker run -p 2570:2570 -p 2573:2573 -it dserv-img:latest

# alternatively, if you want the task code on the host system directly,
# wget https://raw.githubusercontent.com/SheinbergLab/dserv/main/dockerfile
# git clone https://github.com/homebase-sheinberg/ess.git
# docker build -t dserv-img:latest .
# docker run -p 2570:2570 -p 2573:2573 -v /home/lab/docker_dserv/ess:/home/lab/ess -it dserv-img:latest

# Use latest Debian image (similar base to Raspberry Pi OS)
FROM debian:bookworm

# Set non-interactive mode for apt
ENV DEBIAN_FRONTEND=noninteractive

# Install build-time and run-time dependencies
RUN apt-get update && \
    apt-get full-upgrade -y && \
    apt-get install -y --no-install-recommends \
        build-essential cmake libevdev-dev libpq-dev \
        git wget ca-certificates pkg-config jq \
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
RUN DLSH_URL=$(wget -qO- https://api.github.com/repos/SheinbergLab/dlsh/releases \
    | jq -r '.[0].assets[] | select(.name == "dlsh.zip") | .browser_download_url') && \
    if [ -z "$DLSH_URL" ]; then \
        echo "ERROR: Could not find dlsh.zip download URL." >&2; \
        exit 1; \
    fi && \
    wget "$DLSH_URL" -O /tmp/dlsh.zip && \
    mkdir -p /usr/local/dlsh && \
    cp /tmp/dlsh.zip /usr/local/dlsh/ && \
    rm /tmp/dlsh.zip && \
    # Configure dserv
    cp /usr/local/dserv/local/post-pins.tcl.EXAMPLE /usr/local/dserv/local/post-pins.tcl && \
    sed -i '/^\\s*gpio/s/^/#/' /usr/local/dserv/local/post-pins.tcl && \
    # Install and configure ess
    mkdir -p /home/lab && \
    git clone https://github.com/homebase-sheinberg/ess.git /home/lab/ess && \
    git config --system --add safe.directory /home/lab/ess && \
    cp /usr/local/dserv/local/pre-systemdir.tcl.EXAMPLE /usr/local/dserv/local/pre-systemdir.tcl && \
    echo 'set env(ESS_SYSTEM_PATH) /home/lab/' >> /usr/local/dserv/local/pre-systemdir.tcl && \
    # Allow any user to write to the application and data directories
    chmod -R 777 /usr/local/dserv /home/lab

# Configure the entrypoint
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]

# Default command to start dserv in the background and open a shell
CMD ["/bin/bash", "-c", "/usr/local/dserv/dserv -c /usr/local/dserv/config/dsconf.tcl -t /usr/local/dserv/config/triggers.tcl & exec bash"]
