#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root or with sudo: sudo ./unload.sh"
    exit 1
fi

rmmod cbuf
echo "Module cbuf unloaded successfully"
