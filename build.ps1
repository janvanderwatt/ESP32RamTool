<#
.SYNOPSIS
    Build the RamTool RAM payload.

.DESCRIPTION
    Cross-compiles a freestanding binary with an Xtensa ESP32 toolchain.
    There is no idf.py involvement and no
    CMake project; this is a direct gcc invocation with a hand-written linker
    script, because the payload has to land at absolute addresses the host
    script can hand straight to the ROM bootloader.

    Produces build\iram.bin and build\dram.bin plus the ELF that holds the
    entry-point symbol.

    ROM addresses are pinned in rom/esp32.ld; no ESP-IDF checkout or ROM ELF
    extraction is needed. The `sd init` pin override is compiled in by default;
    pass -NoSdPinArgs to drop it and trim the payload. The toolchain is located
    in this order:

      1. -ToolchainPath, if given
      2. $env:IDF_TOOLS_PATH
      3. xtensa-esp32-elf-gcc on PATH

.EXAMPLE
    .\build.ps1
    Uses $env:IDF_TOOLS_PATH, or falls back to whatever is on PATH.

.EXAMPLE
    .\build.ps1 -NoSdPinArgs
    Also compiles out the `sd init` pin-override parser to save a little IRAM.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    # Directory holding xtensa-esp32-elf-gcc
    [string]$ToolchainPath,

    # Compile out the `sd init` pin-override parser and its pad-validation
    # table. `sd init` then always uses the fixed 18/23/19/2/12/32 defaults and
    # rejects arguments. Default is off (override present).
    [switch]$NoSdPinArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$build = Join-Path $root 'build'

# Windows PowerShell 5.1 does not define $IsLinux (it arrived in 6.0), and
# Set-StrictMode turns referencing it into a hard error rather than an empty
# string. Derive it from things that exist in both.
$OnWindows = ($PSVersionTable.PSVersion.Major -lt 6) -or ($env:OS -eq 'Windows_NT')

function Resolve-Executable {
    param([string]$Name)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }
    # Windows resolves PATHEXT behind the scenes; try the usual suffixes too.
    if ($OnWindows) {
        foreach ($ext in @('.exe', '.bat', '.cmd')) {
            $onPath = Get-Command ($Name + $ext) -ErrorAction SilentlyContinue
            if ($onPath) {
                return $onPath.Source
            }
        }
    }
    return $null
}

# ---- locate the toolchain ------------------------------------------------

# Work from the compiler's real path rather than a guessed one. On Windows the
# binary is xtensa-esp32-elf-gcc.exe and on Linux it has no suffix, so the
# extension is taken from whatever was actually found and reused for the
# companion tools rather than assumed.
$cc = $null
$toolDir = $null
$toolExt = ''

function Find-GccIn {
    param([string]$Dir, [int]$Depth)

    $hit = Get-ChildItem -Path $Dir -Filter 'xtensa-esp32-elf-gcc*' -File `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $hit -and $Depth -gt 0) {
        $hit = Get-ChildItem -Path $Dir -Filter 'xtensa-esp32-elf-gcc*' -File `
            -Recurse -Depth $Depth -ErrorAction SilentlyContinue |
            Select-Object -First 1
    }
    return $hit
}

# 1. An explicit -ToolchainPath, or the directory it names.
if ($ToolchainPath) {
    $hit = Find-GccIn -Dir $ToolchainPath -Depth 2
    if ($hit) {
        $cc = $hit.FullName
        $toolDir = $hit.DirectoryName
        $toolExt = $hit.Extension
    }
}

# 2. An ESP-IDF install exports IDF_TOOLS_PATH. The compiler sits several levels
#    below it inside a versioned folder, and some installs nest a further
#    'tools' directory, so the depth here is deliberately generous.
if (-not $cc -and $env:IDF_TOOLS_PATH) {
    $hit = Find-GccIn -Dir $env:IDF_TOOLS_PATH -Depth 6
    if ($hit) {
        $cc = $hit.FullName
        $toolDir = $hit.DirectoryName
        $toolExt = $hit.Extension
    }
}

# 3. Whatever is on PATH.
if (-not $cc) {
    $onPath = Resolve-Executable 'xtensa-esp32-elf-gcc'
    if ($onPath) {
        $cc = $onPath
        $toolDir = Split-Path $onPath -Parent
        $toolExt = [System.IO.Path]::GetExtension($onPath)
    }
}

if (-not $cc) {
    throw @'
Cannot find the Xtensa toolchain.

Install an Xtensa ESP32 toolchain and set IDF_TOOLS_PATH, or put
xtensa-esp32-elf-gcc on PATH, or pass -ToolchainPath explicitly.

The container image in this repository already has all of it:
    docker build -t ramtool .
'@
}

