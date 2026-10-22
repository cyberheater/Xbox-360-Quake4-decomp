# Quake 4 (Xbox 360) for PC - one-click setup.
#
# Builds q4.exe and q4hle.exe from YOUR OWN copy of the game. Nothing from the game is downloaded:
# the game's code is converted into C++ on this PC from your ISO (or default.xex) and compiled here.
#
# What it does (each step is skipped when it is already done, so it is safe to run again):
#   1. Checks for git and Visual Studio 2022 (C++ tools with Clang, CMake, Ninja); offers to install
#      what is missing with winget.
#   2. Asks for your Quake 4 ISO (or default.xex from an extracted copy).
#   3. Downloads the ReXGlue SDK v0.10.0, applies this project's patches and builds it.
#   4. Takes default.xex out of the ISO, generates the recompiled code and builds the game.
#   5. Puts everything in the "Quake4" folder next to this script and makes a desktop shortcut.
#
# Run it with Setup.bat (or: powershell -ExecutionPolicy Bypass -File setup.ps1).
# Options: -GamePath <iso or default.xex>   skip the file dialog
#          -Clean                           rebuild the game from scratch (keeps the SDK)
#          -NoShortcut                      don't create a desktop shortcut

[CmdletBinding()]
param(
    [string]$GamePath = '',
    [switch]$Clean,
    [switch]$NoShortcut
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$Root     = $PSScriptRoot
$Port     = Join-Path $Root 'rexglue-port'
$Work     = Join-Path $Root '.build'
$SdkDir   = Join-Path $Work 'rexglue-sdk'
$SdkTag   = 'v0.10.0'
$SdkUrl   = 'https://github.com/rexglue/rexglue-sdk.git'
$SdkInst  = Join-Path $SdkDir 'out\install\win-amd64'
$OutDir   = Join-Path $Root 'Quake4'
$LogFile  = Join-Path $Root 'setup.log'

New-Item -ItemType Directory -Force $Work | Out-Null
try { Start-Transcript -Path $LogFile -Force | Out-Null } catch { }

function Step([string]$text) {
    Write-Host ''
    Write-Host "==> $text" -ForegroundColor Cyan
}
function Info([string]$text) { Write-Host "    $text" }
function Fail([string]$text) {
    Write-Host ''
    Write-Host "SETUP FAILED: $text" -ForegroundColor Red
    Write-Host "The full log is in $LogFile"
    try { Stop-Transcript | Out-Null } catch { }
    if ($Host.Name -eq 'ConsoleHost') { Read-Host 'Press Enter to close' | Out-Null }
    exit 1
}
# Runs a program; stops the setup if it fails. (Native programs write progress to stderr, which
# Windows PowerShell must not treat as an error, so errors are judged by the exit code only.)
function Run([string]$exe, [string[]]$arguments, [string]$what) {
    $ErrorActionPreference = 'Continue'
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "$what (exit code $LASTEXITCODE)" }
}
function Ask([string]$question) {
    $a = Read-Host "$question [Y/N]"
    return $a -match '^(y|yes)$'
}

Write-Host 'Quake 4 (Xbox 360) for PC - setup' -ForegroundColor Green
Write-Host 'You need your own Xbox 360 copy of Quake 4 (ISO or extracted default.xex).'
Write-Host 'This takes a while the first time (downloading and compiling: roughly 30-60 minutes).'

# ---------------------------------------------------------------------------------------------
Step 'Checking tools'

if (-not [Environment]::Is64BitOperatingSystem) { Fail 'A 64-bit Windows is required.' }

