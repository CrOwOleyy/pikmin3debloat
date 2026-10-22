# Setup script for pikmin3debloat development tools
# Downloads and prepares: Ghidra 12.1.2 + SwitchLoader, nstool, nvdisasm, nx2elf, ssbu-recomp, Python venv

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Split-Path -Parent $ScriptDir
$ToolsDir = Join-Path $ProjectRoot "tools"

Write-Host "=== Setting up tools in $ToolsDir ===" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null

function Download-FileWithProgress($url, $dest) {
    if (-not (Test-Path $dest)) {
        Write-Host "Downloading $url -> $dest" -ForegroundColor Yellow
        $destDir = Split-Path -Parent $dest
        New-Item -ItemType Directory -Force -Path $destDir | Out-Null
        Invoke-WebRequest -Uri $url -OutFile $dest
    } else {
        Write-Host "Already downloaded: $dest" -ForegroundColor Green
    }
}

# 1. Python virtual environment
$VenvDir = Join-Path $ToolsDir "venv"
if (-not (Test-Path $VenvDir)) {
    Write-Host "`n[1/6] Creating Python virtual environment in $VenvDir..." -ForegroundColor Cyan
    python -m venv $VenvDir
    $Pip = Join-Path $VenvDir "Scripts\pip.exe"
    $ReqFile = Join-Path $ProjectRoot "requirements.txt"
    if (Test-Path $ReqFile) {
        & $Pip install -r $ReqFile
    }
} else {
    Write-Host "`n[1/6] Python venv already exists at $VenvDir" -ForegroundColor Green
}

# 2. Ghidra 12.1.2 PUBLIC
$GhidraZip = Join-Path $ToolsDir "ghidra\ghidra_12.1.2_PUBLIC_20260605.zip"
$GhidraHome = Join-Path $ToolsDir "ghidra\ghidra_12.1.2_PUBLIC"
if (-not (Test-Path $GhidraHome)) {
    Write-Host "`n[2/6] Setting up Ghidra 12.1.2..." -ForegroundColor Cyan
    $GhidraUrl = "https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.1.2_build/ghidra_12.1.2_PUBLIC_20260605.zip"
    Download-FileWithProgress $GhidraUrl $GhidraZip
    Write-Host "Extracting Ghidra..." -ForegroundColor Yellow
    Expand-Archive -Path $GhidraZip -DestinationPath (Join-Path $ToolsDir "ghidra") -Force
} else {
    Write-Host "`n[2/6] Ghidra already present at $GhidraHome" -ForegroundColor Green
}

# 3. SwitchLoader extension for Ghidra
$SwitchLoaderDest = Join-Path $GhidraHome "Ghidra\Extensions\SwitchLoader"
if (-not (Test-Path $SwitchLoaderDest)) {
    Write-Host "`n[3/6] Setting up SwitchLoader extension..." -ForegroundColor Cyan
    $LoaderZip = Join-Path $ToolsDir "ghidra-loader-dl\SwitchLoader-1.6.1-c600d05-Ghidra_12.1.2.zip"
    $LoaderUrl = "https://github.com/Adubbz/Ghidra-Switch-Loader/releases/download/1.6.1-c600d05-Ghidra_12.1.2/SwitchLoader-1.6.1-c600d05-Ghidra_12.1.2.zip"
    Download-FileWithProgress $LoaderUrl $LoaderZip
    Write-Host "Extracting SwitchLoader into Ghidra extensions..." -ForegroundColor Yellow
    Expand-Archive -Path $LoaderZip -DestinationPath (Join-Path $GhidraHome "Ghidra\Extensions") -Force
} else {
    Write-Host "`n[3/6] SwitchLoader already present at $SwitchLoaderDest" -ForegroundColor Green
}

# 4. nstool v1.9.2
$NstoolDir = Join-Path $ToolsDir "nstool"
$NstoolExe = Join-Path $NstoolDir "nstool.exe"
if (-not (Test-Path $NstoolExe)) {
    Write-Host "`n[4/6] Setting up nstool v1.9.2..." -ForegroundColor Cyan
    $NstoolZip = Join-Path $NstoolDir "nstool-v1.9.2-win_x64.zip"
    $NstoolUrl = "https://github.com/Jakcron/nstool/releases/download/v1.9.2/nstool-v1.9.2-win_x64.zip"
    Download-FileWithProgress $NstoolUrl $NstoolZip
    Expand-Archive -Path $NstoolZip -DestinationPath $NstoolDir -Force
} else {
    Write-Host "`n[4/6] nstool already present at $NstoolExe" -ForegroundColor Green
}

# 5. nvdisasm (CUDA 12.9.88)
$NvdisasmDir = Join-Path $ToolsDir "nvdisasm"
$NvdisasmExe = Join-Path $NvdisasmDir "nvdisasm.exe"
if (-not (Test-Path $NvdisasmExe)) {
    Write-Host "`n[5/6] Setting up nvdisasm..." -ForegroundColor Cyan
    $NvdisasmZip = Join-Path $ToolsDir "nvdisasm-dl\cuda_nvdisasm-windows-x86_64-12.9.88-archive.zip"
    $NvdisasmUrl = "https://developer.download.nvidia.com/compute/cuda/redist/cuda_nvdisasm/windows-x86_64/cuda_nvdisasm-windows-x86_64-12.9.88-archive.zip"
    Download-FileWithProgress $NvdisasmUrl $NvdisasmZip
    $TempExtract = Join-Path $ToolsDir "nvdisasm-dl\extract"
    Expand-Archive -Path $NvdisasmZip -DestinationPath $TempExtract -Force
    New-Item -ItemType Directory -Force -Path $NvdisasmDir | Out-Null
    Copy-Item "$TempExtract\*\bin\nvdisasm.exe" $NvdisasmExe -Force
    Remove-Item -Recurse -Force $TempExtract -ErrorAction SilentlyContinue
} else {
    Write-Host "`n[5/6] nvdisasm already present at $NvdisasmExe" -ForegroundColor Green
}

# 6. Git repositories: nx2elf & ssbu-recomp
Write-Host "`n[6/6] Checking git repositories..." -ForegroundColor Cyan
$Nx2elfDir = Join-Path $ToolsDir "nx2elf-src"
if (-not (Test-Path $Nx2elfDir)) {
    Write-Host "Cloning nx2elf..." -ForegroundColor Yellow
    git clone https://github.com/shuffle2/nx2elf.git $Nx2elfDir
    if (Get-Command cmake -ErrorAction SilentlyContinue) {
        Write-Host "Building nx2elf..." -ForegroundColor Yellow
        cmake -B "$Nx2elfDir\build" -S $Nx2elfDir
        cmake --build "$Nx2elfDir\build" --config Release
    }
} else {
    Write-Host "nx2elf repo already present at $Nx2elfDir" -ForegroundColor Green
}

$SsbuDir = Join-Path $ToolsDir "ssbu-recomp"
if (-not (Test-Path $SsbuDir)) {
    Write-Host "Cloning ssbu-recomp..." -ForegroundColor Yellow
    git clone --depth 1 https://github.com/Denkishi/ssbu-recomp.git $SsbuDir
} else {
    Write-Host "ssbu-recomp repo already present at $SsbuDir" -ForegroundColor Green
}

Write-Host "`n=== Tools setup complete! ===" -ForegroundColor Green
