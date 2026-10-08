#!/bin/sh
set -e
cd /home/james/nova3probe
cp port/LICENSE LICENSE
sed -i 's#android/qemu/#qemu/#g' port/android/README.md
[ -d .git ] || { git init -q; git symbolic-ref HEAD refs/heads/master; }
git config user.name JamesCrumble
git config user.email namster222333@gmail.com
git add -A
echo "files: $(git diff --cached --name-only | wc -l)"
git diff --cached --name-only | while read f; do [ -f "$f" ] && stat -c '%s %n' "$f"; done | sort -rn | head -5
git commit -q -m "Initial commit: NOVA3 Bridge

N.O.V.A. 3 (ARM32) on 64-bit-only Android phones: patched qemu-user 11.1.2,
GLES bridge to the phone GPU, Java APK wrapper, tools.

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git remote get-url origin >/dev/null 2>&1 || git remote add origin git@github.com:JamesCrumble/NOVA3-Bridge.git
git push -u origin master 2>&1 | tail -3
git log --oneline -1
