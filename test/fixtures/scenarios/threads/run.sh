#!/bin/sh
# threads: 4 threads, each opening its own file. Overlap is real (threads
# block on pipes while siblings work) but milestones are ordered: the main
# thread polls file existence (stat: invisible, we never decode it) between
# creations, and an ack-pipe chain orders opens (f0..f3) and deaths
# (t3,t2,t1,t0). Deterministic and genuinely concurrent.
export PATH=/usr/bin:/bin
export PYTHONHASHSEED=0
export PYTHONDONTWRITEBYTECODE=1
DIR=$(dirname "$0")
D=$(mktemp -d)
# -S skips site imports (.pth, sitecustomize, dist-packages): their file sets
# differ per distro release, so without -S the golden would be version-bound.
python3 -B -S "$DIR/worker.py" "$D"
