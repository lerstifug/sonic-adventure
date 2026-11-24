#!/bin/sh
# Build the PS2 slice on WSL from a copy of the tree on WSL's own
# filesystem, where make's every look at a file isn't a round trip to
# Windows, as /mnt/'s are.
#
#   engine/tools/ps2wsl.sh [--full] [MAKE'S ARGUMENTS]
#
# run from the Windows tree on WSL (/mnt/d/sonic-adventure, say). The
# sources go to the copy, PS2_MIRROR, ~/sonic-adventure by default, by rsync,
# which sends a file only where its size or time differs and keeps each
# file's time, so make there builds again only what changed. make ps2 then
# runs in the copy, its objects in PS2_OUT, ~/ps2out by default, and the
# game goes to the Windows tree's build/engine/ps2/game/, where an emulator
# on Windows finds it (engine/mk/ps2.mk's PS2_RUN).
#
# the disc (orig/), the fetched tools (tools/) and what the build made
# outside the slices (build/) go once, as the copy begins, or again with
# --full: they rarely change, and they're most of the tree's files
set -e

tree=$(cd "$(dirname "$0")/../.." && pwd)
mirror=${PS2_MIRROR:-$HOME/sonic-adventure}
full=
if [ "$1" = --full ]; then
    full=1
    shift
fi
if [ ! -d "$mirror/orig" ]; then
    full=1
fi
mkdir -p "$mirror"

# the sources: the tree but what follows, each file deleted from it deleted
# from the copy. the excluded go untouched there
rsync -a --delete --exclude=/build/ --exclude=/orig/ --exclude=/tools/ --exclude=/.git/ "$tree/" "$mirror/"
if [ -n "$full" ]; then
    echo "ps2wsl: copying the disc, the tools and the build's own outputs to $mirror"
    rsync -a --delete "$tree/orig/" "$mirror/orig/"
    rsync -a --delete "$tree/tools/" "$mirror/tools/"
    # the other slices' builds stay behind: only the generated sources and
    # the stamps the slice reads go
    rsync -a --exclude=/engine/ "$tree/build/" "$mirror/build/"
fi

export PS2_OUT="${PS2_OUT:-$HOME/ps2out}"
export PS2_RUN="$tree/build/engine/ps2/game"
cd "$mirror"
exec make -j"$(nproc)" ps2 "$@"
