#!/usr/bin/env bash
# 用 MinGW-w64（posix 线程，std::thread 才可用）交叉编译，确认工程能编过。
# 这不是发布构建：发布包仍用 Visual Studio + /MT，见 README。
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cxx=${CXX:-x86_64-w64-mingw32-g++-posix}
rc=${RC:-x86_64-w64-mingw32-windres}
if ! command -v "$cxx" >/dev/null 2>&1; then
  echo "找不到 $cxx。请先安装 g++-mingw-w64-x86-64（posix 变体）。" >&2
  exit 1
fi
cmake -S "$root" -B "$root/build-mingw" \
  -DCMAKE_SYSTEM_NAME=Windows \
  -DCMAKE_CXX_COMPILER="$cxx" \
  -DCMAKE_RC_COMPILER="$rc" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$root/build-mingw" --parallel "${NPROC:-$(nproc)}"
echo "OK: $root/build-mingw/stargazer.exe"
