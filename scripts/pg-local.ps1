# 本地绿色版 PostgreSQL 管理脚本（免安装、无需管理员，以普通用户进程运行）。
# 用法:
#   ./scripts/pg-local.ps1 init    # 首次：初始化数据目录 + 启动 + 建库建用户建扩展
#   ./scripts/pg-local.ps1 start   # 启动
#   ./scripts/pg-local.ps1 stop    # 停止
#   ./scripts/pg-local.ps1 status  # 状态
#
# 说明：这是开发联调用的本地实例，密码为弱口令，请勿用于生产。
param([Parameter(Mandatory)][ValidateSet('init','start','stop','status')] [string]$Action)
$ErrorActionPreference = "Stop"

$PG_HOME   = "D:\Cpp\pgsql"           # 绿色版 binaries 解压目录
$PGDATA    = "D:\Cpp\pgdata"          # 数据目录
$PORT      = 5432
$SUPERPASS = "CHANGE_ME"              # 超级用户 postgres 密码（开发用，本地自行设置）
$APPUSER   = "aegis"
$APPPASS   = "CHANGE_ME"              # 应用账号密码（开发用，需与 config.json 一致）
$APPDB     = "aegis"

$initdb = Join-Path $PG_HOME "bin\initdb.exe"
$pg_ctl = Join-Path $PG_HOME "bin\pg_ctl.exe"
$psql   = Join-Path $PG_HOME "bin\psql.exe"

function Start-Pg { & $pg_ctl -D $PGDATA -l (Join-Path $PGDATA "server.log") -o "-p $PORT" start }

switch ($Action) {
  'init' {
    if (Test-Path $PGDATA) { Write-Host "PGDATA 已存在，跳过 initdb"; }
    else {
      $pw = New-TemporaryFile
      Set-Content -Path $pw -Value $SUPERPASS -NoNewline
      & $initdb -D $PGDATA -U postgres -A scram-sha-256 --pwfile=$pw -E UTF8 --locale=C
      Remove-Item $pw -Force
    }
    Start-Pg
    Start-Sleep -Seconds 2
    $env:PGPASSWORD = $SUPERPASS
    # 幂等创建用户/库/扩展
    & $psql -U postgres -h 127.0.0.1 -p $PORT -v ON_ERROR_STOP=0 -c "CREATE ROLE $APPUSER LOGIN PASSWORD '$APPPASS';"
    & $psql -U postgres -h 127.0.0.1 -p $PORT -v ON_ERROR_STOP=0 -c "CREATE DATABASE $APPDB OWNER $APPUSER;"
    & $psql -U postgres -h 127.0.0.1 -p $PORT -d $APPDB -c "CREATE EXTENSION IF NOT EXISTS pg_trgm; CREATE EXTENSION IF NOT EXISTS pgcrypto;"
    Write-Host "`n==> PostgreSQL 就绪: 127.0.0.1:$PORT  db=$APPDB user=$APPUSER pass=$APPPASS"
  }
  'start'  { Start-Pg }
  'stop'   { & $pg_ctl -D $PGDATA stop }
  'status' { & $pg_ctl -D $PGDATA status }
}
