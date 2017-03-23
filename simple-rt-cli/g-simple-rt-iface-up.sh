#!/bin/bash
# SimpleRT: Reverse tethering utility for Android
# Copyright (C) 2016-2017 Konstantin Menyaev
# Copyright (C) 2017 Aleksander Morgado <aleksander@aleksander.es>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.

#params from simple-rt-cli

[ $# -ge 6 ] || {
    echo "error: missing arguments"
    exit 1
}

PLATFORM=$1
TUN_DEV=$2
LOCAL_INTERFACE=$3
TUNNEL_NET=$4
TUNNEL_CIDR=$5
HOST_ADDR=$6

LOGGER="$(which logger)"
[ -n "${LOGGER}" ] || exit 1

IPTABLES="$(which iptables)"
[ -n "${IPTABLES}" ] || exit 1

IPROUTE2="$(which ip)"
[ -n "${IPROUTE2}" ] || exit 1

SYSCTL="$(which sysctl)"
[ -n "${SYSCTL}" ] || exit 1

FLOCK="$(which flock)"
[ -n "${FLOCK}" ] || exit 1

# Only allow Linux platform here
[ "$PLATFORM" = "linux" ] || exit 2

# Get exclusive lock so that we don't collide with other threads trying to set
# this up at the same time
(
    ${FLOCK} -x --timeout=30 200 || exit 3

    ${LOGGER} -s -t "g-simple-rt" "configured ${TUN_DEV} as ${HOST_ADDR}/${TUNNEL_CIDR}"
    ${IPROUTE2} addr add $HOST_ADDR/$TUNNEL_CIDR dev $TUN_DEV
    ${IPROUTE2} link set dev $TUN_DEV up

    # Enable IPv4 forwarding if not already done before
    if [ $(${SYSCTL} -n net.ipv4.ip_forward) -eq 0 ]; then
        ${LOGGER} -s -t "g-simple-rt" "enabled IPv4 forwarding"
        ${SYSCTL} -w net.ipv4.ip_forward=1
    fi

    # Don't create multiple rules of the same type (e.g. when tethering more
    # than one device)
    ${IPTABLES} -w -C FORWARD -j ACCEPT > /dev/null 2>&1
    if [ $? -ne 0 ]; then
        ${LOGGER} -s -t "g-simple-rt" "enabled forwarding in iptables rules"
        ${IPTABLES} -w -I FORWARD -j ACCEPT
    fi

    ${LOGGER} -s -t "g-simple-rt" "enabled forwarding ${TUNNEL_NET}/${TUNNEL_CIDR} --> ${LOCAL_INTERFACE}"
    ${IPTABLES} -w -t nat -I POSTROUTING -s ${TUNNEL_NET}/${TUNNEL_CIDR} -o $LOCAL_INTERFACE -j MASQUERADE

) 200>/var/lock/g-simple-rt-iface-up

exit 0
