#!/bin/bash
# Usage: ./run_tutorial.sh <gmsh> <path/to/file.pro> [gmsh options...]
#
# Copies the directory containing file.pro to a unique tmp directory and runs
# <gmsh> there on file.pro with any extra options passed through unchanged.

set -e # Any non-zero exit aborts the script, propagating the exit code

if [ $# -lt 3 ]; then
    echo "Usage: $0 <gmsh> <getdp> <file.pro> [options...]" >&2
    exit 1
fi

GMSH="$1"
GETDP="$2"
PRO_PATH="$3"
shift 3 # remaining args ($@) are forwarded to gmsh

if [ ! -f "$PRO_PATH" ]; then
    echo "Error: .pro file not found: $PRO_PATH" >&2
    exit 1
fi

PRO_DIR=$(dirname "$PRO_PATH")
PRO_FILE=$(basename "$PRO_PATH")
GEO_FILE="${PRO_FILE%.pro}.geo"

# If GMSH or GETDP was given as a relative path, make it absolute so it still
# resolves after we cd into the tmp directory. A bare command name (no slash) is
# left alone so $PATH lookup still works.
if [[ "$GMSH" == */* && "$GMSH" != /* ]]; then
    GMSH="$(cd "$(dirname "$GMSH")" && pwd)/$(basename "$GMSH")"
fi
if [[ "$GETDP" == */* && "$GETDP" != /* ]]; then
    GETDP="$(cd "$(dirname "$GETDP")" && pwd)/$(basename "$GETDP")"
fi

# Unique tmp directory (mktemp guarantees uniqueness, no collisions).
TMP_DIR=$(mktemp -d -t run-XXXXXXXX)
echo "Working in: $TMP_DIR" >&2

# Copy the *contents* of PRO_DIR (the trailing /. picks up dotfiles too)
# preserving timestamps/permissions.
cp -a "$PRO_DIR"/. "$TMP_DIR"/

cd "$TMP_DIR"

# exec so gmsh's exit code becomes the script's exit code.
"$GMSH" "$GEO_FILE" -bin -3 "$@"
"$GETDP" "$PRO_FILE" -bin -solve \#1 -pos \#1 "$@"
