<#
  Blur Live build script.
    .\build.ps1                 # Release build
    .\build.ps1 -Config Debug   # Debug build
    .\build.ps1 -Test           # build + run the test suite
    .\build.ps1 -Run            # build + launch the app
    .\build.ps1 -Clean          # wipe the build directory first
#>
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
  [switch]$Clean,
  [switch]$Test,
  [switch]$Run,
  [string]$TestArgs = ''
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022 or the VS 2022 Build Tools (Desktop C++ workload)." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio installation with the C++ x64 toolset was found." }

# Import the MSVC x64 developer environment into this PowerShell session.
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

$build = Join-Path $root "build\$Config"
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }

cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "Build OK: $build\BlurLive.exe" -ForegroundColor Green

if ($Test) {
  & "$build\BlurLiveTests.exe" $TestArgs.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
  exit $LASTEXITCODE
}
if ($Run) { Start-Process "$build\BlurLive.exe" }
