#!/bin/sh
# exec_chain: sh -> sh -> env -> ls, four images, no interpreter (python
# startup file sets differ per release; see threads/run.sh).
export PATH=/usr/bin:/bin
D=$(mktemp -d)
exec sh -c "exec env ls \"$D\""