$objcopy = Join-Path $toolDir ('xtensa-esp32-elf-objcopy' + $toolExt)
$size = Join-Path $toolDir ('xtensa-esp32-elf-size' + $toolExt)
$nm = Join-Path $toolDir ('xtensa-esp32-elf-nm' + $toolExt)
foreach ($tool in @($objcopy, $size, $nm)) {
    if (-not (Test-Path $tool)) {
        $resolved = Resolve-Executable (Split-Path $tool -Leaf)
        if ($resolved) {
            $tool = $resolved
        } else {
            throw "companion tool missing next to the compiler: $(Split-Path $tool -Leaf)"
        }
    }
}

# ROM bindings are versioned with the payload, independent of installed SDKs.
$romLd = Join-Path $root 'rom/esp32.ld'
if (-not (Test-Path $romLd)) { throw "pinned ROM table missing: $romLd" }

New-Item -ItemType Directory -Force -Path $build | Out-Null

# -mlongcalls is mandatory: our code sits at 0x40080000 and the ROM functions
# it calls live around 0x4000xxxx, which is outside the 16-bit range of the
# plain CALL instruction. The literal pools this forces go in .xt.lit, which
# the linker script keeps in IRAM.
$common = @(
    '-c'
    '-mlongcalls'
    '-mabi=windowed'
    '-ffreestanding'
    '-fno-builtin'
    '-fno-stack-protector'
    '-fno-pic'
    '-fno-asynchronous-unwind-tables'
    '-fno-unwind-tables'
    '-fomit-frame-pointer'
    '-fno-common'
    '-std=gnu11'
    '-Wall'
    '-Wextra'
    '-Werror'
    "-I$root"
    # Our own code includes littlefs's headers (lfsvol.c uses lfs_t and the
    # error codes), so this path is needed by more than littlefs's own
    # compilation.
    "-I" + (Join-Path $root 'littlefs')
)

if ($Configuration -eq 'Debug') {
    $common += @('-O0', '-g')
} else {
    $common += @('-Os', '-g')
}

# RAMTOOL_SD_PIN_ARGS defaults to 1 (see sdspi.h). -NoSdPinArgs trims the
# payload by compiling the pin-override parser and pad checks out.
if ($NoSdPinArgs) { $common += '-DRAMTOOL_SD_PIN_ARGS=0' }

# start.S goes through the C preprocessor too so the shared defines apply.
$asmCommon = @(
    '-c'
    '-mlongcalls'
    '-mabi=windowed'
    '-ffreestanding'
    '-fno-pic'
    "-I$root"
)

$srcs = @(
    @{ file = 'start.S';   args = $asmCommon },
    @{ file = 'payload.c'; args = $common },
    @{ file = 'lfsvol.c';  args = $common },
    @{ file = 'sdgpio.c';  args = $common },
    @{ file = 'sdspi.c';   args = $common },
    @{ file = 'sdvol.c';   args = $common },
    @{ file = 'fatfs/ff.c'; args = $common },
    @{ file = 'fatfs/ffunicode.c'; args = $common }
)

# littlefs is third-party and does not compile clean under our warning set, so
# it gets its own flags rather than weakening the flags for our own code.
#
# The LFS_NO_* set matters a great deal here:
#   NO_MALLOC      use the buffers we supply instead of an allocator; there is
#                  no heap in this payload
#   NO_THREADSAFE  drop the mutex wrappers; there is only one task
#   NO_DEBUG/NO_WARN/NO_ERROR  these all expand to printf, and there is no
#                  printf. Without them the link fails on undefined references.
#   NO_ASSERT      drops assert(); assert is not available either
#
# -I littlefs is what lets lfs.c find its own lfs.h and lfs_util.h.
$lfsArgs = @(
    '-c'
    '-mlongcalls'
    '-mabi=windowed'
    '-ffreestanding'
    '-fno-builtin'
    '-fno-stack-protector'
    '-fno-pic'
    '-fno-asynchronous-unwind-tables'
    '-fno-unwind-tables'
    '-fomit-frame-pointer'
    '-std=gnu11'
    '-Os'
    '-DLFS_NO_DEBUG'
    '-DLFS_NO_WARN'
    '-DLFS_NO_ERROR'
    '-DLFS_NO_ASSERT'
    '-DLFS_NO_MALLOC'
    '-DLFS_NO_THREADSAFE'
    "-I" + (Join-Path $root 'littlefs')
)
# Source paths use forward slashes: they resolve on Linux, and Join-Path leaves
# them alone on Windows.
$srcs += @{ file = 'littlefs/lfs.c'; args = $lfsArgs }

