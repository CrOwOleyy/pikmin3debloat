<#
.SYNOPSIS
    Builds the Pikmin 3 (Wii U) static recompilation from your own game dump.

.DESCRIPTION
    1. checks the required tools;
    2. finds your dump (folders whose names contain [Game] and [Update]);
    3. fetches the third-party tools (git submodules) and applies the patches in patches/;
    4. decompresses carrot.rpx (Update), links its Cafe OS imports and generates the
       recompiler configuration;
    5. builds XenonRecomp and recompiles the game code into C++;
    6. rebuilds the menu shader file if your dump's copy is damaged;
    7. builds pikmin3.exe.

    Every step is skipped when its output is already up to date, so the script can simply be
    run again after an error or a "git pull".

.PARAMETER GameDir
    Folder that contains the "... [Game] ..." and "... [Update] ..." folders of your dump.
    Default: searched from the repository folder upwards.

.PARAMETER ForceLytShaders
    Always use the rebuilt menu shaders (recomp/content_override), even if the dump's own
    lyt_BuildinShader.gsh looks fine.

.EXAMPLE
    setup.bat
    setup.bat -GameDir "D:\Dumps\Pikmin 3"
#>
param(
    [string]$GameDir = "",
    [switch]$ForceLytShaders
)

$ErrorActionPreference = "Stop"
$Here = $PSScriptRoot                       # decompile/
$RepoRoot = Split-Path -Parent $Here
$Tools = Join-Path $Here "tools"
$Recomp = Join-Path $Here "recomp"

function Step([string]$text) { Write-Host ""; Write-Host "==> $text" -ForegroundColor Cyan }
function Info([string]$text) { Write-Host "    $text" }
function Fail([string]$text) { Write-Host ""; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

function Run([string]$exe, [string[]]$arguments) {
    & $exe @arguments
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($arguments -join ' ')' failed (exit code $LASTEXITCODE)." }
}

# Output is up to date when it exists and is newer than every input.
function UpToDate([string]$output, [string[]]$inputs) {
    if (-not (Test-Path $output)) { return $false }
    $time = (Get-Item $output).LastWriteTimeUtc
    foreach ($i in $inputs) {
        if ((Get-Item $i).LastWriteTimeUtc -gt $time) { return $false }
    }
    return $true
}

# ---------------------------------------------------------------------------------------------
Step "Checking the required tools"

foreach ($tool in @(
        @{ Name = "git"; Hint = "https://git-scm.com/download/win" },
        @{ Name = "python"; Hint = "https://www.python.org/downloads/ (3.9 or newer, tick 'Add to PATH')" },
        @{ Name = "cmake"; Hint = "https://cmake.org/download/ (or the Visual Studio CMake component)" })) {
    if (-not (Get-Command $tool.Name -ErrorAction SilentlyContinue)) {
        Fail "$($tool.Name) not found. Install it: $($tool.Hint)"
    }
}
& python -c "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)"
if ($LASTEXITCODE -ne 0) { Fail "Python 3.9 or newer is required." }

if (-not $env:VULKAN_SDK -or -not (Test-Path $env:VULKAN_SDK)) {
    Fail "Vulkan SDK not found (VULKAN_SDK is not set). Install it: https://vulkan.lunarg.com/sdk/home#windows"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { Fail "Visual Studio 2022 not found: https://visualstudio.microsoft.com/vs/community/" }
$vsDir = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsDir) { Fail "Visual Studio 2022 needs the 'Desktop development with C++' workload." }

# Import the Visual Studio x64 build environment into this PowerShell session.
$vcvars = Join-Path $vsDir "VC\Auxiliary\Build\vcvars64.bat"
& cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}
$llvm = Join-Path $env:ProgramFiles "LLVM\bin"
if (Test-Path (Join-Path $llvm "clang-cl.exe")) { $env:PATH = "$llvm;$env:PATH" }
if (-not (Get-Command clang-cl -ErrorAction SilentlyContinue)) {
    Fail "clang-cl not found. Install LLVM (https://github.com/llvm/llvm-project/releases, 'LLVM-<version>-win64.exe') or the 'C++ Clang tools for Windows' component of Visual Studio."
}
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    Fail "ninja not found. Install the 'C++ CMake tools for Windows' component of Visual Studio, or ninja from https://github.com/ninja-build/ninja/releases"
}
Info "Visual Studio: $vsDir"
Info "clang-cl:      $((Get-Command clang-cl).Source)"
Info "Vulkan SDK:    $env:VULKAN_SDK"

