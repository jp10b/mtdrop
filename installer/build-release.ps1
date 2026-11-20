# Builds everything mtdrop installs and packs it into one setup program (installer\mtdrop.iss, Inno Setup 6).
#
#   .\installer\build-release.ps1 [-Version 0.1.0] [-ToolsRoot D:\mtdrop-tools] [-SkipNative]
#
# Needs (developer machine only; the end user needs none of this):
#   Visual Studio 2026 with MSVC x64 + the WDK component, WDK NuGet packages under <ToolsRoot>\packages
#   (docs/DEV-SETUP.md), CMake + Ninja and MinGW-w64 GCC on PATH, .NET 10 SDK, Inno Setup 6 (ISCC.exe),
#   the Wintun 0.14.1 zip from https://www.wintun.net/ in <ToolsRoot>\downloads\wintun\ (pinned SHA-256 below).
#
# Changes nothing on the machine except files under out\release and the build directories. It does not sign
# anything and installs nothing. Output: out\release\mtdrop-setup-<Version>.exe
#
# What the setup contains and why some things are NOT in it:
#   - service + tray app: .NET 10 self-contained (the user needs no .NET runtime).
#   - radiod (static C runtime), awdld (UCRT only, part of Windows), wintun.dll (official signed DLL, unmodified,
#     redistribution allowed by its licence; it installs the Wintun driver itself when awdl0 is created).
#   - the proxy driver package with an UNSIGNED catalog: it is signed on the user's machine with a certificate made
#     there, whose private key is deleted right after (same as scripts\install-service.ps1).
#   - NOT the MT7961 firmware: the setup downloads it from openwrt/mt76 at a pinned commit and checks its SHA-256
#     (docs/FIRMWARE.md: mtdrop does not redistribute firmware).

param(
    [string]$Version = '0.1.0',
    [string]$ToolsRoot = 'D:\mtdrop-tools',
    [switch]$SkipNative
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'out\release'
$stage = Join-Path $out 'stage'
$build = Join-Path $out 'build'
$WintunZipSha256 = '07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51'   # wintun-0.14.1.zip

function Say([string]$m) { Write-Output ("{0:HH:mm:ss} {1}" -f (Get-Date), $m) }
function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    # Native tools write progress and warnings to stderr; in Windows PowerShell that must not count as an error.
    $ErrorActionPreference = 'Continue'
    & $cmd 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE)" }
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must look like 1.2.3" }

