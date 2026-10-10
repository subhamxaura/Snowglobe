#!/bin/sh
# x32: one x32-ABI mkdirat via the sg_x32 helper (see sg_x32.c). Both
# backends must classify it as an mkdir attempt (R5 class parity).
# No interpreter: python startup file sets differ per release and cannot
# be normalised — same rationale as threads/run.sh.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
H=${SG_HELPERS:?sg helpers missing}
"$H/sg_x32" "$D"
