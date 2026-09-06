#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
BUILD_DIR="${AIRMX_BUILD_DIR:-${TMPDIR:-/tmp}/airmx-a3-local-build}"

if command -v idf.py >/dev/null 2>&1; then
  exec idf.py -B "$BUILD_DIR" build
fi

if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/tools/idf.py" ]]; then
  exec "${PYTHON:-python3}" "$IDF_PATH/tools/idf.py" -B "$BUILD_DIR" build
fi

echo "未找到 ESP-IDF。请先安装 ESP-IDF 6.0.1，并运行其 export.sh / export.ps1 加载环境。" >&2
exit 1
