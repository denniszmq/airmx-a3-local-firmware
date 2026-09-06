#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .test-work
BUILD_DIR="${AIRMX_BUILD_DIR:-${TMPDIR:-/tmp}/airmx-a3-local-build}"

cc -DCJSON_NESTING_LIMIT=16 -std=c11 -Wall -Wextra -Werror -Wno-deprecated-declarations -fsanitize=address,undefined -g -Imain -Icomponents/cjson tests/test_protocol.c main/protocol.c components/cjson/cJSON.c -o .test-work/test_protocol
.test-work/test_protocol

cc -DCJSON_NESTING_LIMIT=16 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain tests/test_ota_guard.c main/ota_guard.c -o .test-work/test_ota_guard
if [[ -f "$BUILD_DIR/airmx_local.bin" ]]; then
  .test-work/test_ota_guard "$BUILD_DIR/airmx_local.bin"
else
  echo "跳过 OTA 镜像测试：请先运行 ./build.sh 生成 $BUILD_DIR/airmx_local.bin"
fi

cc -DCJSON_NESTING_LIMIT=16 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined tests/test_network.c main/network_validate.c -o .test-work/test_network
.test-work/test_network

node tests/test_ui.cjs

cc -DCJSON_NESTING_LIMIT=16 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain tests/test_goose.c main/goose_decode.c -o .test-work/test_goose
.test-work/test_goose

cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Imain tests/test_water_usage.c main/water_usage.c -o .test-work/test_water_usage
.test-work/test_water_usage
