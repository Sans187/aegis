# Aegis 验证系统（重写版）

旧项目 `verifySystem` 因 SQL 注入被攻破，本项目是其**安全重写版**。
技术栈：**Drogon (C++20) + PostgreSQL + Vue3**。TCP 验证协议与旧客户端**完全兼容**，无需重写客户端。

---

## 一、旧系统被攻破的根因分析（务必先读）

### 1. SQL 注入入口
位置：旧 `verifySystem/Controllers/UserCtrl.cc:96-104`

```cpp
auto field = paramsMap.at( "field" );      // ← 直接来自 HTTP 查询参数
sql += " AND " + field + " LIKE ?";        // ← 列名位置裸拼接，注入点
```

其余查询大多用了参数绑定（`LIKE ?`），唯独"搜索字段名"这个列名槽位是裸拼接的。
攻击者把 `field` 构造成子查询/布尔表达式，即可**绕过 `agent_id IN (...)` 的权限过滤**，
进而 dump 出 `agents` 表（含所有代理的 `password_hash`）。

### 2. 为什么"补完注入后，他们还能登录下级代理账号"
旧密码哈希算法（旧 `AuthCtrl.cpp:11`）：

```cpp
md5( password + "s@lt" )   // 固定盐 + 快速哈希
```

固定盐 + MD5，离线**爆破几乎零成本**。攻击者 dump 到哈希后直接还原出明文密码，
所以**即使补掉注入，他们手里的明文密码依然有效**。

> 结论：仅修代码无法止血。必须 ①换强哈希 ②强制所有代理改密 ③吊销全部旧会话/token。
> （改密动作由你手动执行，本系统提供机制：`must_change_password` 标志 + 一键吊销。）

### 3. 第二个后门
旧 `AdminSqlController.cc` 是一个**可从 Web 执行任意 SQL 的接口**，即便有关键词过滤也是巨大攻击面。
**本重写版彻底移除该接口。**

### 4. 数据库慢的真正原因
旧 `config.json`：`"connection_number": 1` —— 所有查询串行挤过单条 SQLite 连接；
且未开 WAL、模糊搜索 `LIKE '%x%'` 全表扫描。**755MB 不是主因，单连接 + 缺索引才是。**

---

## 二、本重写版的安全与性能改造

| 维度 | 旧系统 | Aegis |
|------|--------|-------|
| 密码哈希 | MD5 + 固定盐 | **Argon2id**（libsodium，每用户随机盐） |
| SQL 拼接 | 列名/字段裸拼 | **全参数绑定 + 标识符白名单**（列名、排序只能取自固定枚举） |
| 任意 SQL 接口 | 有（AdminSql） | **删除** |
| 会话 token | 明文比较/固定 | 随机生成，**落库只存 SHA-256**，httpOnly + 过期 + 轮换 |
| 权限过滤 | 可被注入绕过 | 服务端**强制**注入子代理树过滤，不依赖前端字段 |
| 登录防护 | 失败计数 | 失败锁定 + 速率限制 + 审计日志 |
| 数据库 | 单连接 SQLite | **PostgreSQL 连接池** + 复合索引 + **pg_trgm GIN 索引**（模糊搜索提速） |
| 多租户表 | `users_<appid>` 散表 | 统一表 + `app_id` 列 + `(app_id, ...)` 复合索引 |

### "查卡慢"的针对性解法
- 模糊搜索（卡号/备注/用户名 `LIKE '%x%'`）→ **pg_trgm GIN 索引**，大表上从全扫变索引命中。
- 列表分页的过滤维度全部建 **`(app_id, 维度)` 复合索引**。
- PostgreSQL 连接池（默认 N 连接）替代单连接串行。

---

## 三、TCP 验证协议（与旧客户端 100% 兼容，原样保留）

- 帧格式：`[4字节大端长度][payload]`
- payload：`VERIFYSYSTEM|app_id|api_id|<密文>`
- 入站密文：`Base64( RC4( json, key = app_id字符串 ) )`
- 出站密文：`RSA_Base64(rc4key) + "," + Base64( RC4(json, rc4key) )`（RSA 密钥硬编码保持不变）
- app_id：100001–100005
- api：登录 300 / 换绑 301 / 查询 302 / 公告 303 / 更新 304 / 解绑 305；`api>=500` 需 token，500=Verify
- 用户名派生：`card[0:4] + card[-6:]`（长度<10 原样返回）
- 错误码：沿用旧 `ERRO_CODE_*`（见 `src/tcp/Protocol.h`）

> 这些都是客户端契约，**逐字节保留**，所以你的现有客户端不用动。

---

## 四、目录结构

```
Aegis/
├─ server/                 # Drogon C++ 后端
│  ├─ vcpkg.json           # 依赖清单（drogon[postgres]、libsodium、cryptopp…）
│  ├─ CMakeLists.txt
│  ├─ config/config.example.json
│  ├─ sql/schema.sql       # PostgreSQL 建表 + 索引
│  └─ src/
│     ├─ main.cc
│     ├─ db/               # 连接、迁移
│     ├─ security/         # 密码哈希、Crypto、输入校验
│     ├─ tcp/              # TCP 验证子系统（移植自旧 VerifySystem）
│     └─ http/             # 管理后台 API（重写，全部参数化）
└─ web/                    # Vue3 前端（新风格）
```

## 五、前置环境

1. **PostgreSQL 16+**：本机安装或 Docker。建库 `aegis`，启用扩展 `pg_trgm`、`pgcrypto`。
2. **vcpkg**（你已有 `D:\vcpkg`）：用于拉取并编译 `drogon[core,postgres]` 等依赖。
3. **Visual Studio 2022**（MSVC，你已装）或 Ninja。
4. **Node 20+ / npm**（你是 v25，前端 OK）。

构建步骤见 `docs/BUILD.md`（随骨架一起提供）。

---

## 六、开发路线图（分阶段）

- [x] **阶段 0**：旧系统注入根因分析（见上文）
- [x] **阶段 1**：可编译骨架 + 构建系统（vcpkg 静态、Ninja 单配置 Release）。**8.7MB 全静态单文件 exe，零外部 DLL 依赖**
- [x] **阶段 2**：安全地基 —— Argon2id 密码、Crypto 移植、哈希会话/Token、登录锁定、AuthFilter、强制改密
- [x] **阶段 3**：TCP 验证子系统移植（5 App 合并、协议逐字节兼容、Postgres 参数化、Token 改哈希存储、修 2 个旧 bug）
- [x] **阶段 4**：管理后台 API（Auth/Agent/App/Card/User，全参数化 + 列名白名单 + 递归 CTE 强制子树过滤）
- [x] **阶段 5**：Vue3 前端（Vite + Element Plus，新风格；登录/用户/卡密/代理/应用设置/改密）
- [ ] **阶段 6**：接真实 PostgreSQL 联调、压测、安全自检（/security-review）

当前进度：**阶段 1–5 全部完成并编译/运行期验证通过**；待你提供 PostgreSQL 后做阶段 6 联调。
构建：`server/build.ps1`；一键打包发布：`package.ps1`（产出 `release/`）。部署见 `docs/DEPLOY.md`。
