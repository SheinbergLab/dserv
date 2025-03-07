# This runs in an Ubuntu environment.
# Use 22.04 for compatibility (eg version of stdlibc++)
FROM ubuntu:22.04

# Set up a build environment.
RUN export DEBIAN_FRONTEND=noninteractive \
    && apt update \
    && apt install --no-install-recommends --yes git wget ca-certificates build-essential cmake libevdev-dev zlib1g-dev \
    && apt-get clean \
    && apt-get autoremove \
    && rm -rf /var/lib/apt/listhttps://github.com/benjamin-heasly/dlsh/releases/download/initial/dlsh.zips/*

# Install TCL 9 to the build environment.
WORKDIR /work
RUN git clone --depth=1 --branch core-9-0-1-rc https://github.com/tcltk/tcl.git \
  && cd tcl/unix/ \
  && ./configure \
  && make \
  && make install

# Install jansson in the build environment.
WORKDIR /work
RUN git clone https://github.com/akheron/jansson.git \
  && cd jansson/ \
  && git checkout v2.14 \
  && mkdir build \
  && cd build/ \
  && cmake .. \
  && make \
  && make install

# Install dserv itself to the packaging directory.
WORKDIR /work
RUN git clone https://github.com/benjamin-heasly/dserv.git \
  && cd dserv \
  && git checkout ci-releases \
  && mkdir build \
  && cd build/ \
  && cmake -DCMAKE_INSTALL_PREFIX=/work/package/usr/local/ .. \
  && cmake --build . \
  && cmake --install .

# Also install TCL 9 to the packaging directory.
WORKDIR /work/tcl/unix/ \
&& ./configure --prefix=/work/package/usr/local/ \
&& make \
&& make install

# Install dlsh to the packaging directory.
WORKDIR /work/package/usr/local/dlsh
RUN wget https://github.com/benjamin-heasly/dlsh/releases/download/initial/dlsh.zip

# Add debian package metadata and service install script to the packaging directory.
WORKDIR /work/package/DEBIAN
RUN cp /work/dserv/dpkg/control . \
  && cp /work/dserv/dpkg/install-dserv-service.sh . \
  && chmod +x ./install-dserv-service.sh

# Dynamically add the package version and architecture, and build the package.
WORKDIR /work
RUN echo "Version: 0.0.test" >> /work/package/DEBIAN/control \
  && echo "Architecture: $(dpkg --print-architecture)" >> /work/package/DEBIAN/control \
  && dpkg-deb --build /work/package dserv-test.deb

# For testing, install the package we just made.
WORKDIR /work
RUN export DEBIAN_FRONTEND=noninteractive \
&& apt update \
&& apt install --no-install-recommends --yes ./dserv-test.deb \
&& apt-get clean \
&& apt-get autoremove \
&& rm -rf /var/lib/apt/lists/*

# docker build . -t dserv:local

# docker run --rm -ti dserv:local /bin/sh -c 'echo "test" | essctrl && cat history.txt'
# docker run --rm -ti dserv:local /usr/local/dserv/dserv --help
# timeout -k 3 2 docker run --rm -ti dserv:local /usr/local/dserv/dserv -c /usr/local/dserv/config/dsconf.tcl -t /usr/local/dserv/config/triggers.tcl
