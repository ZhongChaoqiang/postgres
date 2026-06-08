# PostgreSQL 18.3 Ubuntu 安装手册

## 1. 概述

本文档介绍如何在 Ubuntu 22.04/24.04 系统上安装 PostgreSQL 18.3 deb 安装包。PostgreSQL 18.3 是一个功能强大的开源对象关系型数据库系统，本安装包基于源码编译构建，已内置 pgvector 向量扩展，无需额外安装。

### 1.1 安装包列表

| 包名 | 说明 | 大小 |
|------|------|------|
| `libpq5_18.3-1_amd64.deb` | PostgreSQL 客户端共享库 | 167KB |
| `libpq-dev_18.3-1_amd64.deb` | libpq 开发头文件和静态库 | 21KB |
| `postgresql-client-18_18.3-1_amd64.deb` | 客户端工具（psql, pg_dump 等） | 759KB |
| `postgresql-18_18.3-1_amd64.deb` | 数据库服务器主程序（含 pgvector） | 5.8MB |
| `postgresql-server-dev-18_18.3-1_amd64.deb` | 服务端开发头文件 | 1.5MB |
| `postgresql-doc-18_18.3-1_all.deb` | 扩展文档 | 3.6KB |

### 1.2 内置扩展

本安装包已内置以下扩展，初始化数据库时自动创建：

| 扩展 | 版本 | 说明 |
|------|------|------|
| plpgsql | 1.0 | PL/pgSQL 过程语言（默认） |
| vector | 0.8.2 | pgvector 向量相似度搜索 |
| jolix_predict | 1.0 | AI 预测扩展 |
| jolix_embedding | 1.0 | 向量嵌入扩展（依赖 pgvector） |

### 1.3 依赖关系

```
postgresql-18 依赖:
  ├── postgresql-client-18
  ├── libpq5 (>= 18~)
  └── ssl-cert

postgresql-client-18 依赖:
  └── libpq5 (>= 18~)

libpq-dev 依赖:
  └── libpq5

postgresql-server-dev-18 依赖:
  ├── libpq-dev
  └── postgresql-common
```

## 2. 系统要求

### 2.1 操作系统

- Ubuntu 22.04 LTS (Jammy Jellyfish) 或更高版本
- Ubuntu 24.04 LTS (Noble Numbat)

### 2.2 硬件要求

| 组件 | 最低要求 | 推荐配置 |
|------|---------|---------|
| CPU | 1 核 | 4 核+ |
| 内存 | 512MB | 4GB+ |
| 磁盘 | 500MB | 10GB+ (取决于数据量) |

### 2.3 软件依赖

安装前需确保以下依赖已安装：

```bash
sudo apt-get update
sudo apt-get install -y \
    libreadline8 \
    zlib1g \
    libssl3 \
    libpam0g \
    libxml2 \
    libxslt1.1 \
    libicu70 \
    liblz4-1 \
    libzstd1 \
    libsystemd0 \
    ssl-cert
```

> **注意**：本安装包已内置 pgvector，无需额外安装 `postgresql-18-pgvector`。
> 如果系统已安装官方 `postgresql-18-pgvector` 包，请先卸载，否则会因 ABI 不兼容导致 vector 扩展无法加载。

## 3. 安装步骤

### 3.1 方法一：使用 deb 包安装（推荐）

#### 3.1.1 卸载冲突包（如已安装官方 PostgreSQL）

```bash
# 停止现有服务
sudo systemctl stop postgresql 2>/dev/null
sudo pkill -u postgres 2>/dev/null

# 卸载官方 PostgreSQL 18 相关包
sudo dpkg --purge postgresql-18-pgvector 2>/dev/null
sudo dpkg --purge postgresql-18-jit 2>/dev/null
sudo dpkg --purge postgresql-18 2>/dev/null
sudo dpkg --purge postgresql-client-18 2>/dev/null
```

#### 3.1.2 将安装包复制到目标服务器

```bash
# 创建临时目录
mkdir -p /tmp/pg18-install

# 将所有 deb 包复制到该目录
cp libpq5_18.3-1_amd64.deb /tmp/pg18-install/
cp libpq-dev_18.3-1_amd64.deb /tmp/pg18-install/
cp postgresql-client-18_18.3-1_amd64.deb /tmp/pg18-install/
cp postgresql-18_18.3-1_amd64.deb /tmp/pg18-install/
cp postgresql-server-dev-18_18.3-1_amd64.deb /tmp/pg18-install/
cp postgresql-doc-18_18.3-1_all.deb /tmp/pg18-install/
```

