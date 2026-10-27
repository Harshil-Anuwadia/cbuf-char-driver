#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

NDEVS=${1:-4}
BUFSZ=${2:-4096}

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root or with sudo: sudo ./load.sh [num_devices] [buf_size]"
    exit 1
fi

if ! insmod cbuf.ko num_devices=$NDEVS buf_size=$BUFSZ 2>/dev/null; then
    insmod -f cbuf.ko num_devices=$NDEVS buf_size=$BUFSZ
fi

chmod 666 /dev/cbuf[0-9]* 2>/dev/null || true

echo "Loaded $NDEVS devices (buf_size=$BUFSZ)"