# ---------------------------------------------------------------------------------------------
Step "Looking for your game dump"

function FindDump([string]$start) {
    $dir = Get-Item -LiteralPath $start -ErrorAction SilentlyContinue
    while ($dir) {
        $game = Get-ChildItem -LiteralPath $dir.FullName -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like "*``[Game``]*" } | Select-Object -First 1
        if ($game) { return $dir.FullName }
        $dir = $dir.Parent
    }
    return $null
}

$dumpRoot = if ($GameDir) { $GameDir } else { FindDump $RepoRoot }
if (-not $dumpRoot -or -not (Test-Path -LiteralPath $dumpRoot)) {
    Fail "No dump found. Put the '... [Game] ...' and '... [Update] ...' folders next to the repository (or in a parent folder), or pass -GameDir <folder that contains them>."
}
$gameFolder = Get-ChildItem -LiteralPath $dumpRoot -Directory | Where-Object { $_.Name -like "*``[Game``]*" } | Select-Object -First 1
$updateFolder = Get-ChildItem -LiteralPath $dumpRoot -Directory | Where-Object { $_.Name -like "*``[Update``]*" } | Select-Object -First 1
$dlcFolder = Get-ChildItem -LiteralPath $dumpRoot -Directory | Where-Object { $_.Name -like "*``[DLC``]*" } | Select-Object -First 1
if (-not $gameFolder) { Fail "No folder with '[Game]' in its name in $dumpRoot." }
if (-not $updateFolder) { Fail "No folder with '[Update]' in its name in $dumpRoot (the game update v96 is required)." }
$rpx = Join-Path $updateFolder.FullName "code\carrot.rpx"
if (-not (Test-Path -LiteralPath $rpx)) { Fail "$rpx not found: the update folder must contain code\carrot.rpx." }
# The function list (recomp/config/carrot_functions.toml) matches one exact build of the game.
$supportedRpx = "E8ACC7E026BEE0C0CE7C81F38D8B2AE14E969E14"  # European version, update v96
$rpxHash = (Get-FileHash -LiteralPath $rpx -Algorithm SHA1).Hash
if ($rpxHash -ne $supportedRpx) {
    Fail "Unsupported carrot.rpx (SHA-1 $rpxHash). Only the European version with update v96 (title 0005000E1012BE00) is supported for now."
}
if (-not (Test-Path -LiteralPath (Join-Path $gameFolder.FullName "content\CMCmn"))) {
    Fail "$($gameFolder.FullName) does not look like a decrypted Pikmin 3 dump (content\CMCmn is missing)."
}
Info "Game:   $($gameFolder.FullName)"
Info "Update: $($updateFolder.FullName)"
Info ("DLC:    " + $(if ($dlcFolder) { $dlcFolder.FullName } else { "(none, optional)" }))

# ---------------------------------------------------------------------------------------------
Step "Fetching the third-party tools (git submodules)"

Run git @("-C", $RepoRoot, "submodule", "update", "--init",
    "decompile/tools/decaf-emu", "decompile/tools/XenonRecomp", "decompile/tools/Vulkan-Headers")
Run git @("-C", (Join-Path $Tools "decaf-emu"), "submodule", "update", "--init",
    "libraries/addrlib", "libraries/cnl", "libraries/cpp-peglib", "libraries/excmd", "libraries/fmt",
    "libraries/glslang", "libraries/gsl-lite", "libraries/spdlog")
Run git @("-C", (Join-Path $Tools "XenonRecomp"), "submodule", "update", "--init", "--recursive")

foreach ($name in @("decaf-emu", "XenonRecomp")) {
    $repo = Join-Path $Tools $name
    $patch = Join-Path $Here "patches\$name.patch"
    & git -C $repo apply --check --reverse --ignore-whitespace $patch 2>$null
    if ($LASTEXITCODE -eq 0) { Info "$name.patch already applied"; continue }
    & git -C $repo apply --check --ignore-whitespace $patch
    if ($LASTEXITCODE -ne 0) {
        Fail "patches\$name.patch does not apply to tools\$name. Reset that folder with 'git -C decompile/tools/$name checkout -- .' and run setup again."
    }
    Run git @("-C", $repo, "apply", "--ignore-whitespace", $patch)
    Info "$name.patch applied"
}

# ---------------------------------------------------------------------------------------------
Step "Preparing the game code (carrot.rpx)"