foreach ($s in $srcs) {
    $in = Join-Path $root $s.file
    # Flatten the path into the object name: littlefs/lfs.c must not become an
    # object inside a directory that does not exist.
    $stem = ($s.file -replace '[\\/]', '_') -replace '\.', '_'
    $out = Join-Path $build $stem
    & $cc @($s.args) '-o' $out $in
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($s.file)" }
}

$objects = $srcs | ForEach-Object {
    $stem = ($_.file -replace '[\\/]', '_') -replace '\.', '_'
    Join-Path $build $stem
}

$elf = Join-Path $build 'ramtool.elf'
$map = Join-Path $build 'ramtool.map'
$ourLd = Join-Path $root 'ramtool.ld'

# Link against our script first so MEMORY/SECTIONS win, then the ROM symbol
# tables so esp_rom_* resolve to their fixed addresses.
$linkArgs = @(
    '-nostdlib'
    '-nostartfiles'
    ('-Wl,-T,' + $ourLd)
    ('-Wl,-T,' + $romLd)
    '-Wl,--gc-sections'
    ('-Wl,-Map=' + $map)
    '-Wl,--no-warn-rwx-segments'
    '-o'
    $elf
) + $objects

& $cc @linkArgs
if ($LASTEXITCODE -ne 0) { throw 'link failed' }

# Two flat images, because the host loads them as two separate transfers.
# --only-section keeps the output at the section's own address, so the host can
# pass the addresses straight through to MEM_BEGIN.
$iramBin = Join-Path $build 'iram.bin'
$dramBin = Join-Path $build 'dram.bin'

& $objcopy '-O' 'binary' '--only-section=.iram_img' $elf $iramBin
if ($LASTEXITCODE -ne 0) { throw 'objcopy (.iram_img) failed' }

& $objcopy '-O' 'binary' '--only-section=.dram_img' $elf $dramBin
if ($LASTEXITCODE -ne 0) { throw 'objcopy (.dram_img) failed' }

& $size $elf

# A tiny manifest of the three addresses run.py needs, so that *running* the
# payload needs neither a toolchain nor the ELF -- just the two .bin files and
# this. The values are still read out of the ELF, so ramtool.ld remains the single
# source of truth and the host cannot drift away from it silently.
$symbols = @('ramtool_entry', '_ramtool_iram_origin', '_ramtool_dram_origin')
$manifestLines = foreach ($sym in $symbols) {
    $value = & $nm $elf |
        Select-String -Pattern "^\s*[0-9a-fA-F]+\s+[tT]\s+$([regex]::Escape($sym))\s*$" |
        ForEach-Object { ($_.Line -split '\s+')[0] } |
        Select-Object -First 1
    if (-not $value) { throw "symbol not found in the ELF: $sym" }
    "$sym=0x$value"
}
$manifest = Join-Path $build 'manifest.txt'
$manifestLines | Set-Content -Path $manifest -Encoding ascii

$iram = (Get-Item $iramBin).Length
$dram = (Get-Item $dramBin).Length
Write-Host ''
Write-Host ("iram.bin {0} bytes at 0x40080000" -f $iram) -ForegroundColor Green
Write-Host ("dram.bin {0} bytes at 0x3FFB0000" -f $dram) -ForegroundColor Green

# ---- create dist folder (minimal runtime) -----------------------------------
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null

Copy-Item $iramBin (Join-Path $dist 'iram.bin') -Force
Copy-Item $dramBin (Join-Path $dist 'dram.bin') -Force
Copy-Item $manifest (Join-Path $dist 'manifest.txt') -Force
Copy-Item (Join-Path $root 'run.py') (Join-Path $dist 'run.py') -Force

# Minimal usage note for Windows users
$usage = @"
RamTool — Windows distribution
==============================

Files in this folder:
  iram.bin     - IRAM payload (loads at 0x40080000)
  dram.bin     - DRAM payload (loads at 0x3FFB0000)
  manifest.txt - Load addresses + entry point
  run.py       - Host loader script

Requirements:
  - Python 3.8+
  - pip install esptool pyserial

Usage:
  python run.py --port COM3

  Options:
    --seconds N   Stay attached for N seconds, then exit
    --boot        Enter download mode and stay there

The payload runs entirely from RAM. Nothing is written to flash.
"@
Set-Content -Path (Join-Path $dist 'README.txt') -Value $usage -Encoding utf8

Write-Host ''
Write-Host "dist/ folder created at $dist" -ForegroundColor Cyan
Write-Host "Copy the entire dist/ folder to any Windows PC to run RamTool." -ForegroundColor Cyan
