#!/bin/sh
# init-box-net.sh -- put the Mac's USB Ethernet adapter on the wiznet-io box
# subnet. The box is static 192.168.11.2; this sets the Mac to 192.168.11.10/24.
#
# The USB adapter's interface name (enX) varies by hub, and macOS keeps
# reverting its manual IP, so run this at the start of each bench session:
#
#     sudo sh init-box-net.sh            # auto-detect the USB LAN interface
#     sudo sh init-box-net.sh en7        # or force a specific interface
#
# Then verify:  route -n get 192.168.11.2   should show "interface: enX".

BOX_NET=192.168.11
MAC_IP=$BOX_NET.10

IFACE="$1"
if [ -z "$IFACE" ]; then
  # first hardware port whose name contains "LAN" (USB Ethernet adapters)
  IFACE=$(networksetup -listallhardwareports \
          | awk '/Hardware Port:.*LAN/{getline; print $2; exit}')
fi
if [ -z "$IFACE" ]; then
  echo "No USB LAN interface found. Plug in the adapter, or pass one explicitly:"
  echo "  sudo sh $0 en7"
  echo "Candidates:"; networksetup -listallhardwareports | grep -A1 -i 'LAN\|ethernet'
  exit 1
fi

echo "Setting $IFACE -> $MAC_IP/24"
ifconfig "$IFACE" inet $MAC_IP netmask 255.255.255.0 || {
  echo "failed (run with sudo)"; exit 1; }

R=$(route -n get $BOX_NET.2 2>/dev/null | awk '/interface:/{print $2}')
echo "route to $BOX_NET.2 -> interface: ${R:-none}"
if [ "$R" = "$IFACE" ]; then
  echo "OK: Mac can reach the box on $IFACE"
else
  echo "WARN: route to the box is via '$R', not '$IFACE'."
  echo "      Another interface may also be on $BOX_NET.x, or the adapter is elsewhere."
fi
