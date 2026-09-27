#!/bin/sh
# threads: C helper with 4 threads and ordered milestones (see sg_threads.c).
# No interpreter: python startup file sets differ per release (link order,
# import closure) and cannot be normalised — compiled helpers open libc only.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
H=${SG_HELPERS:?sg helpers missing}
"$H/sg_threads" "$D"
