# 检查并修复本项目源文件的编码问题。
#
# 背景：这个项目要求源文件是 UTF-8 带 BOM。少了 BOM，MSVC 会按 GBK 解 UTF-8 的中文
# 注释，字节被读坏后会报一堆看不懂的语法错误。
#
# 注意：仓库里有几十个上游文件本来就没有 BOM（它们要么是纯 ASCII，要么是 GBK 编码），
# 那些文件能正常编译，不要去动。所以默认只检查「上游基点之后动过」的文件，
# 也就是本分支真正碰过的那些（已提交和未提交都算）。想看全部就加 -All。
#
# 用法：
#   pwsh -File scripts/check-bom.ps1          # 检查改动过的文件
#   pwsh -File scripts/check-bom.ps1 -Fix     # 检查并补 BOM
#   pwsh -File scripts/check-bom.ps1 -All     # 检查全部源文件

param(
    [switch]$Fix,
    [switch]$All
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$srcRoot = Join-Path $repoRoot 'MusicPlayer2'

if (-not (Test-Path $srcRoot)) {
    Write-Host "找不到源码目录：$srcRoot" -ForegroundColor Red
    exit 1
}

# 第三方库不动（它们有自己的编码约定，且不该被改动）
$skipDirs = @('taglib', 'scintilla', 'nlohmann', 'tinyxml2', 'qrcodegen', 'puff', 'res', 'skins')

$candidates = Get-ChildItem $srcRoot -Recurse -File |
    Where-Object { $_.Extension -in '.cpp', '.h', '.hpp', '.cxx' } |
    Where-Object {
        $rel = $_.FullName.Substring($srcRoot.Length + 1)
        -not ($skipDirs | Where-Object { $rel -like "$_\*" })
    }

# 仓库里的上游基点提交。默认只检查「这个基点之后动过的文件」。
$baseCommit = '328af4cc'

if (-not $All) {
    # 范围 = 相对基点有改动的文件（已提交的也算）+ 未跟踪的新文件。
    # 早先只看「相对 git 有未提交改动」，提交完就什么都查不到，护栏等于静默失效。
    $changedNames = @()
    $tracked = & git -C $repoRoot diff --name-only $baseCommit -- MusicPlayer2 2>$null
    if ($LASTEXITCODE -ne 0) { $tracked = @() }
    $untracked = & git -C $repoRoot ls-files --others --exclude-standard -- MusicPlayer2 2>$null
    foreach ($path in @($tracked) + @($untracked)) {
        if ([string]::IsNullOrWhiteSpace($path)) { continue }
        $changedNames += (Split-Path $path.Trim().Trim('"') -Leaf)
    }
    # 注意：即使改动列表为空也要过滤，否则会退化成检查全部文件
    $candidates = $candidates | Where-Object { $changedNames -contains $_.Name }
}

function Test-HasBom([byte[]]$bytes) {
    return $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
}

$missingBom = @()
$badEnding = @()

foreach ($f in $candidates) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)

    if (-not (Test-HasBom $bytes)) {
        $missingBom += $f
        continue
    }

    # 统计行尾：混用 CRLF 和 LF 会让 git diff 出现大量无关改动
    $crlf = 0
    $lf = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -eq 0x0A) {
            if ($i -gt 0 -and $bytes[$i - 1] -eq 0x0D) { $crlf++ } else { $lf++ }
        }
    }
    if ($crlf -gt 0 -and $lf -gt 0) {
        $badEnding += [pscustomobject]@{ File = $f; CRLF = $crlf; LF = $lf }
    }
}

# 把绝对路径显示成相对源码目录的形式，加个前缀避免和别的目录混淆
function Format-Path([string]$fullPath) {
    return "MusicPlayer2/" + $fullPath.Substring($srcRoot.Length + 1).Replace('\', '/')
}

$scope = if ($All) { '全部源文件' } else { '本次改动过的源文件' }

if ($candidates.Count -eq 0) {
    Write-Host "没有需要检查的源文件（相对 git 没有改动）。想看全部加 -All" -ForegroundColor Cyan
    exit 0
}

Write-Host "已检查 $($candidates.Count) 个文件（$scope）"
Write-Host ""

if ($missingBom.Count -eq 0 -and $badEnding.Count -eq 0) {
    Write-Host "✓ 编码正常，可以放心编译" -ForegroundColor Green
    exit 0
}

if ($missingBom.Count -gt 0) {
    Write-Host "缺少 UTF-8 BOM（$($missingBom.Count) 个）：" -ForegroundColor Yellow
    foreach ($f in $missingBom) {
        Write-Host "    $(Format-Path $f.FullName)"
    }

    if ($Fix) {
        Write-Host ""
        Write-Host "正在补上 BOM…"
        foreach ($f in $missingBom) {
            # 按 UTF-8（不带 BOM）读取，再带 BOM 写回。用 .NET 方法，不改动内容。
            $text = [System.IO.File]::ReadAllText($f.FullName, [System.Text.UTF8Encoding]::new($false))
            [System.IO.File]::WriteAllText($f.FullName, $text, [System.Text.UTF8Encoding]::new($true))
            Write-Host "    ✓ $(Format-Path $f.FullName)" -ForegroundColor Green
        }
    }
    Write-Host ""
}

if ($badEnding.Count -gt 0) {
    Write-Host "行尾混用了 CRLF 和 LF：" -ForegroundColor Yellow
    foreach ($e in $badEnding) {
        Write-Host "    $(Format-Path $e.File.FullName)  (CRLF=$($e.CRLF) LF=$($e.LF))"
    }
    Write-Host ""
}

if (-not $Fix -and $missingBom.Count -gt 0) {
    Write-Host "加 -Fix 可以自动补 BOM" -ForegroundColor Cyan
    exit 2
}

exit 0

