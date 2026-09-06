#!/usr/bin/env python3
"""Back up and flash AIRMX A3 local firmware through a 3.3 V USB-TTL adapter."""

from __future__ import annotations

import argparse
import hashlib
import subprocess
import sys
from datetime import datetime
from pathlib import Path


ROOT = Path(__file__).resolve().parent
FIRMWARE = ROOT / "firmware" / "v1.3.4"
IMAGES = [
    ("0x1000", FIRMWARE / "bootloader.bin"),
    ("0x8000", FIRMWARE / "partition-table.bin"),
    ("0xe000", FIRMWARE / "ota_data_initial.bin"),
    ("0x10000", FIRMWARE / "airmx-a3-v1.3.4.bin"),
]
EXPECTED_SHA256 = {
    "bootloader.bin": "9eb62ed9f8c74809ae224de510bfd5ad046cc852eb6bee10cdd3d1cd7b73c51d",
    "partition-table.bin": "1ae446228d79cf83a4b83de41c33d1e01dc21a3557149d7da90fbcbfc303311d",
    "ota_data_initial.bin": "7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f",
    "airmx-a3-v1.3.4.bin": "ad4789a3a964cb660da49721d24884e05455caff2887253e6ea8747e01431e37",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def esptool(port: str, baud: int, command: list[str]) -> None:
    args = [
        sys.executable,
        "-m",
        "esptool",
        "--chip",
        "esp32",
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        "no-reset",
        "--after",
        "no-reset",
        *command,
    ]
    subprocess.run(args, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Back up the full 4 MB flash, then install AIRMX A3 local firmware v1.3.4."
    )
    parser.add_argument("port", help="Serial port, e.g. /dev/cu.usbserial-XXXX or COM3")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate (default: 115200)")
    args = parser.parse_args()

    try:
        import esptool  # noqa: F401
    except ImportError:
        print("缺少 esptool。请先运行：python -m pip install --upgrade esptool", file=sys.stderr)
        return 2

    for _, image in IMAGES:
        actual = sha256(image)
        expected = EXPECTED_SHA256[image.name]
        if actual != expected:
            print(f"校验失败：{image.name}\n期望 {expected}\n实际 {actual}", file=sys.stderr)
            return 3

    print("\n刷写前必须确认：")
    print("1. 原 24V 电源和主板上的所有外设插头均已拔掉。")
    print("2. USB-TTL 为 3.3V TTL；已接 GND、3.3V、交叉 TX/RX。")
    print("3. G0 已可靠接到 GND，并在此状态下重新给主板上电。")
    answer = input("以上三项均确认，输入 FLASH 继续：").strip()
    if answer != "FLASH":
        print("已取消，未读取或写入 Flash。")
        return 1

    print("\n检查 ESP32 连接……")
    esptool(args.port, args.baud, ["flash-id"])

    backup_dir = ROOT / "backups"
    backup_dir.mkdir(exist_ok=True)
    backup = backup_dir / f"airmx-a3-before-v1.3.4-{datetime.now():%Y%m%d-%H%M%S}.bin"
    print(f"\n先备份完整 4 MB Flash 到：{backup}")
    esptool(args.port, args.baud, ["read-flash", "0x0", "0x400000", str(backup)])
    print(f"备份 SHA-256：{sha256(backup)}")

    print("\n写入 v1.3.4（不执行全片擦除）……")
    write_args = [
        "write-flash",
        "--flash-mode",
        "dio",
        "--flash-size",
        "4MB",
        "--flash-freq",
        "40m",
    ]
    for address, image in IMAGES:
        write_args.extend([address, str(image)])
    esptool(args.port, args.baud, write_args)

    verify_args = ["verify-flash"]
    for address, image in IMAGES:
        verify_args.extend([address, str(image)])
    print("\n再次逐段校验……")
    esptool(args.port, args.baud, verify_args)

    print("\n刷写和校验完成。")
    print("先拔掉 USB；拆除 G0-GND；恢复所有外设插头；最后再接原 24V 电源开机。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
