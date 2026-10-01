# One-step setup for Windows 10/11: installs the build prerequisites with
# winget, then fetches the pinned dependencies and builds (scripts/setup.py).
#
#   powershell -ExecutionPolicy Bypass -File setup.ps1 [--test-extras] [--with-mame]
#
# Installs (skipping what is already there): Git, CMake, Ninja, Python 3 and
# Visual Studio 2022 Build Tools with the C++ workload and its Clang tools
# (clang-cl, MSVC and the Windows SDK, which has Direct3D 12); the game is
# built with Clang. SDL 3 is fetched and built with the project; Vulkan comes
# with the GPU driver. Put your own ROM set at roms\daytona93.zip to have the
# game recompiled too.

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Write-Error "winget not found. Install 'App Installer' from the Microsoft Store, then run setup.ps1 again."
}

function Install-Package([string]$Id, [string]$Override = '') {
    $listed = winget list --id $Id --exact --accept-source-agreements 2>$null | Select-String -SimpleMatch $Id
    if ($listed) { Write-Host "== $Id already installed"; return }
    Write-Host "== Installing $Id"
    $wingetArgs = @('install', '--id', $Id, '--exact', '--silent', '--accept-package-agreements', '--accept-source-agreements')
    if ($Override) { $wingetArgs += @('--override', $Override) }
    & winget @wingetArgs
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {  # -1978335189: already installed
        Write-Error "winget could not install $Id (exit $LASTEXITCODE)"
    }
}

Install-Package 'Git.Git'
Install-Package 'Kitware.CMake'
Install-Package 'Ninja-build.Ninja'
Install-Package 'Python.Python.3.12'
# The C++ tools, with Clang (clang-cl and the ClangCL toolset): setup.py
# builds with Clang when it is there, MSVC otherwise.
$vsComponents = @('Microsoft.VisualStudio.Workload.VCTools',
                  'Microsoft.VisualStudio.Component.VC.Llvm.Clang',
                  'Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset')
$vsAdd = ($vsComponents | ForEach-Object { "--add $_" }) -join ' '
Install-Package 'Microsoft.VisualStudio.2022.BuildTools' "--wait --passive --norestart $vsAdd --includeRecommended"

# winget leaves an existing Visual Studio or Build Tools install as it is, so
# add the C++ and Clang tools to it if they are missing.
$installerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$vswhere = Join-Path $installerDir 'vswhere.exe'
if (Test-Path $vswhere) {
    $withClang = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset -property installationPath
    if (-not $withClang) {
        $vsPath = & $vswhere -latest -products * -property installationPath
        if ($vsPath) {
            Write-Host "== Adding the C++ and Clang tools to $vsPath (the Visual Studio Installer may ask for permission)"
            $modify = @('modify', '--installPath', "`"$vsPath`"", '--passive', '--norestart', '--includeRecommended') +
                      ($vsComponents | ForEach-Object { @('--add', $_) })
            $p = Start-Process -FilePath (Join-Path $installerDir 'setup.exe') -ArgumentList $modify -Wait -PassThru
            if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {  # 3010: done, restart suggested
                Write-Warning "The Visual Studio Installer could not add Clang (exit $($p.ExitCode)); setup continues with MSVC."
            }
        }
    }
}

# Pick up the new tools without opening a new terminal.
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')

# Prefer the py launcher: a bare "python" can be the Microsoft Store stub.
$python = (Get-Command py -ErrorAction SilentlyContinue)
if (-not $python) { $python = (Get-Command python -ErrorAction SilentlyContinue) }
if (-not $python) { Write-Error "Python was installed but is not on PATH yet; open a new terminal and run setup.ps1 again." }

& $python.Source scripts\setup.py @args
exit $LASTEXITCODE
