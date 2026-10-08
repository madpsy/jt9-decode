#!/usr/bin/env bash
#
# Build jt9_decode for amd64 and arm64, and check that each one decodes.
#
# Built inside ubuntu:24.04, the same image the UberSDR container runtime stage
# uses, and linked fully statically: the binary needs no Qt, no libstdc++ and
# no particular glibc, so it runs on any amd64/arm64 Linux. The build refuses a
# binary that needs any shared library at all.
#
# arm64 is built by running an arm64 ubuntu:24.04 under binfmt/qemu, not by
# cross-compiling, so the toolchain is the target toolchain. Slow, and correct
# without a sysroot to keep in step.
#
# Building is the easy half. A binary that links and runs can still fail to
# talk to jt9 (the shared memory has to match what jt9's Qt expects), so unless
# told otherwise each one decodes test_ft8.wav with the real jt9 from that
# arch's WSJT-X .deb on the 1.0.0 release, and must produce exactly the decodes
# in tests/golden/ft8_wav.txt. amd64 is checked in ubuntu:24.04 and arm64 in
# debian:trixie, the distribution the arm64 .deb was built for.
#
# Usage:
#   ./build.sh [options]
#
#   --arch LIST     comma-separated: amd64, arm64 (default: both)
#   --native        build on this host with the host toolchain instead of in a
#                   container.  This host arch only; for a quick edit-compile
#                   loop, not for anything you intend to ship
#   --clean         delete dist/ and the cached .debs first
#   --no-check      build only, skip the decode check
#   --image IMAGE   build container image (default: ubuntu:24.04)
#   --publish       upload what this run built to the GitHub release named in
#                   the VERSION file (created if it does not exist)
#   --yes           answer the publish confirmation in advance
#

set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
arches="amd64 arm64"
image=ubuntu:24.04
native=0
clean=0
check=1
publish=0
assume_yes=0

dist=$repo/dist
cache=$repo/.cache

# Binaries are published as jt9_decode_<arch> on the release named by VERSION.
# Only those two assets are added or replaced: anything else on the release
# (the WSJT-X .debs on 1.0.0) is never touched.
REPO="${JT9_DECODE_REPO:-madpsy/jt9-decode}"
VERSION=$(tr -d '[:space:]' < "$repo/VERSION")

# Where the WSJT-X .debs used for the decode check live. Fixed rather than
# following VERSION: later releases need not carry the .debs again.
DEB_TAG=1.0.0

# jt9's own shared libraries, enough to run it without installing WSJT-X
JT9_LIBS="libfftw3-single3 libgfortran5 libgomp1 libqt5core5t64"

while [ $# -gt 0 ]; do
    case "$1" in
        --arch)     arches=$(echo "$2" | tr ',' ' '); shift 2 ;;
        --native)   native=1; shift ;;
        --clean)    clean=1; shift ;;
        --no-check) check=0; shift ;;
        --image)    image=$2; shift 2 ;;
        --publish)  publish=1; shift ;;
        --yes)      assume_yes=1; shift ;;
        -h|--help)  sed -n '2,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^#\{0,1\} \{0,1\}//'; exit 0 ;;
        *)          echo "build.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

say()  { printf '\n== %s\n' "$*"; }
fail() { printf '\nbuild.sh: %s\n' "$*" >&2; exit 1; }

for a in $arches; do
    case "$a" in
        amd64|arm64) ;;
        *) fail "unknown arch '$a' (expected amd64 or arm64)" ;;
    esac
done

