# 打包发布版：把 BoardMark 拷成一份「拷到任何电脑都能跑」的文件夹。
#
# 为什么要写这个脚本
# ------------------
# 只跑 windeployqt 是不够的。实测踩到的坑：
#   * windeployqt 只拷 Qt 自己的 DLL，**不拷它们的第三方依赖**
#     （libicuuc78、libpcre2-16-0、libzstd、freetype、harfbuzz ……）
#   * 更不拷 MinGW 运行时（libgcc_s_seh-1、libstdc++-6、libwinpthread-1）
#   结果就是：开发机上因为 PATH 里有 MinGW 的 bin 目录，一切正常；
#   拷到别人电脑上直接报「由于找不到 libgcc_s_seh-1.dll，无法继续执行代码」。
#
# 所以这个脚本做三件事：
#   1. 拷 exe、跑 windeployqt
#   2. **反复扫描依赖直到收敛** —— 缺什么就从 MinGW 的 bin 里补什么
#   3. 用「清空 PATH」的方式验证一遍，确认真的能独立运行

param(
    [string]$OutDir = (Join-Path $PSScriptRoot 'dist'),
    [string]$QtBin  = 'E:\Environment\mingw-w64\ucrt64\bin',
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    # 把本机的 settings.json（**含 API Key**）一起拷进发布版。
    # 自己拿去教室测试时加上这个开关，省得在那边重新填一遍；
    # 要分给别人用就别加 —— 那是你的私人凭据。
    [switch]$IncludeSettings
)

$ErrorActionPreference = 'Stop'
$env:PATH = "$QtBin;$env:PATH"

$exe = Join-Path $BuildDir 'BoardMark.exe'
if (-not (Test-Path $exe)) {
    throw "找不到 $exe —— 请先编译。"
}
if (-not (Test-Path (Join-Path $QtBin 'windeployqt.exe'))) {
    throw "在 $QtBin 里找不到 windeployqt.exe"
}

Write-Host "== 1/4 准备目录 ==" -ForegroundColor Cyan
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory $OutDir | Out-Null
Copy-Item $exe $OutDir
Write-Host "  已放入 BoardMark.exe"

Write-Host "== 2/4 收集 Qt 依赖 ==" -ForegroundColor Cyan
# windeployqt 会往 stderr 写警告（dxcompiler 之类），而 $ErrorActionPreference='Stop'
# 会把「原生命令的 stderr」当成致命错误。这里临时放开，否则脚本会莫名中止。
$savedEAP = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& (Join-Path $QtBin 'windeployqt.exe') --release --no-translations `
    --no-system-d3d-compiler --no-opengl-sw (Join-Path $OutDir 'BoardMark.exe') 2>&1 |
    Where-Object { $_ -notmatch 'dxcompiler|not a problem' } | Out-Null
$ErrorActionPreference = $savedEAP
Write-Host "  windeployqt 完成"

# --- 工具函数 ---
function Get-ImportNames([string]$Path) {
    # 用 objdump 读 PE 的导入表
    & (Join-Path $QtBin 'objdump.exe') -p $Path 2>$null |
        Select-String 'DLL Name:' |
        ForEach-Object { ($_.Line -replace '.*DLL Name:\s*', '').Trim() } |
        Where-Object { $_ }
}

function Test-IsSystemDll([string]$Name) {
    # API 集（api-ms-win-*、ext-ms-*）在文件系统里查不到，由加载器解析，
    # 属于系统的一部分，不用拷。
    if ($Name -match '^(?i)(api-ms-win|ext-ms-)') { return $true }
    return (Test-Path (Join-Path $env:SystemRoot "System32\$Name"))
}

Write-Host "== 3/4 补齐第三方依赖（反复扫描直到收敛）==" -ForegroundColor Cyan
$missingHard = @()
for ($round = 1; $round -le 8; $round++) {
    $added = 0
    $files = Get-ChildItem $OutDir -Recurse -Include *.exe, *.dll
    foreach ($f in $files) {
        foreach ($dep in (Get-ImportNames $f.FullName)) {
            if (Test-Path (Join-Path $OutDir $dep)) { continue }
            if (Test-IsSystemDll $dep) { continue }

            $src = Join-Path $QtBin $dep
            if (Test-Path $src) {
                Copy-Item $src $OutDir
                Write-Host "  + $dep"
                $added++
            } else {
                if ($missingHard -notcontains $dep) { $missingHard += $dep }
            }
        }
    }
    if ($added -eq 0) { break }
}
if ($missingHard.Count -gt 0) {
    Write-Host "  以下依赖没找到（可能影响运行）：" -ForegroundColor Yellow
    $missingHard | ForEach-Object { Write-Host "    ! $_" -ForegroundColor Yellow }
}

Write-Host "== 4/4 验证：清空 PATH 后能否独立运行 ==" -ForegroundColor Cyan
if ($IncludeSettings) {
    $cfg = Join-Path $BuildDir 'settings.json'
    if (Test-Path $cfg) {
        Copy-Item $cfg $OutDir -Force
        Write-Host "  已带上 settings.json（含 API Key）" -ForegroundColor Yellow
        Write-Host "  ⚠ 分发给别人前记得删掉它" -ForegroundColor Yellow
    }
}
$savedPath = $env:PATH
$savedEAP = $ErrorActionPreference
try {
    # 只留系统目录 —— 这样 MinGW 的 bin 就找不到了，
    # 能跑起来就说明依赖全在发布文件夹里。
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
    # 同上：原生命令的 stderr 不能被当成致命错误
    $ErrorActionPreference = 'Continue'
    $out = & (Join-Path $OutDir 'BoardMark.exe') --info 2>&1
    $code = $LASTEXITCODE
} finally {
    $env:PATH = $savedPath
    $ErrorActionPreference = $savedEAP
}

if ($code -eq 0 -and ($out | Select-String 'Qt ')) {
    Write-Host "  通过 —— 不依赖 PATH，可以拷到任何 Windows 10+ 电脑" -ForegroundColor Green
} else {
    Write-Host "  失败！退出码 $code" -ForegroundColor Red
    $out | Select-Object -First 6 | ForEach-Object { Write-Host "    $_" }
    Write-Host "  退出码 -1073741515 表示缺 DLL" -ForegroundColor Red
    exit 1
}

$size = (Get-ChildItem $OutDir -Recurse -File | Measure-Object Length -Sum).Sum
$count = (Get-ChildItem $OutDir -Recurse -File).Count
Write-Host ""
Write-Host "发布版已生成：$OutDir" -ForegroundColor Green
Write-Host ("  {0:N1} MB，{1} 个文件" -f ($size / 1MB), $count)
Write-Host "  整个文件夹拷到目标电脑即可，不需要装任何东西。"
