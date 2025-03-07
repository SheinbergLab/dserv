#!/bin/sh
set -e
cp /usr/local/dserv/systemd/dserv.service /etc/systemd/system
systemctl daemon-reload
systemctl enable dserv
systemctl start dserv
