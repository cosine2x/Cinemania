param([string]$Python = "python")

# Builds Release binaries with an installed Visual Studio MSVC toolchain, CMake and Ninja.
$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs)
{
    throw "Visual Studio with MSVC x86 and x64 tools was not found."
}

$devcmd = Join-Path $vs "Common7/Tools/VsDevCmd.bat"
Push-Location $repo
try
{
    $receipt = Join-Path $repo "build/receipt.json"
    if (Test-Path -LiteralPath $receipt)
    {
        Remove-Item -LiteralPath $receipt
    }

    $snapshot = & $Python ./scripts/package.py --source-snapshot
    if ($LASTEXITCODE -ne 0)
    {
        throw "Could not fingerprint build sources."
    }

    foreach ($target in @(@("x86", "windows-x86"), @("x64", "windows-x64-worker")))
    {
        $arch = $target[0]
        $preset = $target[1]
        & cmd.exe /d /c "call `"$devcmd`" -no_logo -arch=$arch -host_arch=x64 && cmake --preset $preset && cmake --build --preset $preset-release"
        if ($LASTEXITCODE -ne 0)
        {
            throw "Build failed for $arch."
        }
    }

    & $Python ./scripts/package.py --write-receipt $snapshot
    if ($LASTEXITCODE -ne 0)
    {
        throw "Could not create build receipt."
    }
}
finally
{
    Pop-Location
}