#### 3.1.3 按顺序安装各组件

```bash
cd /tmp/pg18-install

# 1. 安装客户端共享库（基础依赖）
sudo dpkg -i libpq5_18.3-1_amd64.deb

# 2. 安装客户端工具
sudo dpkg -i --force-overwrite postgresql-client-18_18.3-1_amd64.deb

# 3. 安装数据库服务器（含 pgvector）
sudo dpkg -i --force-overwrite postgresql-18_18.3-1_amd64.deb

# 4. （可选）安装开发头文件
sudo dpkg -i --force-overwrite libpq-dev_18.3-1_amd64.deb
sudo dpkg -i --force-overwrite postgresql-server-dev-18_18.3-1_amd64.deb

# 5. （可选）安装文档
sudo dpkg -i postgresql-doc-18_18.3-1_all.deb
```

#### 3.1.4 修复可能的依赖问题

```bash
sudo apt-get install -f
```

#### 3.1.5 一键安装（替代方案）

```bash
cd /tmp/pg18-install
sudo dpkg -i --force-overwrite *.deb
sudo apt-get install -f
```

### 3.2 方法二：从源码构建安装

#### 3.2.1 安装编译依赖

```bash
sudo apt-get update
sudo apt-get install -y \
    debhelper \
    libreadline-dev \
    zlib1g-dev \
    libssl-dev \
    libpam0g-dev \
    libxml2-dev \
    libxslt1-dev \
    libicu-dev \
    liblz4-dev \
    libzstd-dev \
    bison \
    flex \
    pkg-config \
    meson \
    ninja-build \
    dpkg-dev \
    fakeroot \
    libsystemd-dev \
    python3-dev \
    libcurl4-openssl-dev
```

> **注意**：pgvector 已包含在源码 `contrib/pgvector` 目录中，无需额外安装。

#### 3.2.2 构建 deb 包

```bash
cd /path/to/postgres/source

# 清理旧的构建
make distclean 2>/dev/null || true

# 设置文件权限
chmod +x debian/rules
chmod -x debian/*.install

# 构建包
dpkg-buildpackage -us -uc -j$(nproc) -b
```

#### 3.2.3 安装构建产物

```bash
cd ..
sudo dpkg -i --force-overwrite libpq5_18.3-1_amd64.deb
sudo dpkg -i --force-overwrite postgresql-client-18_18.3-1_amd64.deb
sudo dpkg -i --force-overwrite postgresql-18_18.3-1_amd64.deb
sudo apt-get install -f
```

## 4. 初始化和配置

### 4.1 创建数据目录

```bash
# 安装脚本会自动创建 postgres 用户和基本目录
# 如需手动创建：
sudo mkdir -p /var/lib/postgresql/18/main
sudo chown postgres:postgres /var/lib/postgresql/18/main

sudo mkdir -p /var/log/postgresql
sudo chown postgres:postgres /var/log/postgresql

sudo mkdir -p /var/run/postgresql
sudo chown postgres:postgres /var/run/postgresql
```

### 4.2 初始化数据库集群

初始化时会自动创建 vector、jolix_predict、jolix_embedding 扩展。

```bash
# 切换到 postgres 用户
sudo -u postgres initdb -D /var/lib/postgresql/18/main

# 或者使用自定义编码和区域设置
sudo -u postgres initdb -D /var/lib/postgresql/18/main \
    --encoding=UTF-8 \
    --locale=C.UTF-8 \
    --data-checksums
```

初始化成功后会看到：

```
Success. You can now start the database server using:

    pg_ctl -D /var/lib/postgresql/18/main -l logfile start
```

### 4.3 配置 PostgreSQL

#### 4.3.1 编辑 postgresql.conf

```bash
sudo nano /var/lib/postgresql/18/main/postgresql.conf
```

关键配置项：

```ini
# 监听地址（0.0.0.0 表示监听所有地址）
listen_addresses = 'localhost'

# 端口
port = 5432

# 最大连接数
max_connections = 100

# 共享缓冲区（建议设为系统内存的 25%）
shared_buffers = 128MB

# 工作内存
work_mem = 4MB

# 维护工作内存
maintenance_work_mem = 64MB

# WAL 配置
wal_level = replica
max_wal_size = 1GB
min_wal_size = 80MB

# 日志配置
logging_collector = on
log_directory = 'log'
log_filename = 'postgresql-%Y-%m-%d.log'
log_statement = 'mod'
```

#### 4.3.2 编辑 pg_hba.conf（客户端认证）

```bash
sudo nano /var/lib/postgresql/18/main/pg_hba.conf
```

