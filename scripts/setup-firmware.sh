#!/bin/bash
#
# setup-firmware.sh — Prepare a forked genoqs-community/source repo with
# the 5 Linux/Windows port patches, then add it as a submodule.
#
# Prerequisites:
#   1. Fork https://github.com/genoqs-community/source on GitHub
#   2. Run this script from the Octopus repo root:
#
#   ./scripts/setup-firmware.sh git@github.com:YOURUSER/source.git
#
# What this does:
#   - Clones your fork to a temp directory
#   - Applies the 5 patches from patches/
#   - Commits and pushes to your fork
#   - Adds the fork as a git submodule at firmware/
#
set -e

FORK_URL="${1:-}"
if [ -z "$FORK_URL" ]; then
    echo "Usage: $0 <fork-url>"
    echo "Example: $0 git@github.com:youruser/source.git"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PATCH_DIR="$REPO_DIR/patches"

echo "=== Linux/Windows port firmware setup ==="
echo "Fork URL: $FORK_URL"
echo ""

# Clone fork to temp dir
TMPDIR=$(mktemp -d)
echo "Cloning fork to $TMPDIR ..."
git clone "$FORK_URL" "$TMPDIR"
cd "$TMPDIR"

# Apply patches
echo ""
echo "Applying patches..."
for patch in "$PATCH_DIR"/*.patch; do
    echo "  $(basename "$patch")"
    git apply --verbose "$patch"
done

# Commit and push
echo ""
echo "Committing patched firmware..."
git add -A
git commit -m "Apply Linux/Windows port patches

5 files modified with #ifdef __linux__ / _WIN32 guards:
- includes-declarations.h: swap eCos headers for hal_linux.h
- Init_memory.h: NULL guard in PAGE_init()
- cpu-load.c: disable CPU load check on Linux/Windows
- play_MIDI.h: route MIDI_send() to ALSA/winmm backend
- show_hwdriver.h: suppress hardware VIEWER_show_MIR()
"

echo ""
echo "Pushing to fork..."
git push origin HEAD

FORK_COMMIT=$(git rev-parse HEAD)
echo "Fork commit: $FORK_COMMIT"

# Cleanup
cd "$REPO_DIR"
rm -rf "$TMPDIR"

# Add submodule
echo ""
echo "Adding submodule at firmware/ ..."
if [ -d "$REPO_DIR/firmware" ]; then
    echo "Error: firmware/ already exists. Remove it first."
    exit 1
fi
git submodule add "$FORK_URL" firmware
cd firmware
git checkout "$FORK_COMMIT"
cd "$REPO_DIR"
git add firmware .gitmodules

echo ""
echo "=== Done ==="
echo "The firmware submodule is ready at firmware/"
echo "OCT_OS/ and NEMO_OS/ are inside firmware/"
echo ""
echo "Next: commit the submodule reference:"
echo "  git commit -m 'Add firmware submodule (genoqs-community/source fork)'"