$winget = Get-Command winget -ErrorAction SilentlyContinue

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Info 'git is not installed.'
    if ($winget -and (Ask 'Install git now with winget?')) {
        Run 'winget' @('install', '--id', 'Git.Git', '-e', '--accept-source-agreements', '--accept-package-agreements') 'Installing git'
        $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('PATH', 'User')
    }
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail 'git is required: install it from https://git-scm.com and run setup again.' }
}
Info "git: $((git --version) -join ' ')"

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Find-VS {
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -version '[17.0,18.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang `
        -property installationPath
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}
$vs = Find-VS
if (-not $vs) {
    Info 'Visual Studio 2022 with the C++ tools and Clang was not found.'
    Info 'Needed: "Desktop development with C++" plus "C++ Clang tools for Windows" (and C++ CMake tools).'
    if ($winget -and (Ask 'Install Visual Studio 2022 Build Tools with these components now (about 6 GB)?')) {
        $components = '--add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ' +
                      '--add Microsoft.VisualStudio.Component.Windows11SDK.22621 --add Microsoft.VisualStudio.Component.VC.CMake.Project ' +
                      '--add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset'
        Run 'winget' @('install', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '-e', '--accept-source-agreements',
                       '--accept-package-agreements', '--override', "--wait --passive --norestart $components") 'Installing Visual Studio Build Tools'
        $vs = Find-VS
    }
    if (-not $vs) {
        Fail ('Install Visual Studio 2022 (Community or Build Tools) with "Desktop development with C++", ' +
              '"C++ Clang tools for Windows" and "C++ CMake tools for Windows", then run setup again.')
    }
}
Info "Visual Studio: $vs"