示例配置：

```
# TYPE  DATABASE        USER            ADDRESS                 METHOD
# 本地连接
local   all             all                                     peer
# IPv4 本地连接
host    all             all             127.0.0.1/32            scram-sha-256
# IPv6 本地连接
host    all             all             ::1/128                 scram-sha-256
# 允许特定网段连接（按需配置）
# host    all             all             192.168.1.0/24          scram-sha-256
```

### 4.4 配置 systemd 服务

#### 4.4.1 创建 systemd 服务文件

```bash
sudo nano /etc/systemd/system/postgresql-18.service
```

内容如下：

```ini
[Unit]
Description=PostgreSQL 18 database server
Documentation=man:postgres(1)
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
User=postgres
ExecStart=/usr/lib/postgresql/18/bin/postgres -D /var/lib/postgresql/18/main
ExecReload=/bin/kill -HUP $MAINPID
KillMode=mixed
KillSignal=SIGINT
TimeoutSec=infinity
OOMScoreAdjust=-1000

[Install]
WantedBy=multi-user.target
```

#### 4.4.2 启用并启动服务

```bash
sudo systemctl daemon-reload
sudo systemctl enable postgresql-18
sudo systemctl start postgresql-18
```

#### 4.4.3 检查服务状态

```bash
sudo systemctl status postgresql-18
```

## 5. 验证安装

### 5.1 检查版本

```bash
postgres --version
psql --version
```

### 5.2 连接数据库

```bash
sudo -u postgres psql
```

### 5.3 验证扩展

```sql
-- 查看已安装的扩展
SELECT extname, extversion FROM pg_extension ORDER BY extname;

-- 预期输出：
--  extname     | extversion
-- -------------+------------
--  jolix_embedding | 1.0
--  jolix_predict   | 1.0
--  plpgsql         | 1.0
--  vector          | 0.8.2
```

### 5.4 执行基本查询

```sql
-- 查看版本
SELECT version();

-- 查看数据库列表
\l

-- 创建测试数据库
CREATE DATABASE testdb;

-- 连接到测试数据库
\c testdb

-- 创建测试表
CREATE TABLE test_table (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100),
    created_at TIMESTAMP DEFAULT NOW()
);

-- 插入测试数据
INSERT INTO test_table (name) VALUES ('Hello PostgreSQL 18');

-- 查询数据
SELECT * FROM test_table;

-- 退出
\q
```

### 5.5 验证 pgvector 向量功能

```sql
-- 创建向量测试表
CREATE TABLE items (id serial PRIMARY KEY, embedding vector(3));

-- 插入向量数据
INSERT INTO items (embedding) VALUES ('[1,2,3]'), ('[4,5,6]'), ('[7,8,9]');

-- 查询向量数据
SELECT * FROM items;

-- 向量相似度查询（L2 距离）
SELECT id, embedding <=> '[3,1,2]'::vector AS distance
FROM items ORDER BY distance;

-- 创建 HNSW 索引加速查询
CREATE INDEX ON items USING hnsw (embedding vector_l2_ops);

-- 向量内积相似度
SELECT id, embedding <#> '[3,1,2]'::vector AS distance
FROM items ORDER BY distance;

-- 余弦相似度
SELECT id, embedding <=> '[3,1,2]'::vector AS distance
FROM items ORDER BY distance;
```

## 6. 扩展使用

### 6.1 pgvector 向量扩展（已内置）

pgvector 已内置在安装包中，初始化数据库时自动创建。支持的功能：

- **向量类型**：`vector(N)` - N 维浮点向量
- **距离运算符**：
  - `<=>` - L2 距离（欧几里得距离）
  - `<#>` - 内积
  - `<=>` - 余弦距离
- **索引类型**：
  - HNSW 索引（推荐）：`CREATE INDEX ON table USING hnsw (column vector_l2_ops);`
  - IVFFlat 索引：`CREATE INDEX ON table USING ivfflat (column vector_l2_ops);`

如需在新建数据库中手动创建：

```sql
CREATE EXTENSION IF NOT EXISTS vector SCHEMA public;
```

### 6.2 jolix_predict 扩展（已内置）

AI 预测扩展，初始化数据库时自动创建。

```sql
-- 如需手动创建
CREATE EXTENSION IF NOT EXISTS jolix_predict;
```

### 6.3 jolix_embedding 扩展（已内置）

向量嵌入扩展，依赖 pgvector，初始化数据库时自动创建。

```sql
-- 如需手动创建（需先确保 vector 扩展已创建）
CREATE EXTENSION IF NOT EXISTS jolix_embedding;
```

