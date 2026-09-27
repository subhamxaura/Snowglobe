#!/bin/sh
# tcp_loopback: C helper for TCP checks (see sg_tcp.c). No network beyond
# one unanswered TEST-NET SYN and loopback traffic.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
H=${SG_HELPERS:?sg helpers missing}
"$H/sg_tcp" "$D"