# Load the Visual Studio x64 build environment into this PowerShell session.
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found in $vs" }
$envCmd = Join-Path $Work 'vcenv.cmd'
Set-Content -Path $envCmd -Encoding ASCII -Value "@call `"$vcvars`" >nul 2>&1`r`n@set"
$envDump = & cmd.exe /c $envCmd
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$extra = @(
    (Join-Path $vs 'VC\Tools\Llvm\x64\bin'),
    (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
    (Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin')
) | Where-Object { Test-Path $_ }
$env:PATH = ($extra -join ';') + ';' + $env:PATH

foreach ($tool in 'clang++', 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool was not found. In the Visual Studio Installer add 'C++ Clang tools for Windows' and 'C++ CMake tools for Windows'."
    }
}
$cmakeVer = [version]'0.0'
if (((cmake --version) | Select-Object -First 1) -match '(\d+\.\d+(\.\d+)?)') { $cmakeVer = [version]$Matches[1] }
if ($cmakeVer -lt [version]'3.25') { Fail "CMake 3.25 or newer is needed (found $cmakeVer). Update Visual Studio." }
Info "clang: $(((clang++ --version) | Select-Object -First 1))"
Info "cmake: $cmakeVer"

# ---------------------------------------------------------------------------------------------
Step 'Your copy of Quake 4'

$savedGame = Join-Path $Work 'game_path.txt'
if (-not $GamePath -and (Test-Path $savedGame)) {
    $prev = (Get-Content $savedGame -Raw).Trim()
    if ($prev -and (Test-Path -LiteralPath $prev)) {
        Info "Using $prev (from the last run)"
        $GamePath = $prev
    }
}
if (-not $GamePath) {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = 'Choose your Quake 4 (Xbox 360) ISO, or default.xex from an extracted copy'
    $dlg.Filter = 'Quake 4 ISO or default.xex (*.iso;*.xex)|*.iso;*.xex|All files (*.*)|*.*'
    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No game file chosen.' }
    $GamePath = $dlg.FileName
}
if (-not (Test-Path -LiteralPath $GamePath -PathType Leaf)) { Fail "Not found: $GamePath" }
$GamePath = (Resolve-Path -LiteralPath $GamePath).Path
Set-Content -Path $savedGame -Value $GamePath
Info "Game: $GamePath"

# Reads default.xex out of an Xbox 360 disc image (XDVDFS), without extracting anything else.
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
public static class Q4Xiso {
    const long Sector = 2048;
    static readonly long[] Partitions = { 0x0, 0xFD90000, 0x2080000, 0x18300000 };
    public static bool ExtractFile(string iso, string wanted, string dest) {
        using (var fs = new FileStream(iso, FileMode.Open, FileAccess.Read, FileShare.Read)) {
            var br = new BinaryReader(fs);
            foreach (long part in Partitions) {
                long hdr = part + 32 * Sector;
                if (hdr + 28 > fs.Length) continue;
                fs.Position = hdr;
                if (Encoding.ASCII.GetString(br.ReadBytes(20)) != "MICROSOFT*XBOX*MEDIA") continue;
                uint rootSector = br.ReadUInt32();
                uint rootSize = br.ReadUInt32();
                long dir = part + rootSector * Sector;
                var todo = new Stack<long>();
                var seen = new HashSet<long>();
                todo.Push(0);
                while (todo.Count > 0) {
                    long off = todo.Pop();
                    if (off >= rootSize || !seen.Add(off)) continue;
                    fs.Position = dir + off;
                    ushort left = br.ReadUInt16();
                    ushort right = br.ReadUInt16();
                    if (left == 0xFFFF && right == 0xFFFF) continue;
                    uint start = br.ReadUInt32();
                    uint size = br.ReadUInt32();
                    byte attr = br.ReadByte();
                    byte nameLen = br.ReadByte();
                    string name = Encoding.ASCII.GetString(br.ReadBytes(nameLen));
                    if (left != 0) todo.Push(left * 4L);
                    if (right != 0) todo.Push(right * 4L);
                    if ((attr & 0x10) == 0 && string.Equals(name, wanted, StringComparison.OrdinalIgnoreCase)) {
                        fs.Position = part + start * Sector;
                        using (var o = new FileStream(dest, FileMode.Create, FileAccess.Write)) {
                            byte[] buf = new byte[1 << 20];
                            long left2 = size;
                            while (left2 > 0) {
                                int n = fs.Read(buf, 0, (int)Math.Min(buf.Length, left2));
                                if (n <= 0) throw new IOException("unexpected end of image");
                                o.Write(buf, 0, n);
                                left2 -= n;
                            }
                        }
                        return true;
                    }
                }
            }
        }
        return false;
    }
}
'@

$assets = Join-Path $Port 'assets'
New-Item -ItemType Directory -Force $assets | Out-Null
$xex = Join-Path $assets 'default.xex'
$newXex = Join-Path $Work 'default.xex.new'
if ($GamePath -match '\.xex$') {
    Copy-Item -LiteralPath $GamePath -Destination $newXex -Force
    $gameRoot = Split-Path -Parent $GamePath
} else {
    Info 'Reading default.xex from the ISO...'
    if (-not [Q4Xiso]::ExtractFile($GamePath, 'default.xex', $newXex)) {
        Fail 'This file does not look like an Xbox 360 game disc image (no default.xex found).'
    }
    $gameRoot = $GamePath
}
$xexChanged = $true
if (Test-Path $xex) {
    $xexChanged = (Get-FileHash $xex).Hash -ne (Get-FileHash $newXex).Hash
}
Move-Item -Force $newXex $xex
Info ("default.xex: {0:N1} MB" -f ((Get-Item $xex).Length / 1MB))

# ---------------------------------------------------------------------------------------------
Step "ReXGlue SDK $SdkTag"

$rexglue = Join-Path $SdkInst 'bin\rexglue.exe'
if (Test-Path $rexglue) {
    Info 'Already built.'
} else {
    if (-not (Test-Path (Join-Path $SdkDir '.git'))) {
        Info 'Downloading (the SDK and its libraries, about 1-2 GB)...'
        Run 'git' @('-c', 'core.autocrlf=false', 'clone', '--depth', '1', '--branch', $SdkTag, $SdkUrl, $SdkDir) 'Downloading the SDK'
    }
    Push-Location $SdkDir
    try {
        Run 'git' @('-c', 'core.autocrlf=false', 'submodule', 'update', '--init', '--recursive', '--depth', '1') 'Downloading the SDK libraries'
        # Windows checks out a few libmspack symlinks as small text files holding the target path.
        $msp = Join-Path $SdkDir 'thirdparty\libmspack\cabextract\mspack'
        if (Test-Path $msp) {
            foreach ($f in Get-ChildItem -LiteralPath $msp -File) {
                if ($f.Length -gt 0 -and $f.Length -lt 200) {
                    $target = (Get-Content -LiteralPath $f.FullName -Raw).Trim()
                    if ($target -match '^\.\./') {
                        $src = Join-Path $f.DirectoryName ($target -replace '/', '\')
                        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $f.FullName -Force }
                    }
                }
            }
        }
        $patch = Join-Path $Port 'rexglue-sdk-patches.diff'
        $ErrorActionPreference = 'Continue'
        & git apply --check --ignore-whitespace $patch 2>$null
        if ($LASTEXITCODE -eq 0) {
            Run 'git' @('apply', '--ignore-whitespace', $patch) 'Applying the SDK patches'
            Info 'Patches applied.'
        } else {
            & git apply --check --reverse --ignore-whitespace $patch 2>$null
            if ($LASTEXITCODE -ne 0) { Fail 'The SDK patches do not apply to the downloaded SDK. Delete the .build folder and run setup again.' }
            Info 'Patches already applied.'
        }
        $ErrorActionPreference = 'Stop'
        Info 'Building the SDK (this is the longest step)...'
        Run 'cmake' @('--preset', 'win-amd64') 'Configuring the SDK'
        Run 'cmake' @('--build', 'out/build/win-amd64', '--config', 'Release', '--target', 'install') 'Building the SDK'
    } finally { Pop-Location }
    if (-not (Test-Path $rexglue)) { Fail "rexglue.exe was not produced ($rexglue)" }
}

# ---------------------------------------------------------------------------------------------
Step 'Converting the game code (on this PC, from your copy)'

$gen = Join-Path $Port 'generated\default'
$buildDir = Join-Path $Port 'out\build\win-amd64-release'
if ($Clean) {
    foreach ($d in $gen, $buildDir) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
}
if ($xexChanged -or -not (Test-Path (Join-Path $gen 'sources.cmake'))) {
    Push-Location $Port
    try { Run $rexglue @('codegen', 'q4_manifest.toml') 'Generating the recompiled code' } finally { Pop-Location }
} else {
    Info 'Up to date.'
}

# ---------------------------------------------------------------------------------------------
Step 'Building q4.exe and q4hle.exe'

Push-Location $Port
try {
    Run 'cmake' @('--preset', 'win-amd64-release', '-DQ4_HLE=ON', "-DCMAKE_PREFIX_PATH=$SdkInst") 'Configuring the game'
    Run 'cmake' @('--build', '--preset', 'win-amd64-release') 'Building the game'
} finally { Pop-Location }
foreach ($exe in 'q4.exe', 'q4hle.exe') {
    if (-not (Test-Path (Join-Path $buildDir $exe))) { Fail "$exe was not built" }
}

# ---------------------------------------------------------------------------------------------
Step "Installing into $OutDir"

New-Item -ItemType Directory -Force $OutDir | Out-Null
Copy-Item (Join-Path $buildDir 'q4.exe'), (Join-Path $buildDir 'q4hle.exe') $OutDir -Force
Get-ChildItem (Join-Path $SdkInst 'bin') -Filter *.dll | Copy-Item -Destination $OutDir -Force
$ico = Join-Path $buildDir 'q4.ico'
if (Test-Path $ico) { Copy-Item $ico $OutDir -Force }

# Tell the game where your copy is (q4.toml next to the exe; your settings are kept).
$toml = Join-Path $OutDir 'q4.toml'
if ($gameRoot.Contains("'")) {
    $line = 'game_data_root = "' + $gameRoot.Replace('\', '\\').Replace('"', '\"') + '"'
} else {
    $line = "game_data_root = '$gameRoot'"
}
if (Test-Path $toml) {
    $content = Get-Content $toml
    if ($content -match '^\s*game_data_root\s*=') {
        ($content -replace '^\s*game_data_root\s*=.*$', $line) | Set-Content $toml
    } else {
        Add-Content $toml $line
    }
} else {
    Set-Content $toml @('# Quake 4 settings (the in-game menus save here too)', $line)
}
Info "Game location saved: $gameRoot"

if (-not $NoShortcut) {
    try {
        $shell = New-Object -ComObject WScript.Shell
        $lnk = $shell.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'Quake 4.lnk'))
        $lnk.TargetPath = Join-Path $OutDir 'q4hle.exe'
        $lnk.WorkingDirectory = $OutDir
        if (Test-Path (Join-Path $OutDir 'q4.ico')) { $lnk.IconLocation = Join-Path $OutDir 'q4.ico' }
        $lnk.Save()
        Info 'Desktop shortcut "Quake 4" created (runs q4hle.exe).'
    } catch { Info "Could not create a desktop shortcut: $($_.Exception.Message)" }
}

Write-Host ''
Write-Host 'Done!' -ForegroundColor Green
Write-Host "  $OutDir\q4hle.exe  - native renderer: 120 Hz, Options > GRAPHICS menu, 3D mode (recommended)"
Write-Host "  $OutDir\q4.exe     - emulated Xbox 360 GPU (the original port)"
Write-Host 'Run Setup.bat again after updating the repository to rebuild.'
try { Stop-Transcript | Out-Null } catch { }
if ($Host.Name -eq 'ConsoleHost') { Read-Host 'Press Enter to close' | Out-Null }
