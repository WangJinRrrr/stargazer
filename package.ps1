# 打包发布包：dist\Stargazer-<版本>-win64.zip（stargazer.exe + README.md + LICENSE）
#
# 版本号只有一个来源：resources\stargazer.rc 的 VERSIONINFO，构建后从 exe 里读回来，
# 所以不需要在别处再维护一份版本号。
#
# 用法（cmake 不在 PATH 上时用 -Cmake 指到 VS 自带的那份；VS 没注册到 VS Installer 时
# 还要用 -ConfigureArgs 补实例版本，详见 README 的「打包」一节）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
param(
    [switch]$Update,                                   # 额外产出“覆盖式更新包”（只含 exe）
    [string]$Cmake = 'cmake',
    [string]$ConfigureArgs = '',                       # 首次配置用；已有 build\ 时不生效
    [string]$Config = 'Release',
    [switch]$SkipTests                                 # 万不得已才跳过测试闸门
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'

# 1) 首次配置（build\ 里已有缓存就不重配，免得覆盖掉别人的生成器设置）
if (-not (Test-Path (Join-Path $build 'CMakeCache.txt'))) {
    $cfgArgs = @('-S', $root, '-B', $build, '-G', 'Visual Studio 18 2026', '-A', 'x64')
    if ($ConfigureArgs) { $cfgArgs += $ConfigureArgs.Split(' ') }
    & $Cmake @cfgArgs
    if ($LASTEXITCODE -ne 0) { throw "配置失败（exit $LASTEXITCODE）" }
}

# 2) 编译：主程序 + 三个测试（测试是发布闸门，不给“测试都没跑就发版”留口子）
& $Cmake --build $build --config $Config --target stargazer test_model test_io test_layout
if ($LASTEXITCODE -ne 0) { throw "编译失败（exit $LASTEXITCODE）" }
$outDir = Join-Path $build $Config
$exe = Join-Path $outDir 'stargazer.exe'
if (-not (Test-Path $exe)) { throw "没找到 $exe" }

# 3) 测试闸门
if (-not $SkipTests) {
    foreach ($t in @('test_model', 'test_io', 'test_layout')) {
        & (Join-Path $outDir "$t.exe") | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "$t 没过（exit $LASTEXITCODE）" }
    }
    Write-Host '三个测试套件都通过'
}

# 4) 版本号从 exe 的 VERSIONINFO 读回来（1.0.0.0 -> 1.0.0）
$ver = (Get-Item $exe).VersionInfo.ProductVersion
if (-not $ver) { throw 'exe 里没有版本信息' }
$semver = ($ver -split '\.')[0..2] -join '.'

# 5) 打 zip
New-Item -ItemType Directory -Force $dist | Out-Null
$stage = Join-Path $dist "_stage_$semver"
Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item $exe $stage
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'LICENSE') $stage

$zip = Join-Path $dist "Stargazer-$semver-win64.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
Remove-Item $stage -Recurse -Force

$hash = (Get-FileHash $exe -Algorithm SHA256).Hash
""
"发布包: $zip"
"        {0:N1} KB   （stargazer.exe {1:N1} KB）" -f ((Get-Item $zip).Length / 1KB), ((Get-Item $exe).Length / 1KB)
"版本  : $semver  （来自 exe 的 VERSIONINFO）"
"SHA256: $hash"

# 6) 覆盖式更新包：只有一个 exe。单文件便携程序的最小更新形态 ——
#    用户解开覆盖旧 exe 即可，data\ 是用户数据，一个字节都不碰。
if ($Update) {
    $uzip = Join-Path $dist "Stargazer-$semver-update.zip"
    Remove-Item $uzip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path $exe -DestinationPath $uzip
    "更新包: $uzip"
    "        {0:N1} KB   （解开覆盖 stargazer.exe；data\ 原地不动）" -f ((Get-Item $uzip).Length / 1KB)
}