# ---- tools -----------------------------------------------------------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the MSVC x64 tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$msbuild = Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$inf2cat = Get-ChildItem (Join-Path $ToolsRoot 'packages') -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x86\\' } | Select-Object -First 1
if (-not $inf2cat) { $inf2cat = Get-ChildItem (Join-Path $ToolsRoot 'packages') -Recurse -Filter Inf2Cat.exe -ErrorAction SilentlyContinue | Select-Object -First 1 }
if (-not $inf2cat) { throw "Inf2Cat.exe not found under $ToolsRoot\packages (WDK NuGet package)" }
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found: winget install JRSoftware.InnoSetup' }
foreach ($t in 'cmake', 'ninja', 'gcc', 'dotnet') { if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t not on PATH" } }
$wintunZip = Join-Path $ToolsRoot 'downloads\wintun\wintun-0.14.1.zip'
if (-not (Test-Path $wintunZip)) { throw "missing $wintunZip (download it from https://www.wintun.net/)" }
if ((Get-FileHash $wintunZip -Algorithm SHA256).Hash -ne $WintunZipSha256) { throw "$wintunZip does not have the pinned SHA-256" }

# ---- clean stage ------------------------------------------------------------------------------------------
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage, "$stage\app", "$stage\driver", "$stage\setup", "$stage\licenses", $build | Out-Null

# ---- native parts -----------------------------------------------------------------------------------------
$radiodBuild = Join-Path $build 'radiod'
$awdldBuild = Join-Path $build 'awdld'
if (-not $SkipNative) {
    Say 'radiod (MSVC, static C runtime)'
    $bat = Join-Path $build 'radiod.bat'
    @"
@echo off
set "PATH=%PATH%;$(Split-Path $vswhere)"
call "$vcvars" >nul 2>&1 || exit /b 1
cmake -S "$repo\driver\radiod" -B "$radiodBuild" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
cmake --build "$radiodBuild" || exit /b 1
"@ | Set-Content -Encoding ascii $bat
    Invoke-Checked 'radiod build' { cmd.exe /c $bat }

    Say 'awdld (MinGW-w64)'
    Invoke-Checked 'awdld configure' { cmake -S "$repo\awdld" -B $awdldBuild -G Ninja -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ -DCMAKE_BUILD_TYPE=Release -DMTDROP_OWL_TESTS=OFF }
    Invoke-Checked 'awdld build' { cmake --build $awdldBuild }

    Say 'proxy driver (WDK)'
    Invoke-Checked 'proxy build' { & $msbuild "$repo\driver\proxy\mtdrop_proxy.vcxproj" /p:Configuration=Release /p:Platform=x64 "/p:WdkPkgRoot=$ToolsRoot\packages\" /nologo /v:m }
}
foreach ($f in "$radiodBuild\radiod.exe", "$awdldBuild\awdld.exe") { if (-not (Test-Path $f)) { throw "missing $f (run without -SkipNative)" } }
if (Get-Command objdump -ErrorAction SilentlyContinue) {
    $deps = (& objdump -p "$radiodBuild\radiod.exe" 2>$null) -match 'DLL Name: VCRUNTIME'
    if ($deps) { throw 'radiod.exe still needs VCRUNTIME140.dll (static runtime not applied)' }
}
Copy-Item "$radiodBuild\radiod.exe", "$awdldBuild\awdld.exe" $stage

Say 'wintun.dll 0.14.1 (official, signed)'
$wx = Join-Path $build 'wintun'
if (Test-Path $wx) { Remove-Item $wx -Recurse -Force }
Expand-Archive $wintunZip $wx
Copy-Item "$wx\wintun\bin\amd64\wintun.dll" $stage
if ((Get-AuthenticodeSignature "$stage\wintun.dll").Status -ne 'Valid') { throw 'wintun.dll signature is not valid' }
Copy-Item "$wx\wintun\LICENSE.txt" "$stage\licenses\wintun.txt"

Say 'proxy driver package + unsigned catalog'
$proxyOut = Join-Path $repo 'driver\proxy\x64\Release'
Copy-Item "$proxyOut\mtdrop_proxy\mtdrop_proxy.inf", "$proxyOut\mtdrop_proxy.dll" "$stage\driver"
$catOut = & $inf2cat.FullName "/driver:$stage\driver" /os:10_X64 /uselocaltime 2>&1
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$stage\driver\mtdrop_proxy.cat")) { $catOut | ForEach-Object { Say "  inf2cat: $_" }; throw 'Inf2Cat failed' }

# ---- .NET parts -------------------------------------------------------------------------------------------
Say 'service and tray app (.NET 10 self-contained)'
Invoke-Checked 'service publish' { dotnet publish "$repo\service\Mtdrop.Service" -c Release -r win-x64 --self-contained true -p:Version=$Version -o $stage --nologo -v q }
Invoke-Checked 'app publish' { dotnet publish "$repo\app\Mtdrop.App" -c Release -r win-x64 --self-contained true -p:Version=$Version -o "$stage\app" --nologo -v q }
Get-ChildItem $stage -Recurse -Filter *.pdb | Remove-Item

# ---- setup script, licences -------------------------------------------------------------------------------
Copy-Item "$PSScriptRoot\setup.ps1" "$stage\setup\setup.ps1"
Copy-Item "$repo\LICENSE" "$stage\licenses\mtdrop-GPL-3.0.txt"
Copy-Item "$repo\third_party\NOTICE" "$stage\licenses\NOTICE.txt"

# ---- setup program ----------------------------------------------------------------------------------------
Say "setup program (Inno Setup), version $Version"
Invoke-Checked 'ISCC' { & $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/O$out" "$PSScriptRoot\mtdrop.iss" /Q }
$exe = Join-Path $out "mtdrop-setup-$Version.exe"
$hash = (Get-FileHash $exe -Algorithm SHA256).Hash.ToLowerInvariant()
"$hash  mtdrop-setup-$Version.exe" | Set-Content -Encoding ascii "$exe.sha256"
Say ("done: {0} ({1:N1} MB), SHA-256 {2}" -f $exe, ((Get-Item $exe).Length / 1MB), $hash)
