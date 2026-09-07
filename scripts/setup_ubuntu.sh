#!/usr/bin/env bash
set -euo pipefail

if ! command -v apt-get >/dev/null 2>&1; then
  echo "This setup script targets Ubuntu. Required packages: build-essential cmake ninja-build libgtest-dev python3"
  exit 1
fi

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build libgtest-dev python3
echo "StreamVault build dependencies are installed."