$extracted = Join-Path $Here "extracted"
New-Item -ItemType Directory -Force $extracted, (Join-Path $Recomp "config"), (Join-Path $Recomp "runtime\generated") | Out-Null
$elf = Join-Path $extracted "carrot.elf"
$linked = Join-Path $Recomp "carrot_linked.elf"
$functions = Join-Path $Recomp "config\carrot_functions.toml"
$recompConfig = Join-Path $Recomp "config\carrot.toml"
$scripts = Join-Path $Here "scripts"

if (UpToDate $elf @($rpx)) { Info "carrot.elf up to date" }
else { Run python @((Join-Path $scripts "rpx_decompress.py"), $rpx, $elf) }

if (UpToDate $linked @($elf, (Join-Path $scripts "link_rpl.py"))) { Info "carrot_linked.elf up to date" }
else {
    Run python @((Join-Path $scripts "link_rpl.py"), $elf, $linked, (Join-Path $Recomp "config\imports.toml"),
        (Join-Path $Recomp "runtime\generated\imports.inc"))
}

if (UpToDate $recompConfig @($linked, $functions, (Join-Path $scripts "gen_recomp_config.py"))) { Info "recompiler configuration up to date" }
else { Run python @((Join-Path $scripts "gen_recomp_config.py"), $elf, $linked, $functions, (Join-Path $Recomp "config")) }

# ---------------------------------------------------------------------------------------------
Step "Recompiling the game code with XenonRecomp"

$xenon = Join-Path $Tools "XenonRecomp"
$xenonExe = Join-Path $xenon "build-win\XenonRecomp\XenonRecomp.exe"
if (-not (Test-Path $xenonExe) -or -not (UpToDate $xenonExe @($Here + "\patches\XenonRecomp.patch"))) {
    Run cmake @("-S", $xenon, "-B", (Join-Path $xenon "build-win"), "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")
    Run cmake @("--build", (Join-Path $xenon "build-win"), "--target", "XenonRecomp")
}
$generatedMarker = Join-Path $Recomp "generated\ppc_func_mapping.cpp"
if (UpToDate $generatedMarker @($recompConfig, $linked, $xenonExe)) { Info "generated C++ up to date" }
else {
    Info "this takes a few minutes..."
    Push-Location $Here
    try { Run $xenonExe @("recomp\config\carrot.toml", "recomp\runtime\ppc_context.h") } finally { Pop-Location }
}

# ---------------------------------------------------------------------------------------------
Step "Building pikmin3.exe (the first build takes a while)"

$build = Join-Path $Recomp "build"
if (-not (Test-Path (Join-Path $build "build.ninja"))) {
    Run cmake @("-S", $Recomp, "-B", $build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=clang-cl", "-DCMAKE_CXX_COMPILER=clang-cl")
}
Run cmake @("--build", $build)
# pikmin3.exe looks for the dump next to itself and in parent folders, then in this file.
[System.IO.File]::WriteAllText((Join-Path $build "dump_root.txt"), (Resolve-Path -LiteralPath $dumpRoot).Path)

# ---------------------------------------------------------------------------------------------
Step "Checking the menu shaders (lyt_BuildinShader.gsh)"

$gsh = Join-Path $gameFolder.FullName "content\CMCmn\shader\lyt_BuildinShader.gsh"
$override = Join-Path $Recomp "content_override\CMCmn\shader\lyt_BuildinShader.gsh"
$damaged = $true
if (Test-Path -LiteralPath $gsh) {
    $magic = [System.IO.File]::ReadAllBytes($gsh)[0..3]
    $damaged = -not ($magic[0] -eq 0x47 -and $magic[1] -eq 0x66 -and $magic[2] -eq 0x78 -and $magic[3] -eq 0x32)  # "Gfx2"
}
if ($damaged -or $ForceLytShaders) {
    if ($damaged) { Info "your dump's copy is damaged or missing: building the replacement" }
    Run cmake @("--build", $build, "--target", "latte_assembler")
    New-Item -ItemType Directory -Force (Split-Path $override) | Out-Null
    Run python @((Join-Path $Recomp "shaders\lyt\make_lyt_gsh.py"), (Join-Path $build "gpu\latte_assembler.exe"), $override)
}
elseif (Test-Path $override) {
    Remove-Item $override
    Info "your dump's copy is fine (the previous replacement was removed)"
}
else { Info "your dump's copy is fine" }

# ---------------------------------------------------------------------------------------------
$exe = Join-Path $build "pikmin3.exe"
Write-Host ""
Write-Host "Done! Start the game with:" -ForegroundColor Green
Write-Host "    `"$exe`" --split"
Write-Host "(or run play.bat in the repository folder)"
