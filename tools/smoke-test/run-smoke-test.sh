#!/usr/bin/env bash
#
# yabridge: a Wine plugin bridge
# Copyright (C) 2020-2026 Robbert van der Helm
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.
#
# End-to-end smoke test for yabridge's 32-bit and 64-bit plugin hosts. This
# builds a minimal Windows VST2 plugin as both a 32-bit and a 64-bit DLL and a
# minimal native VST2 host, and then uses those to test:
#
# - the 64-bit plugin through the 64-bit Winelib host,
# - the 32-bit plugin through the 32-bit PE host,
# - both plugins loaded at the same time in the same native process, and
# - opening the editors, if an X11 server is available.
#
# Usage:
#
#   tools/smoke-test/run-smoke-test.sh <build_dir> [<pe_build_dir>]
#
# Where `<build_dir>` is the regular build directory (configured with
# `cross-wine.conf`), and `<pe_build_dir>` is the build directory for the
# 32-bit PE host (configured with `cross-mingw-i686.conf`). If the second
# directory is omitted, `yabridge-host-32.exe` should already be in
# `<build_dir>`.
#
# Environment variables:
#
# - `WINEPREFIX`: The Wine prefix to use. Defaults to a new temporary prefix.
# - `SMOKE_TEST_NO_EDITOR=1`: Skip the editor tests.
# - `MINGW_SUFFIX`: Suffix for the MinGW compilers, e.g. `-posix` on Debian and
#   Ubuntu. Defaults to nothing, which is correct for Arch Linux.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo >&2 "Usage: $0 <build_dir> [<pe_build_dir>]"
  exit 2
fi

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
source_dir=$(cd "$script_dir/../.." && pwd)
build_dir=$(cd "$1" && pwd)
pe_build_dir=${2:+$(cd "$2" && pwd)}
mingw_suffix=${MINGW_SUFFIX:-}

work_dir=$(mktemp -d -t yabridge-smoke-test.XXXXXXXX)
trap 'rm -rf "$work_dir"' EXIT

if [[ -z ${WINEPREFIX:-} ]]; then
  export WINEPREFIX="$work_dir/prefix"
fi
export WINEDEBUG=${WINEDEBUG:--all}

echo "Building the test plugins and the test host..."
for arch in i686 x86_64; do
  "$arch-w64-mingw32-g++$mingw_suffix" -std=c++20 -O2 -shared -static \
    -I"$source_dir/src/include" -o "$work_dir/test-$arch.dll" \
    "$script_dir/test-plugin.cpp" -lgdi32
done
g++ -std=c++20 -O2 -I"$source_dir/src/include" -o "$work_dir/test-host" \
  "$script_dir/test-host.cpp" -lxcb -ldl -pthread

# yabridge looks for the host binaries next to the actual plugin library, so we
# need a directory with the plugin libraries, the host binaries, and the
# Windows plugins
plugin_dir="$work_dir/plugins"
mkdir -p "$plugin_dir"
cp "$build_dir/yabridge-host.exe" "$build_dir/yabridge-host.exe.so" "$plugin_dir/"
if [[ -n $pe_build_dir ]]; then
  cp "$pe_build_dir/yabridge-host-32.exe" "$plugin_dir/"
else
  cp "$build_dir/yabridge-host-32.exe" "$plugin_dir/"
fi
for bits in 32 64; do
  # These need to be separate copies, since the dynamic linker would otherwise
  # consider two symlinks to the same file to be the same library
  cp "$build_dir/libyabridge-vst2.so" "$plugin_dir/yabridge-test-$bits.so"
done
cp "$work_dir/test-i686.dll" "$plugin_dir/yabridge-test-32.dll"
cp "$work_dir/test-x86_64.dll" "$plugin_dir/yabridge-test-64.dll"

editor_flag=--editor
if [[ ${SMOKE_TEST_NO_EDITOR:-0} == 1 || -z ${DISPLAY:-} ]]; then
  echo "Skipping the editor tests"
  editor_flag=
fi

echo "Initializing the Wine prefix at '$WINEPREFIX'..."
# The Mono and Gecko installers would otherwise show a dialog and block, and
# `wineserver -w` waits until the prefix has been fully set up
WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1 || true
wineserver -w

failed=0
run_test() {
  local description=$1
  shift

  echo
  echo "=== $description"
  if "$work_dir/test-host" "$@" > "$work_dir/output.log" 2>&1; then
    grep -E '^\[|round trip|Running' "$work_dir/output.log" || true
  else
    cat "$work_dir/output.log"
    echo "*** FAILED: $description"
    failed=1
  fi
}

run_test "64-bit plugin" "$plugin_dir/yabridge-test-64.so" $editor_flag
run_test "32-bit plugin" "$plugin_dir/yabridge-test-32.so" $editor_flag
run_test "32-bit plugin alongside a 64-bit plugin" \
  "$plugin_dir/yabridge-test-32.so" $editor_flag \
  --alongside "$plugin_dir/yabridge-test-64.so"

echo
if [[ $failed == 0 ]]; then
  echo "All smoke tests passed"
else
  echo "Some smoke tests failed"
fi
exit $failed
