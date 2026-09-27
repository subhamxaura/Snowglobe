#!/bin/sh
# failing_exec: a genuine ENOENT (sh forks, exec fails, child exits 127),
# a PATH-search success, and a setuid binary under ptrace.
# NOTE on passwd: a ptraced process cannot gain privileges via setuid exec,
# so /usr/bin/passwd runs unprivileged here (its --help needs none) and the
# trace shows an ordinary exec. `|| true` keeps the script at exit 0; only
# the recorded proc.exit codes matter and they are deterministic.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
sh -c 'exec definitely-not-a-real-binary-xyz' || true
ls "$D" > /dev/null
/usr/bin/passwd --help > /dev/null 2>&1 || true
