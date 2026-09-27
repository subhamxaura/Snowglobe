#!/bin/sh
# unix_sockets: AF_UNIX listener+connector on a filesystem path and on a
# fixed abstract name, plus a datagram sendto with destination. No network.
# The abstract name is fixed (no pid) so the golden is stable.
export PATH=/usr/bin:/bin
export PYTHONHASHSEED=0
export PYTHONDONTWRITEBYTECODE=1
DIR=$(dirname "$0")
D=$(mktemp -d)
# -S skips site imports (.pth, sitecustomize, dist-packages): their file sets
# differ per distro release, so without -S the golden would be version-bound.
python3 -B -S "$DIR/sock.py" "$D"