### 6.4 jolix_vectorize 扩展

自动向量化扩展，需手动创建。

```sql
CREATE EXTENSION IF NOT EXISTS jolix_vectorize;
```

### 6.5 其他内置扩展

以下扩展随 PostgreSQL 一起安装，可按需创建：

```sql
-- pg_stat_statements 性能统计
CREATE EXTENSION pg_stat_statements;

-- pgcrypto 加密函数
CREATE EXTENSION pgcrypto;

-- hstore 键值存储
CREATE EXTENSION hstore;

-- uuid-ossp UUID 生成
CREATE EXTENSION "uuid-ossp";

-- pg_trgm 模糊匹配
CREATE EXTENSION pg_trgm;
```

## 7. 日常运维

### 7.1 启动/停止/重启

```bash
# 使用 systemd
sudo systemctl start postgresql-18
sudo systemctl stop postgresql-18
sudo systemctl restart postgresql-18
sudo systemctl reload postgresql-18

# 使用 pg_ctl
sudo -u postgres pg_ctl -D /var/lib/postgresql/18/main start
sudo -u postgres pg_ctl -D /var/lib/postgresql/18/main stop
sudo -u postgres pg_ctl -D /var/lib/postgresql/18/main restart
sudo -u postgres pg_ctl -D /var/lib/postgresql/18/main reload
```

### 7.2 备份与恢复

```bash
# 备份单个数据库
sudo -u postgres pg_dump dbname > dbname_backup.sql

# 备份所有数据库
sudo -u postgres pg_dumpall > all_backup.sql

# 恢复数据库
sudo -u postgres psql dbname < dbname_backup.sql

# 自定义格式备份（推荐）
sudo -u postgres pg_dump -Fc dbname > dbname_backup.dump
sudo -u postgres pg_restore -d dbname dbname_backup.dump
```

### 7.3 日志查看

```bash
# 查看最新日志
sudo tail -f /var/lib/postgresql/18/main/log/postgresql-$(date +%Y-%m-%d).log

# 查看 systemd 日志
sudo journalctl -u postgresql-18 -f
```

### 7.4 性能监控

```sql
-- 查看活跃连接
SELECT * FROM pg_stat_activity;

-- 查看数据库大小
SELECT pg_size_pretty(pg_database_size(datname)) AS size, datname
FROM pg_database ORDER BY pg_database_size(datname) DESC;

-- 查看表大小
SELECT relname, pg_size_pretty(pg_total_relation_size(relid)) AS total_size
FROM pg_catalog.pg_statio_user_tables
ORDER BY pg_total_relation_size(relid) DESC;

-- 查看慢查询（需要启用 pg_stat_statements）
SELECT query, calls, total_exec_time, mean_exec_time
FROM pg_stat_statements
ORDER BY mean_exec_time DESC
LIMIT 10;
```

## 8. 卸载

### 8.1 停止服务

```bash
sudo systemctl stop postgresql-18
sudo systemctl disable postgresql-18
```

### 8.2 卸载软件包

```bash
# 卸载但保留配置和数据
sudo dpkg -r postgresql-18 postgresql-client-18 postgresql-server-dev-18 \
    libpq-dev postgresql-doc-18

# 完全卸载（包括配置）
sudo dpkg --purge postgresql-18 postgresql-client-18 postgresql-server-dev-18 \
    libpq-dev postgresql-doc-18 libpq5
```

### 8.3 清理数据（谨慎操作！）

```bash
# 删除数据目录
sudo rm -rf /var/lib/postgresql/18

# 删除日志
sudo rm -rf /var/log/postgresql

# 删除运行时目录
sudo rm -rf /var/run/postgresql

# 删除 postgres 用户（如果不再需要）
sudo userdel postgres
```

## 9. 常见问题

### 9.1 vector 扩展加载失败

```
ERROR: could not load library "/usr/lib/postgresql/18/lib/vector.so": undefined symbol: LWLockRegisterTranche
```

**原因**：系统安装了官方 `postgresql-18-pgvector` 包，其编译的 vector.so 与本安装包的 PostgreSQL ABI 不兼容。

**解决方案：**
```bash
# 卸载官方 pgvector 包
sudo dpkg --remove postgresql-18-pgvector

# 确认使用本安装包内置的 vector.so
ls -la /usr/lib/postgresql/18/lib/vector.so
```

### 9.2 连接被拒绝

```
psql: error: connection to server on socket "/var/run/postgresql/.s.PGSQL.5432" failed
```

