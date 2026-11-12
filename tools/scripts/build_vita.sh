#!/usr/bin/env bash
set -euo pipefail

# Default: build everything (both VPKs) and report the real game VPK.
# --smoke: build/report only the toolchain smoke-test VPK (faster, useful
#   when iterating on platform glue without touching the game runtime).
smoke_only=0
for arg in "$@"; do
    case "$arg" in
        --smoke) smoke_only=1 ;;
        *)
            echo "Unknown argument: $arg (supported: --smoke)" >&2
            exit 1
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$repo_root"

if [ -z "${VITASDK:-}" ]; then
    if [ -d "$HOME/vitasdk" ]; then
        export VITASDK="$HOME/vitasdk"
    else
        cat >&2 <<'EOF'
VITASDK is not set and $HOME/vitasdk was not found.

Install VitaSDK first (see docs/research/VITA_BUILD.md for full details):
  brew install wget cmake
  git clone https://github.com/vitasdk/vdpm ~/vita-toolchain-src/vdpm
  cd ~/vita-toolchain-src/vdpm
  ./bootstrap-vitasdk.sh --install-dir "$HOME/vitasdk"
EOF
        exit 1
    fi
fi
export PATH="$VITASDK/bin:$PATH"

cmake -S vita -B build/vita \
    -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release

if [ "$smoke_only" = "1" ]; then
    cmake --build build/vita --target "CruisnWorldVitaSmoke.vpk-vpk" --parallel
    vpk="$repo_root/build/vita/CruisnWorldVitaSmoke.vpk"
    label="Vita smoke-test VPK"
else
    cmake --build build/vita --parallel
    vpk="$repo_root/build/vita/CruisnWorldXtreme.vpk"
    label="Vita VPK"
fi

if [ -f "$vpk" ]; then
    printf '%s: %s\n' "$label" "$vpk"
else
    echo "Build finished but the VPK was not found at the expected path: $vpk" >&2
    exit 1
fi