[[ $VERSION =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    || fail "VERSION must hold a version like 1.2.3, not '$VERSION'"

# --- Refusals, decided before anything is built ---------------------------

if [ "$publish" = 1 ] && [ "$check" = 0 ]; then
    fail "--publish and --no-check together would upload a binary nothing has
watched decode anything. A jt9_decode that builds and runs but whose shared
memory jt9 cannot attach to decodes nothing, which a receiver shows as a quiet
band. Drop one of the two."
fi

if [ "$publish" = 1 ] && [ "$native" = 1 ]; then
    fail "--publish and --native together would upload a binary built with this
host's toolchain. Build it in the container: drop --native."
fi

if [ "$publish" = 1 ] && [ -n "$(git -C "$repo" status --porcelain --untracked-files=no)" ]; then
    fail "--publish with uncommitted changes would upload binaries that match no
commit. Commit first, so the release can be traced back to its source."
fi

# --- Publishing ------------------------------------------------------------

publish_release() { # <binary>...
    local uploads=("$@")

    command -v gh >/dev/null 2>&1 || {
        echo "not published: gh not found — install the GitHub CLI, or upload the
  binaries by hand." >&2
        return 1
    }
    gh auth status >/dev/null 2>&1 || {
        echo "not published: gh is not logged in — run 'gh auth login'." >&2
        return 1
    }

    local exists=0
    gh release view "$VERSION" --repo "$REPO" >/dev/null 2>&1 && exists=1

    echo
    if [ "$exists" = 1 ]; then
        echo "  Upload to https://github.com/$REPO/releases/tag/$VERSION, replacing what is there:"
    else
        echo "  Create release $VERSION on $REPO at $(git -C "$repo" rev-parse --short HEAD) with:"
    fi
    for b in "${uploads[@]}"; do
        printf '      %-24s %s\n' "$(basename "$b")" "$(du -h "$b" | cut -f1)"
    done
    # Only the arches this run built are replaced; any other asset on the
    # release stays exactly as it was.
    echo

    if [ "$assume_yes" = 1 ]; then
        echo "  --yes given; uploading."
    elif [ ! -t 0 ]; then
        echo "not published: --publish asks before uploading and there is no terminal
  to ask on. Pass --yes to answer it in advance." >&2
        return 1
    else
        local reply=''
        read -r -p "  type 'yes' to upload: " reply || true
        if [ "$reply" != "yes" ]; then
            echo "  not published."
            return 1
        fi
    fi

    if [ "$exists" = 1 ]; then
        # --clobber because the asset names are constants
        gh release upload "$VERSION" "${uploads[@]}" --clobber --repo "$REPO"
    else
        gh release create "$VERSION" "${uploads[@]}" --repo "$REPO" \
            --target "$(git -C "$repo" rev-parse HEAD)" --title "$VERSION" \
            --notes "jt9_decode $VERSION"
    fi || { echo "not published: the upload failed — the binaries are intact, try again." >&2; return 1; }
    echo "  uploaded to https://github.com/$REPO/releases/tag/$VERSION"
}

# --- Inside the build container -------------------------------------------
#
# Fed to bash on stdin rather than passed as `bash -c '...'`: an apostrophe
# anywhere in here would close the quote and silently truncate the script.
#
# $1 = arch, $2 = version, $3:$4 = host uid:gid
build_script=$(cat <<'BUILD_EOF'
set -euo pipefail
arch=$1; version=$2; uid=$3; gid=$4

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends g++ >/dev/null

g++ -std=c++11 -O2 -Wall -Wextra -static -pthread -I/src/wsjtx \
    "-DJT9_DECODE_VERSION=\"$version\"" \
    -o "/out/jt9_decode_$arch" /src/jt9_decode.cpp
strip "/out/jt9_decode_$arch"

# The container runs as root; without this dist/ comes out root-owned
chown "$uid:$gid" "/out/jt9_decode_$arch"
BUILD_EOF
)

# --- Inside the check container ---------------------------------------------
#
# $1 = arch, $2 = version, $3 = the .deb (relative to /debs)
check_script=$(cat <<'CHECK_EOF'
set -euo pipefail
arch=$1; version=$2; deb=$3
bin=/dist/jt9_decode_$arch

got=$("$bin" --version)
if [ "$got" != "jt9_decode $version" ]; then
    echo "--version says '$got', expected 'jt9_decode $version'" >&2
    exit 1
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends $JT9_LIBS >/dev/null
dpkg-deb -x "/debs/$deb" /tmp/wsjtx
jt9=/tmp/wsjtx/usr/bin/jt9

# jt9 writes timer.out and wisdom files into its working directory
mkdir -p /tmp/run && cd /tmp/run
"$bin" -j "$jt9" -m FT8 /src/test_ft8.wav > out.txt 2> err.txt || {
    echo "jt9_decode failed:" >&2; tail -20 err.txt >&2; exit 1
}
{ grep -E '^[0-9]{6} ' out.txt || true; } | cut -c7- | sed 's/[[:space:]]*$//' | LC_ALL=C sort > decodes.txt

# amd64 must match the golden output exactly. Other architectures compute
# slightly different floating point results, so SNR/DT/frequency can differ by
# a unit there: compare the decoded messages instead.
golden=/src/tests/golden/ft8_wav.txt
if [ "$arch" = amd64 ]; then
    what="decodes"
    cp "$golden" want.txt; cp decodes.txt got.txt
else
    what="decoded messages"
    sed 's/.*~  //' "$golden" | LC_ALL=C sort > want.txt
    sed 's/.*~  //' decodes.txt | LC_ALL=C sort > got.txt
fi
if ! diff -u want.txt got.txt >&2; then
    echo "$what differ from $golden (above); jt9_decode stderr:" >&2
    tail -20 err.txt >&2
    exit 1
fi
echo "CHECK_OK $(wc -l < got.txt) $what match tests/golden/ft8_wav.txt"
CHECK_EOF
)

# Refuse anything but a fully static binary of the right architecture
verify_static() { # <binary> <arch>
    local b=$1 desc want
    case "$2" in amd64) want="x86-64" ;; arm64) want="ARM aarch64" ;; esac
    desc=$(file -b "$b")
    [[ $desc == *"$want"* ]] || fail "$(basename "$b") is not $2: $desc"
    if readelf -d "$b" 2>/dev/null | grep -q NEEDED; then
        echo "$(basename "$b") links shared libraries that should have been static:" >&2
        readelf -d "$b" | grep NEEDED >&2
        exit 1
    fi
}

