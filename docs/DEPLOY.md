# Aegis 部署指南

## 一、打包（开发机上执行一次）

```powershell
cd D:\Cpp\Aegis
./package.ps1
```

产物在 `release\`：

```
release/
├─ aegis_server.exe      # 全静态单文件，无需安装任何运行库
├─ config/
│  └─ config.example.json
├─ sql/
│  └─ schema.sql
└─ web_dist/             # 前端静态资源（由 exe 内置 HTTP 托管）
```

## 二、服务器上部署（目标机什么库都不用装）

1. 把整个 `release\` 拷到服务器任意目录。
2. 准备一个 PostgreSQL（本机或远程都行，连接信息填进配置即可）：
   ```sql
   CREATE USER aegis WITH PASSWORD '强密码';
   CREATE DATABASE aegis OWNER aegis;
   \c aegis
   CREATE EXTENSION IF NOT EXISTS pg_trgm;
   CREATE EXTENSION IF NOT EXISTS pgcrypto;
   ```
3. 复制 `config\config.example.json` 为 `config\config.json`，填写：
   - `db_clients[0]` 的 host/port/dbname/user/passwd
   - `custom_config.bootstrap_admin` 的首个超管用户名/密码（仅首次建库时用于创建）
4. 运行：
   ```
   aegis_server.exe
   ```
   - HTTP 后台 + API：`http://<服务器IP>:3000`
   - TCP 验证服务：端口 `9002`（`custom_config.tcp_verify_port` 可改）
   - 启动时自动建表、种入 5 个应用、创建首个超管。

> exe 仅依赖 Windows 系统自带 DLL（kernel32/ws2_32/crypt32 等），
> 不需要 vcredist、不需要 openssl/libpq 等任何第三方库。

## 三、首次登录与止血

1. 浏览器打开 `http://<服务器IP>:3000`，用 bootstrap 超管登录。
2. 系统会**强制修改密码**（`must_change_password`），改完才能进入。
3. 在「代理管理」里为每个下级代理**重置密码**——这会把旧密码作废并吊销其全部会话，
   彻底堵住旧系统泄露的明文密码。

## 四、端口与反向代理（可选）

- 生产建议在前面挂 Nginx/Caddy 做 HTTPS 终止，反代到 `127.0.0.1:3000`。
- TCP 9002 直接对客户端暴露（与旧系统一致）。

## 五、备份

- 只需备份 PostgreSQL 数据库（`pg_dump aegis`）。前端/exe 无状态。
