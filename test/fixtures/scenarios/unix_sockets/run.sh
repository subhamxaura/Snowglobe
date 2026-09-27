#!/bin/sh
# unix_sockets: C helper for AF_UNIX checks (see sg_sockets.c). No network,
# no interpreter (same rationale as threads/run.sh).
export PATH=/usr/bin:/bin
D=$(mktemp -d)
H=${SG_HELPERS:?sg helpers missing}
"$H/sg_sockets" "$D"
