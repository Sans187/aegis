# Aegis 一键打包：编译后端 + 构建前端 + 组装可部署发布包到 ./release
# 用法（普通 PowerShell）：  ./package.ps1
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

Write-Host "==> [1/3] 编译后端（静态单文件 exe）"
& "$root\server\build.ps1"

Write-Host "==> [2/3] 构建前端"
Push-Location "$root\web"
npm install
npm run build
Pop-Location

Write-Host "==> [3/3] 组装发布包 -> release\"
$rel = "$root\release"
Remove-Item -Recurse -Force $rel -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $rel | Out-Null
Copy-Item "$root\server\build\aegis_server.exe" $rel
Copy-Item -Recurse "$root\server\sql" "$rel\sql"
Copy-Item -Recurse "$root\server\config" "$rel\config"
Copy-Item -Recurse "$root\web\dist" "$rel\web_dist"
# 不要把示例配置当成真实配置；提醒填写
Rename-Item "$rel\config\config.example.json" "config.example.json" -ErrorAction SilentlyContinue

Write-Host "`n==> 完成。发布包在: $rel"
Write-Host "    部署：把 release\ 整个拷到服务器，复制 config\config.example.json 为 config.json 填好数据库，"
Write-Host "    然后运行 aegis_server.exe（无需安装任何运行库）。"
