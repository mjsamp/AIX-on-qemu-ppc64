#!/bin/bash
BRIDGE=virbr1
WIRELESS=wlp5s0
TAP=tap0
TAP1=tap1
NETWORK=192.168.123.0
NETMASK=255.255.255.0
GATEWAY=192.168.123.1
DHCPRANGE=192.168.123.2,192.168.123.254

ip link add $BRIDGE type bridge
ip link set dev $BRIDGE up
ip addr add dev $BRIDGE $GATEWAY/$NETMASK

ip tuntap add $TAP mode tap
ip link set $TAP master $BRIDGE
ip link set up dev $TAP

ip tuntap add $TAP1 mode tap
ip link set $TAP1 master $BRIDGE
ip link set up dev $TAP1

sysctl -w net.ipv4.ip_forward=1 > /dev/null 2>&1

iptables --flush
iptables -t nat -F
iptables -X
iptables -Z
#iptables -P PREROUTING ACCEPT
#iptables -P POSTROUTING ACCEPT
iptables -P OUTPUT ACCEPT
iptables -P INPUT ACCEPT
iptables -P FORWARD ACCEPT
iptables -A INPUT -i $BRIDGE -p tcp -m tcp --dport 67 -j ACCEPT
iptables -A INPUT -i $BRIDGE -p udp -m udp --dport 67 -j ACCEPT
iptables -A INPUT -i $BRIDGE -p tcp -m tcp --dport 53 -j ACCEPT
iptables -A INPUT -i $BRIDGE -p udp -m udp --dport 53 -j ACCEPT
iptables -A FORWARD -i $BRIDGE -o $BRIDGE -j ACCEPT
iptables -A FORWARD -s $NETWORK/$NETMASK -i $BRIDGE -j ACCEPT
iptables -A FORWARD -d $NETWORK/$NETMASK -o $BRIDGE -m state --state RELATED,ESTABLISHED -j ACCEPT
# make a distinction between the bridge packets and routed packets, don't want the
# bridged frames/packets to be masqueraded.
iptables -t nat -A POSTROUTING -s $NETWORK/$NETMASK -d $NETWORK/$NETMASK -j ACCEPT
iptables -t nat -A POSTROUTING -s $NETWORK/$NETMASK -j MASQUERADE

dns_cmd=(
    dnsmasq
    --strict-order
    --except-interface=lo
    --interface=$BRIDGE
    --listen-address=$GATEWAY
    --bind-interfaces
    --dhcp-range=$DHCPRANGE
    --conf-file=""
    --pid-file=/var/run/qemu-dnsmasq-$BRIDGE.pid
    --dhcp-leasefile=/var/run/qemu-dnsmasq-$BRIDGE.leases
    --dhcp-no-override
)

echo ${dns_cmd[@]} | bash

# set the traffic get through the wireless
iptables -A FORWARD -i $BRIDGE -o $WIRELESS -j ACCEPT
iptables -t nat -A POSTROUTING -o $WIRELESS -j MASQUERADE
# let the known traffic get back at bridge
iptables -A FORWARD -i $WIRELESS -o $BRIDGE -m state --state RELATED,ESTABLISHED -j ACCEPT
