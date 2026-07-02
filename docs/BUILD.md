# Aegis 构建与运行（阶段 1：行走骨架）

目标：先把 **vcpkg + Drogon + PostgreSQL** 工具链打通，跑起健康检查接口，
确认环境无误后再灌入业务代码。

## 1. 安装 PostgreSQL
- Windows 安装包：https://www.postgresql.org/download/windows/ （建议 16+）
- 安装后创建库与账号（用 `psql -U postgres`）：

```sql
CREATE USER aegis WITH PASSWORD '你的强密码';
CREATE DATABASE aegis OWNER aegis;
\c aegis
CREATE EXTENSION IF NOT EXISTS pg_trgm;
CREATE EXTENSION IF NOT EXISTS pgcrypto;
```

> `pg_trgm` / `pgcrypto` 需要超管创建扩展。建表本身由程序启动时自动执行 `sql/schema.sql`。

## 2. 准备配置
```bash
cd server/config
copy config.example.json config.json   # 然后填入 db passwd 与 bootstrap_admin
```

## 3. 用 vcpkg 安装依赖并构建
首次会编译 Drogon + libpq + libsodium + cryptopp，耗时较长（10~30 分钟），属正常。

```bash
cd server
# 在“x64 Native Tools Command Prompt for VS 2022”里执行，确保 MSVC 可用
cmake --preset default          # 触发 vcpkg manifest 安装依赖并配置
cmake --build build             # 编译
```

产物：`server/build/aegis_server.exe`（config/ 与 sql/ 会自动拷到旁边）。

## 4. 运行与自检
```bash
cd server/build
aegis_server.exe
```
浏览器或 curl 访问：`http://127.0.0.1:3000/api/health`

期望返回：
```json
{ "ok": true, "service": "aegis", "db": "up" }
```

`db: "up"` 表示 PostgreSQL 连接、建表、引导超管全部成功。
启动日志里应能看到：`schema applied: N statements` 与 `bootstrap admin created`。

## 常见问题
- **找不到 Drogon::Drogon** —— 确认用了 `--preset default`（带 vcpkg 工具链），
  且 `vcpkg.json` 中 `drogon[postgres]` 已安装成功。
- **db: down** —— 检查 `config.json` 的 host/port/user/passwd 与 Postgres 是否在监听 5432。
- **CREATE EXTENSION 权限不足** —— 用超管预先执行第 1 步的 `CREATE EXTENSION`。
