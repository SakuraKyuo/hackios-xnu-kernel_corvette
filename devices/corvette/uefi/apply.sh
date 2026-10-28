#!/bin/bash
# Make ../../../Mu-Silicium: a clone of Project-Silicium/Mu-Silicium at the commit our port was made
# on, with the corvette port (this directory) put into it.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../../Mu-Silicium
[ -e $MU ] && { echo "uefi/apply.sh: $MU already exists"; exit 1; }
git clone https://github.com/Project-Silicium/Mu-Silicium.git $MU
git -C $MU checkout -q "$(cat BASE)"
git -C $MU submodule update --init --recursive
git -C $MU apply --whitespace=nowarn "$PWD/mu-silicium.patch"
cp -r files/. $MU/
echo "uefi/apply.sh: done; next: python3 build_uefi.py -d corvette -r RELEASE"
