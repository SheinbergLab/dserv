#!/bin/sh
set -e

# Turn the installed dserv into a running, boot-surviving service.
#
# The postinst now places /etc/systemd/system/dserv.service itself (absent only,
# never clobbering), so this script is no longer how the unit gets there -- it
# is the "and actually start it" half, which the package deliberately leaves to
# a human. The copy stays for a box whose dserv predates that change, and it too
# refuses to overwrite an existing unit: site edits live there.
if [ ! -e /etc/systemd/system/dserv.service ]; then
    cp /usr/local/dserv/systemd/dserv.service /etc/systemd/system/dserv.service
fi

systemctl daemon-reload
systemctl enable dserv
systemctl start dserv