**解决方案：**
1. 检查 PostgreSQL 服务是否运行：`sudo systemctl status postgresql-18`
2. 检查 socket 目录权限：`ls -la /var/run/postgresql/`
3. 检查 `postgresql.conf` 中的 `unix_socket_directories` 配置

### 9.3 认证失败

```
psql: error: FATAL: password authentication failed for user "postgres"
```

**解决方案：**
1. 修改 `pg_hba.conf`，临时将认证方式改为 `trust`
2. 重启服务后修改密码：`ALTER USER postgres PASSWORD 'new_password';`
3. 恢复 `pg_hba.conf` 中的认证方式为 `scram-sha-256`

### 9.4 共享内存不足

```
FATAL: could not map anonymous shared memory: Cannot allocate memory
```

**解决方案：**
1. 减小 `shared_buffers` 的值
2. 增加系统共享内存：`sudo sysctl -w kernel.shmmax=268435456`

### 9.5 端口被占用

```
FATAL: could not create any TCP/IP sockets
```

**解决方案：**
1. 检查端口占用：`sudo lsof -i :5432`
2. 修改 `postgresql.conf` 中的 `port` 配置
3. 停止占用端口的进程

### 9.6 依赖缺失

```
dpkg: dependency problems prevent configuration of postgresql-18
```

**解决方案：**
```bash
sudo apt-get install -f
```

### 9.7 与官方 PostgreSQL 包冲突

**解决方案：**
```bash
# 完全卸载官方 PostgreSQL 18 包
sudo dpkg --purge postgresql-18-pgvector postgresql-18-jit \
    postgresql-18 postgresql-client-18

# 然后安装本安装包
sudo dpkg -i --force-overwrite libpq5_18.3-1_amd64.deb \
    postgresql-client-18_18.3-1_amd64.deb \
    postgresql-18_18.3-1_amd64.deb
```

## 10. 安全建议

1. **修改默认密码**：安装后立即修改 postgres 用户密码
2. **限制监听地址**：生产环境不要使用 `listen_addresses = '*'`
3. **使用 SSL**：启用 SSL 加密客户端连接
4. **配置防火墙**：只开放必要的端口
5. **定期备份**：设置自动备份策略
6. **更新补丁**：定期检查并安装安全更新
7. **审计日志**：启用 `log_statement = 'all'` 记录所有 SQL 操作
8. **最小权限原则**：为应用创建专用用户，只授予必要权限

## 11. 附录

### 11.1 编译选项

本安装包使用以下编译选项：

| 选项 | 值 | 说明 |
|------|-----|------|
| ICU | enabled | 国际化支持 |
| OpenSSL | enabled | SSL/TLS 加密 |
| PAM | enabled | PAM 认证 |
| libxml | enabled | XML 支持 |
| libxslt | enabled | XSL 转换 |
| LZ4 | enabled | LZ4 压缩 |
| ZSTD | enabled | Zstandard 压缩 |
| NLS | enabled | 多语言支持 |
| PL/Perl | auto | Perl 过程语言 |
| PL/Python | auto | Python 过程语言 |
| PL/Tcl | auto | Tcl 过程语言 |
| pgvector | 0.8.2 | 向量相似度搜索（内置） |

### 11.2 文件位置

本安装包使用 PostgreSQL 标准 Ubuntu 路径布局：

| 路径 | 说明 |
|------|------|
| `/usr/lib/postgresql/18/bin/postgres` | 服务器主程序 |
| `/usr/lib/postgresql/18/bin/psql` | 命令行客户端 |
| `/usr/bin/postgres` | 服务器主程序符号链接 |
| `/usr/bin/psql` | 客户端符号链接 |
| `/usr/lib/postgresql/18/lib/` | 扩展模块目录 |
| `/usr/lib/postgresql/18/lib/vector.so` | pgvector 扩展 |
| `/usr/lib/postgresql/18/lib/pgxs/` | 扩展构建框架 |
| `/usr/lib/x86_64-linux-gnu/libpq.so.5` | 客户端共享库 |
| `/usr/include/postgresql/` | 头文件 |
| `/usr/share/postgresql/18/` | 数据文件 |
| `/usr/share/postgresql/18/extension/` | 扩展定义文件 |
| `/var/lib/postgresql/18/main/` | 数据目录 |
| `/var/log/postgresql/` | 日志目录 |
| `/var/run/postgresql/` | 运行时目录 |

### 11.3 相关链接

- PostgreSQL 官方网站：https://www.postgresql.org/
- PostgreSQL 18 文档：https://www.postgresql.org/docs/18/
- pgvector 扩展：https://github.com/pgvector/pgvector
