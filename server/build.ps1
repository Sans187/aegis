# Aegis 后端一键构建（静态单文件 Release）。
# 用法：在普通 PowerShell 里运行  ./build.ps1   （脚本会自动进入 VS 开发环境）
$ErrorActionPreference = "Stop"

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -property installationPath
Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"

Set-Location $PSScriptRoot
cmake --preset default
cmake --build build
Write-Host "`n==> 产物: $PSScriptRoot\build\aegis_server.exe"
