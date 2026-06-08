#!/bin/bash
# PostgreSQL 18 一键编译出包脚本
# 用法: ./build_deb.sh [clean]
#   clean: 清理旧构建后重新编译

set -e

SOURCE_DIR="/mnt/d/workspace/postgres"
OUTPUT_DIR="/mnt/d/workspace"

echo "=========================================="
echo " PostgreSQL 18 一键编译出包"
echo "=========================================="

cd "$SOURCE_DIR"

# 清理旧构建
if [ "$1" = "clean" ]; then
    echo "[1/3] 清理旧构建..."
    rm -rf debian/build debian/tmp
    rm -f "$OUTPUT_DIR"/*.deb "$OUTPUT_DIR"/*.buildinfo "$OUTPUT_DIR"/*.changes
else
    echo "[1/3] 增量构建（使用 clean 参数可全量重建）"
fi

# 编译打包
echo "[2/3] 编译打包（dpkg-buildpackage）..."
dpkg-buildpackage -us -uc -b 2>&1 | tail -5

# 列出生成的包
echo "[3/3] 生成的安装包:"
ls -lh "$OUTPUT_DIR"/postgresql-18_*.deb "$OUTPUT_DIR"/postgresql-client-18_*.deb "$OUTPUT_DIR"/libpq5_*.deb "$OUTPUT_DIR"/libpq-dev_*.deb "$OUTPUT_DIR"/postgresql-server-dev-18_*.deb 2>/dev/null

echo ""
echo "=========================================="
echo " 出包完成！"
echo " 主安装包: $OUTPUT_DIR/postgresql-18_18.3-1_amd64.deb"
echo "=========================================="
