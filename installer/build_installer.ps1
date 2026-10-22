# Builds Quake4-Setup-<version>.exe: the repository (as committed at HEAD) zipped into a small C#
# installer, compiled with the C# compiler that ships with Windows (.NET Framework 4.x).
# Usage: powershell -ExecutionPolicy Bypass -File installer\build_installer.ps1 [-Version 0.2.0] [-Icon path\to\q4.ico]
param(
    [string]$Version = '0.2.0',
    [string]$Icon = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$out = Join-Path $repo 'installer\out'
New-Item -ItemType Directory -Force $out | Out-Null

$zip = Join-Path $out 'repo.zip'
if (Test-Path $zip) { Remove-Item $zip }
git -C $repo archive --format=zip -o $zip HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }

$csc = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if (-not (Test-Path $csc)) { throw "C# compiler not found: $csc" }
$exe = Join-Path $out "Quake4-Setup-$Version.exe"
$cscArgs = @('/nologo', '/target:exe', '/optimize+', "/out:$exe", "/resource:$zip,repo.zip",
             '/r:System.Windows.Forms.dll', '/r:System.IO.Compression.dll', '/r:System.IO.Compression.FileSystem.dll')
if ($Icon -and (Test-Path $Icon)) { $cscArgs += "/win32icon:$Icon" }
$cscArgs += (Join-Path $PSScriptRoot 'Quake4Setup.cs')
& $csc @cscArgs
if ($LASTEXITCODE -ne 0) { throw 'csc failed' }
Write-Host "Built $exe ($([math]::Round((Get-Item $exe).Length / 1KB)) KB)"
