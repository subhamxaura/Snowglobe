#!/bin/sh
# fork_storm: 50 children, each writing one file. Strictly sequential
# (fork, run, wait) so the event stream is deterministic: tracer order
# follows kernel stop order for a single-threaded controller.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
i=1
while [ "$i" -le 50 ]; do
  sh -c "echo storm > $D/f-$i" &
  wait
  i=$((i + 1))
done
