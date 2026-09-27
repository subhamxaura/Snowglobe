#!/bin/sh
# exec_chain: sh -> python3 -> sh -> ls. Fixed PATH keeps execvp search
# deterministic (/usr/bin first hit everywhere); ls runs on an empty dir so
# its stdout is empty. $D in argv normalises to $TMP.
export PATH=/usr/bin:/bin
export PYTHONHASHSEED=0
export PYTHONDONTWRITEBYTECODE=1
DIR=$(dirname "$0")
D=$(mktemp -d)
# -S skips site imports (.pth, sitecustomize, dist-packages): their file sets
# differ per distro release, so without -S the golden would be version-bound.
python3 -B -S "$DIR/chain.py" "$D"
