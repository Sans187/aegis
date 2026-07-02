-- Aegis 数据库 schema (PostgreSQL 16+)
-- 设计目标：多租户统一表 + app_id 分区维度 + pg_trgm 模糊搜索加速
-- 幂等：可重复执行。

CREATE EXTENSION IF NOT EXISTS pg_trgm;     -- 模糊搜索 GIN 索引（解决"查卡慢"）
CREATE EXTENSION IF NOT EXISTS pgcrypto;    -- gen_random_uuid 等

-- ============================ 代理（后台账号） ============================
CREATE TABLE IF NOT EXISTS agents (
    id                    TEXT PRIMARY KEY,
    username              TEXT NOT NULL UNIQUE,
    password_hash         TEXT NOT NULL,                 -- Argon2id 编码串（含盐与参数）
    security_password_hash TEXT,                          -- 二级/安全密码（Argon2id），仅超管用于敏感操作
    must_change_password  BOOLEAN NOT NULL DEFAULT FALSE, -- 强制首次改密（止血用）
    nickname             TEXT,
    level                INTEGER NOT NULL,
    parent_id            TEXT REFERENCES agents(id) ON DELETE SET NULL,
    apps                 TEXT NOT NULL DEFAULT '[]',     -- JSON 数组：可访问的 app_id
    perms                TEXT NOT NULL DEFAULT '[]',     -- JSON 数组：权限键
    card_types           TEXT NOT NULL DEFAULT '{}',     -- JSON 对象 {app_id:[卡种id...]}；无条目=不限该软件卡种
    status               TEXT NOT NULL DEFAULT 'active', -- active / disabled
    last_login_ip        TEXT,
    last_login_ua        TEXT,
    last_login_at        BIGINT,
    password_changed_at  BIGINT,                         -- 用于"早于此时间签发的会话一律失效"
    created_at           BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_agents_parent ON agents(parent_id);
-- 兼容已存在的库：补上新增列（幂等）
ALTER TABLE agents ADD COLUMN IF NOT EXISTS security_password_hash TEXT;
ALTER TABLE agents ADD COLUMN IF NOT EXISTS card_types TEXT NOT NULL DEFAULT '{}';
-- 余额系统：balance=该代理的总额度（由父级分配/调整）；consumed=已被激活永久消耗的金额（只增）。
-- card_types 结构升级为 {app_id:{type_id: 注册卡价格}}，键=可制卡种白名单，值=单价。
ALTER TABLE agents ADD COLUMN IF NOT EXISTS balance  NUMERIC(14,2) NOT NULL DEFAULT 0;
ALTER TABLE agents ADD COLUMN IF NOT EXISTS consumed NUMERIC(14,2) NOT NULL DEFAULT 0;

-- ============================ 应用 ============================
CREATE TABLE IF NOT EXISTS apps (
    id         TEXT PRIMARY KEY,
    owner_id   TEXT NOT NULL,
    name       TEXT NOT NULL,
    mode       TEXT NOT NULL CHECK (mode IN ('card','user')),
    created_at BIGINT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_apps_owner_name ON apps(owner_id, name);
CREATE INDEX IF NOT EXISTS idx_apps_owner ON apps(owner_id);

CREATE TABLE IF NOT EXISTS app_settings (
    app_id     TEXT PRIMARY KEY REFERENCES apps(id) ON DELETE CASCADE,
    settings   JSONB NOT NULL,
    updated_at BIGINT NOT NULL
);

-- ============================ 卡种 ============================
CREATE TABLE IF NOT EXISTS card_types (
    id         TEXT PRIMARY KEY,
    app_id     TEXT NOT NULL,
    name       TEXT NOT NULL,
    hours      INTEGER NOT NULL,
    prefix     TEXT NOT NULL,
    created_at BIGINT NOT NULL
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_card_types_app_prefix ON card_types(app_id, prefix);
CREATE INDEX IF NOT EXISTS idx_card_types_app ON card_types(app_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_card_types_name_trgm ON card_types USING gin (name gin_trgm_ops);

-- ============================ 制卡批次 ============================
CREATE TABLE IF NOT EXISTS card_batches (
    id         TEXT PRIMARY KEY,
    app_id     TEXT NOT NULL,
    type_id    TEXT NOT NULL,
    count      INTEGER NOT NULL,
    note       TEXT,
    maker_id   TEXT NOT NULL,
    maker_name TEXT NOT NULL,
    maker_ip   TEXT,
    created_at BIGINT NOT NULL
);
ALTER TABLE card_batches ADD COLUMN IF NOT EXISTS maker_ip TEXT;
CREATE INDEX IF NOT EXISTS idx_batches_app_maker_created ON card_batches(app_id, maker_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_batches_note_trgm ON card_batches USING gin (note gin_trgm_ops);

-- ============================ 卡密 ============================
CREATE TABLE IF NOT EXISTS cards (
    id         TEXT PRIMARY KEY,
    app_id     TEXT NOT NULL,
    type_id    TEXT NOT NULL,
    code       TEXT NOT NULL,
    status     TEXT NOT NULL,                  -- unused / used / expired
    remark     TEXT,
    frozen     INTEGER NOT NULL DEFAULT 0,
    created_at BIGINT NOT NULL,
    used_at    BIGINT,
    hours      INTEGER,
    expire_at  BIGINT,
    maker_id   TEXT,
    maker_name TEXT,
    batch_id   TEXT,
    price      NUMERIC(14,2) NOT NULL DEFAULT 0   -- 制卡时锁定的单价（注册卡价格），用于余额占用/消耗
);
ALTER TABLE cards ADD COLUMN IF NOT EXISTS price NUMERIC(14,2) NOT NULL DEFAULT 0;
ALTER TABLE cards ADD COLUMN IF NOT EXISTS used_by TEXT;   -- 卡密首次使用时写入的用户名（卡密反查用户）
CREATE UNIQUE INDEX IF NOT EXISTS uq_cards_app_code ON cards(app_id, code);
CREATE INDEX IF NOT EXISTS idx_cards_app_status ON cards(app_id, status);
CREATE INDEX IF NOT EXISTS idx_cards_app_type ON cards(app_id, type_id);
CREATE INDEX IF NOT EXISTS idx_cards_app_batch ON cards(app_id, batch_id);
CREATE INDEX IF NOT EXISTS idx_cards_app_maker_created ON cards(app_id, maker_id, created_at DESC);
-- 模糊搜索加速（卡号、备注）
CREATE INDEX IF NOT EXISTS idx_cards_code_trgm ON cards USING gin (code gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_cards_remark_trgm ON cards USING gin (remark gin_trgm_ops);

-- ============================ 用户（被验证的终端用户） ============================
CREATE TABLE IF NOT EXISTS users (
    id            BIGSERIAL PRIMARY KEY,
    app_id        TEXT NOT NULL,
    username      TEXT NOT NULL,                 -- 卡密模式派生 / 账号模式用户自定义
    password_hash TEXT,                          -- Argon2(卡密 或 账号密码)，绝不明文
    recharges     JSONB NOT NULL DEFAULT '[]',   -- 充值/激活历史 [{card_id,code,hours,type_id,maker_name,at}]
    machine_code  TEXT,
    rebind_cnt    INTEGER NOT NULL DEFAULT 0,
    last_rebind_at BIGINT NOT NULL DEFAULT 0,      -- 上次换绑时间(epoch 秒)，用于换绑冷却
    created_at    BIGINT NOT NULL,
    expired_at    BIGINT NOT NULL,
    frozen        INTEGER NOT NULL DEFAULT 0,
    online        INTEGER NOT NULL DEFAULT 0,
    ip_address    TEXT,
    remark        TEXT,                          -- 备注
    extra         TEXT,                          -- 附属项（预留扩展字段）
    agent_id      TEXT
);
CREATE UNIQUE INDEX IF NOT EXISTS uq_users_app_username ON users(app_id, username);
-- 兼容已有库：补新列、清理废弃列（幂等，无 DO 块以适配按分号切分的迁移器）
ALTER TABLE users ADD COLUMN IF NOT EXISTS password_hash TEXT;
ALTER TABLE users ADD COLUMN IF NOT EXISTS recharges JSONB NOT NULL DEFAULT '[]';
ALTER TABLE users ADD COLUMN IF NOT EXISTS extra TEXT;
ALTER TABLE users ADD COLUMN IF NOT EXISTS last_rebind_at BIGINT NOT NULL DEFAULT 0;
ALTER TABLE users DROP COLUMN IF EXISTS card_id;
ALTER TABLE users DROP COLUMN IF EXISTS type_id;
ALTER TABLE users DROP COLUMN IF EXISTS card_key;
ALTER TABLE users DROP COLUMN IF EXISTS note;
CREATE INDEX IF NOT EXISTS idx_users_app_agent_created ON users(app_id, agent_id, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_users_app_agent_frozen_created ON users(app_id, agent_id, frozen, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_users_app_expired ON users(app_id, expired_at);
CREATE INDEX IF NOT EXISTS idx_users_username_trgm ON users USING gin (username gin_trgm_ops);
-- 模糊搜索加速（机器码/IP/备注/附属项）
CREATE INDEX IF NOT EXISTS idx_users_machine_trgm ON users USING gin (machine_code gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_users_ip_trgm ON users USING gin (ip_address gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_users_remark_trgm ON users USING gin (remark gin_trgm_ops);
CREATE INDEX IF NOT EXISTS idx_users_extra_trgm ON users USING gin (extra gin_trgm_ops);

-- ============================ 终端用户会话 token ============================
CREATE TABLE IF NOT EXISTS tokens (
    id           BIGSERIAL PRIMARY KEY,
    app_id       TEXT NOT NULL,
    username     TEXT NOT NULL,
    token_hash   TEXT NOT NULL UNIQUE,            -- 只存 SHA-256(token)
    device_id    TEXT NOT NULL,
    first_ip     TEXT,
    last_ip      TEXT,
    user_agent   TEXT,
    created_at   BIGINT NOT NULL,
    last_seen_at BIGINT NOT NULL,
    expires_at   BIGINT NOT NULL,
    revoked      INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_tokens_app_username ON tokens(app_id, username);
CREATE INDEX IF NOT EXISTS idx_tokens_app_device ON tokens(app_id, device_id);
CREATE INDEX IF NOT EXISTS idx_tokens_exp ON tokens(expires_at);

-- ============================ 后台会话（管理员/代理） ============================
CREATE TABLE IF NOT EXISTS admin_sessions (
    token_hash TEXT PRIMARY KEY,                  -- 只存 SHA-256(session token)
    agent_id   TEXT NOT NULL,
    ip         TEXT,
    user_agent TEXT,
    created_at BIGINT NOT NULL,
    expires_at BIGINT NOT NULL,
    revoked    INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS idx_admin_sessions_agent ON admin_sessions(agent_id);
CREATE INDEX IF NOT EXISTS idx_admin_sessions_exp ON admin_sessions(expires_at);

-- ============================ 审计日志 ============================
CREATE TABLE IF NOT EXISTS audit_log (
    id         BIGSERIAL PRIMARY KEY,
    actor      TEXT,
    action     TEXT NOT NULL,
    target     TEXT,
    detail     TEXT,
    ip         TEXT,
    created_at BIGINT NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_audit_actor_created ON audit_log(actor, created_at DESC);
CREATE INDEX IF NOT EXISTS idx_audit_action_created ON audit_log(action, created_at DESC);
