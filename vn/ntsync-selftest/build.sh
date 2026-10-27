#!/usr/bin/env bash
# Builds Linux's ntsync selftests (tools/testing/selftests/drivers/ntsync) as a 32-bit binary for the root fs
# and zips it as ntsync.zip, to run against VNBox's /dev/ntsync (source/kernel/devs/devntsync.cpp).
# Linked against Debian 12's glibc 2.36 (the root fs has 2.38): a static glibc binary crashes in malloc at start,
# and musl's threads don't start under the emulator. Built without -O2 like kselftest: at -O2 gcc keeps
# stress_wait's unsynchronized counter in a register and the test fails on any kernel. Needs Docker.
# Test source: Linux master facd8463 (adds wait_args_validation, fixes wake_all's EXPECT); harness: v6.14
# (later harnesses need pidfd_open). Usage: vn/ntsync-selftest/build.sh <out dir>
set -euo pipefail
out=$(realpath -m "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
get() { curl -sSfL -o "$work/$3" "https://raw.githubusercontent.com/torvalds/linux/$1/$2"; echo "$4  $work/$3" | sha256sum -c --quiet; }
mkdir -p "$work/linux"
get facd8463a5b62278fd5cdaa3fd6bad49315702a0 tools/testing/selftests/drivers/ntsync/ntsync.c ntsync.c deb03a0339ca750a8182d59d0b62fd6f36cc8f97da4d2a3664fbd3332bda624c
get v6.14 tools/testing/selftests/kselftest_harness.h kselftest_harness.h 68ee4952aa3db3f5bc47fcbe1cae985c599749d1752e66f365b30ad8bcec6160
get v6.14 tools/testing/selftests/kselftest.h kselftest.h e0273c88b63a3db4b29cffd8f5fdb9dd4c9229ba6a8890b15899f06791a1ec1b
get v6.14 include/uapi/linux/ntsync.h linux/ntsync.h 006437ee52a3e04f921df77081eb5c21c44c71f598b10ac534c6ef9e78296262
mkdir -p "$out"
docker run --rm --platform linux/386 -v "$work:/s" -w /s -e HOST_UID="$(id -u)" i386/debian:bookworm-slim sh -euc '
  apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev linux-libc-dev zip >/dev/null 2>&1
  gcc -I/s -o ntsync_test ntsync.c -lpthread
  zip -q ntsync.zip ntsync_test && chown "$HOST_UID" ntsync.zip'
cp "$work/ntsync.zip" "$out/"
echo "$out/ntsync.zip"
