#!/bin/bash
# PostgreSQL 18 一键编译出包脚本
# 用法: ./build_deb.sh [clean]
#   clean: 清理旧构建后重新编译
#
# 说明:
#   - 自动从本地 D:\workspace\pgvector 同步 pgvector 0.8.2 源码到 contrib/pgvector
#   - pgvector 需要单独的 meson.build 文件（已维护在 contrib/pgvector/meson.build）
#   - 由于社区 pgvector 与 PG18 存在 API 不兼容，使用本地维护的兼容版本
#   - 构建在 ext4 文件系统（/tmp/pg-build）上进行，避免 NTFS 权限问题

set -e

SOURCE_DIR="/mnt/d/workspace/postgres"
PGVECTOR_SOURCE_DIR="/mnt/d/workspace/pgvector"
OUTPUT_DIR="/mnt/d/workspace"
BUILD_DIR="/tmp/pg-build"

echo "=========================================="
echo " PostgreSQL 18 一键编译出包"
echo "=========================================="

cd "$SOURCE_DIR"

# 步骤 0: 同步本地 pgvector 源码到 contrib/pgvector
echo "[0/4] 同步本地 pgvector 源码到 contrib/pgvector..."
if [ -d "$PGVECTOR_SOURCE_DIR" ]; then
    # 备份 meson.build（pgvector 源码中不包含此文件，由 postgres 仓库维护）
    MESON_BUILD_BACKUP=""
    if [ -f "$SOURCE_DIR/contrib/pgvector/meson.build" ]; then
        MESON_BUILD_BACKUP=$(mktemp)
        cp "$SOURCE_DIR/contrib/pgvector/meson.build" "$MESON_BUILD_BACKUP"
    fi

    sudo rm -rf "$SOURCE_DIR/contrib/pgvector"
    cp -r "$PGVECTOR_SOURCE_DIR" "$SOURCE_DIR/contrib/pgvector"
    # 清理编译产物
    rm -f "$SOURCE_DIR/contrib/pgvector"/src/*.o \
          "$SOURCE_DIR/contrib/pgvector"/src/*.bc \
          "$SOURCE_DIR/contrib/pgvector"/vector.so \
          "$SOURCE_DIR/contrib/pgvector"/sql/vector--0.8.4.sql 2>/dev/null || true

    # 恢复 meson.build
    if [ -n "$MESON_BUILD_BACKUP" ]; then
        cp "$MESON_BUILD_BACKUP" "$SOURCE_DIR/contrib/pgvector/meson.build"
        rm -f "$MESON_BUILD_BACKUP"
    fi

    echo "  pgvector 源码已同步到 contrib/pgvector"
    echo "  pgvector 版本: $(grep '^EXTVERSION' "$PGVECTOR_SOURCE_DIR/Makefile" | awk '{print $3}')"
else
    echo "  警告: 本地 pgvector 源码目录 $PGVECTOR_SOURCE_DIR 不存在"
    echo "  跳过 pgvector 同步（将使用 contrib/pgvector 中已有的代码）"
fi

# 确认 meson.build 存在
if [ ! -f "$SOURCE_DIR/contrib/pgvector/meson.build" ]; then
    echo "  错误: contrib/pgvector/meson.build 不存在，无法构建 pgvector"
    exit 1
fi

# 步骤 1: 清理旧构建
if [ "$1" = "clean" ]; then
    echo "[1/4] 清理旧构建..."
    sudo rm -rf "$BUILD_DIR"
    rm -f "$OUTPUT_DIR"/*.deb "$OUTPUT_DIR"/*.buildinfo "$OUTPUT_DIR"/*.changes
    sudo rm -rf "$SOURCE_DIR/debian/build" "$SOURCE_DIR/debian/tmp" \
             "$SOURCE_DIR/debian/.debhelper" \
             "$SOURCE_DIR/debian/postgresql-18" "$SOURCE_DIR/debian/postgresql-client-18" \
             "$SOURCE_DIR/debian/libpq5" "$SOURCE_DIR/debian/libpq-dev" \
             "$SOURCE_DIR/debian/postgresql-server-dev-18" "$SOURCE_DIR/debian/postgresql-doc-18" \
             "$SOURCE_DIR/debian/libecpg6" "$SOURCE_DIR/debian/libecpg-dev" \
             "$SOURCE_DIR/debian/files" "$SOURCE_DIR/debian/substvars" \
             "$SOURCE_DIR/debian/debhelper-build-stamp"
    echo "  已清理"
else
    echo "[1/4] 增量构建（使用 clean 参数可全量重建）"
fi

# 步骤 2: 准备构建目录（复制到 ext4 文件系统避免 NTFS 权限问题）
echo "[2/4] 准备构建目录..."
sudo rm -rf "$BUILD_DIR"
sudo mkdir -p "$BUILD_DIR"
# 使用 git archive 导出已跟踪的文件，然后手动复制 debian/ 和 contrib/pgvector/
cd "$SOURCE_DIR"
git archive --format=tar HEAD | sudo tar -xf - -C "$BUILD_DIR/"
# 复制 debian 目录（不在 git 中）
sudo cp -r "$SOURCE_DIR/debian" "$BUILD_DIR/"
sudo rm -rf "$BUILD_DIR/debian/build" "$BUILD_DIR/debian/tmp" "$BUILD_DIR/debian/.debhelper" \
         "$BUILD_DIR/debian/postgresql-18" "$BUILD_DIR/debian/postgresql-client-18" \
         "$BUILD_DIR/debian/libpq5" "$BUILD_DIR/debian/libpq-dev" \
         "$BUILD_DIR/debian/postgresql-server-dev-18" "$BUILD_DIR/debian/postgresql-doc-18" \
         "$BUILD_DIR/debian/libecpg6" "$BUILD_DIR/debian/libecpg-dev" \
         "$BUILD_DIR/debian/files" "$BUILD_DIR/debian/substvars" \
         "$BUILD_DIR/debian/debhelper-build-stamp"
# 复制 pgvector（不在 git 中）
sudo cp -r "$SOURCE_DIR/contrib/pgvector" "$BUILD_DIR/contrib/"
# 修复文件所有权
sudo chown -R "$(whoami):$(whoami)" "$BUILD_DIR"
echo "  构建目录已准备好: $BUILD_DIR"

# 步骤 3: 编译打包
echo "[3/4] 编译打包（dpkg-buildpackage）..."
cd "$BUILD_DIR"
dpkg-buildpackage -us -uc -b 2>&1 | tail -20

# 复制生成的 deb 包到输出目录
echo "  复制 deb 包到输出目录..."
sudo cp /tmp/postgresql-18_*.deb /tmp/postgresql-client-18_*.deb \
        /tmp/libpq5_*.deb /tmp/libpq-dev_*.deb \
        /tmp/postgresql-server-dev-18_*.deb /tmp/postgresql-doc-18_*.deb \
        /tmp/*.buildinfo /tmp/*.changes \
        "$OUTPUT_DIR/" 2>/dev/null || true
sudo chown "$(whoami):$(whoami)" "$OUTPUT_DIR"/*.deb 2>/dev/null || true

# 步骤 4: 列出生成的包
echo "[4/4] 生成的安装包:"
ls -lh "$OUTPUT_DIR"/postgresql-18_*.deb "$OUTPUT_DIR"/postgresql-client-18_*.deb \
       "$OUTPUT_DIR"/libpq5_*.deb "$OUTPUT_DIR"/libpq-dev_*.deb \
       "$OUTPUT_DIR"/postgresql-server-dev-18_*.deb 2>/dev/null

echo ""
echo "=========================================="
echo " 出包完成！"
echo " 主安装包: $OUTPUT_DIR/postgresql-18_18.3-2_amd64.deb"
echo ""
echo " 安装命令:"
echo "   sudo dpkg -i $OUTPUT_DIR/libpq5_*.deb \\"
echo "               $OUTPUT_DIR/postgresql-client-18_*.deb \\"
echo "               $OUTPUT_DIR/postgresql-18_*.deb"
echo "=========================================="
