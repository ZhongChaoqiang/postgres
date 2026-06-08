#!/bin/bash
set -e

echo "=== 下载 pgvector 最新版 ==="
cd /tmp
rm -rf pgvector
git clone https://github.com/pgvector/pgvector.git 2>&1 | tail -3

echo "=== 编译 pgvector ==="
cd /tmp/pgvector
make -j4 2>&1 | tail -10

echo "=== 安装 pgvector ==="
sudo make install 2>&1 | tail -5

echo "=== 验证 ==="
ls -la /usr/lib/postgresql/18/lib/vector.so
