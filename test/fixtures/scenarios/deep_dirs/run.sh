#!/bin/sh
# deep_dirs: nested mkdir, cd, relative opens, cross-dir rename, rmdirs.
# Exercises dirfd/cwd canonicalisation and fs.rmdir. Strictly sequential.
export PATH=/usr/bin:/bin
D=$(mktemp -d)
mkdir -p "$D/a/b/c"
cd "$D/a" || exit 1
echo data > b/c/f.txt
mv b/c/f.txt b/g.txt
rmdir b/c
rm b/g.txt
rmdir b
cd "$D" || exit 1
rmdir a