if [ "$clean" = 1 ]; then
    say "Removing dist/ and cached .debs"
    rm -rf "$dist" "$cache"
fi
mkdir -p "$dist"

# --- Native build ------------------------------------------------------------

if [ "$native" = 1 ]; then
    host_arch=$(dpkg --print-architecture 2>/dev/null || echo amd64)
    say "Building natively for $host_arch (host toolchain)"
    out=$dist/jt9_decode_$host_arch
    g++ -std=c++11 -O2 -Wall -Wextra -static -pthread -I"$repo/wsjtx" \
        "-DJT9_DECODE_VERSION=\"$VERSION\"" -o "$out" "$repo/jt9_decode.cpp"
    strip "$out"
    verify_static "$out" "$host_arch"
    if [ "$check" = 1 ]; then
        jt9=$(command -v jt9 || true)
        [ -n "$jt9" ] || fail "no jt9 on this host to check against; pass --no-check"
        say "Checking $(basename "$out") with $jt9"
        QUICK=1 "$repo/tests/run_e2e.sh" "$out"
    fi
    say "Done"
    echo "  $out"
    echo
    echo "Built with this host's toolchain — fine for testing here. Drop --native"
    echo "for anything you intend to publish."
    exit 0
fi

# --- Container builds ----------------------------------------------------------

command -v docker >/dev/null 2>&1 \
    || fail "docker is needed to build for both architectures; use --native to
build only for this host"

# arm64 on an amd64 host needs the binfmt handler, or docker starts the
# container and every process in it dies with exec format error.
host_arch=$(dpkg --print-architecture 2>/dev/null || echo amd64)
for a in $arches; do
    [ "$a" = "$host_arch" ] && continue
    if ! ls /proc/sys/fs/binfmt_misc/ 2>/dev/null | grep -qi "qemu-aarch64\|qemu-x86_64"; then
        fail "no qemu binfmt handler registered, so a $a container cannot run here.
Install it with:
  docker run --privileged --rm tonistiigi/binfmt --install all"
    fi
done

# The WSJT-X .debs whose jt9 the check runs against
fetch_deb() { # <arch> -> prints the file name in $cache/debs
    local arch=$1 f
    mkdir -p "$cache/debs"
    f=$(cd "$cache/debs" && ls -- *_"$arch".deb 2>/dev/null | head -1 || true)
    if [ -z "$f" ]; then
        command -v gh >/dev/null 2>&1 \
            || fail "gh is needed to fetch the $arch WSJT-X .deb for the check (or pass --no-check)"
        gh release download "$DEB_TAG" --repo "$REPO" --pattern "*_$arch.deb" --dir "$cache/debs" >&2 \
            || fail "could not download the $arch WSJT-X .deb from release $DEB_TAG"
        f=$(cd "$cache/debs" && ls -- *_"$arch".deb | head -1)
    fi
    echo "$f"
}

built=""
for arch in $arches; do
    say "Building $arch in $image"
    rm -f "$dist/jt9_decode_$arch"
    if ! printf %s "$build_script" | docker run --rm -i \
        --platform "linux/$arch" \
        -v "$repo:/src:ro" -v "$dist:/out" \
        "$image" \
        bash -s -- "$arch" "$VERSION" "$(id -u)" "$(id -g)" \
        2>&1 | sed 's/^/  /'; then
        fail "$arch: the build failed"
    fi
    [ -x "$dist/jt9_decode_$arch" ] || fail "$arch: the build produced no binary"
    verify_static "$dist/jt9_decode_$arch" "$arch"

    if [ "$check" = 1 ]; then
        case "$arch" in amd64) check_image=ubuntu:24.04 ;; arm64) check_image=debian:trixie ;; esac
        deb=$(fetch_deb "$arch")
        say "Checking $arch in $check_image against jt9 from $deb"
        if ! printf %s "$check_script" | docker run --rm -i \
            --platform "linux/$arch" \
            -e JT9_LIBS="$JT9_LIBS" \
            -v "$repo:/src:ro" -v "$dist:/dist:ro" -v "$cache/debs:/debs:ro" \
            "$check_image" \
            bash -s -- "$arch" "$VERSION" "$deb" \
            2>&1 | while IFS= read -r line; do
                case "$line" in
                    CHECK_OK*) echo "  decoded: ${line#CHECK_OK }" ;;
                    *)         echo "  $line" ;;
                esac
            done; then
            fail "$arch: the decode check failed"
        fi
    fi
    built="$built $dist/jt9_decode_$arch"
done

say "Done"
for b in $built; do
    printf '  %s — %s\n' "$(basename "$b")" "$(file -b "$b" | cut -d, -f1-2)"
done

if [ "$publish" = 1 ]; then
    say "Publishing"
    # shellcheck disable=SC2086  # $built is a deliberate whitespace-separated list
    publish_release $built
else
    echo
    echo "Upload both to release $VERSION (only the jt9_decode_* assets are replaced):"
    echo "  ./build.sh --publish"
fi
