#!/usr/bin/env bash
# Run a command (default: interactive shell) inside the Yocto build container.
#   scripts/container.sh                 # shell
#   scripts/container.sh bitbake ...     # one-off command
# WORK (default ~/imx95-work) holds sources, downloads, sstate and build output.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${WORK:-$HOME/imx95-work}"
IMAGE="imx95-devenv-builder:24.04"
mkdir -p "$WORK"

if ! podman image exists "$IMAGE"; then
    podman build -t "$IMAGE" -f "$REPO/container/Containerfile" "$REPO/container"
fi

# Container-only git identity (repo/bitbake need one; nothing is pushed from here)
CHOME="$WORK/.container-home"; mkdir -p "$CHOME"
GITCFG="$CHOME/.gitconfig"
[ -f "$GITCFG" ] || printf '[user]\n\tname = imx95-builder\n\temail = builder@localhost\n[color]\n\tui = false\n' > "$GITCFG"

tty_flags=(-i); [ -t 0 ] && tty_flags=(-it)
exec podman run --rm "${tty_flags[@]}" \
    --userns=keep-id --security-opt label=disable \
    -e HOME="$CHOME" -e WORK="$WORK" -e REPO="$REPO" \
    -v "$WORK:$WORK" -v "$REPO:$REPO" \
    -w "$WORK" "$IMAGE" "${@:-bash}"
