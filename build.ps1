<#
.SYNOPSIS
    Build the RamTool RAM payload.

.DESCRIPTION
    Cross-compiles a freestanding Xtensa binary with the ESP-IDF toolchain that
    is already installed on this machine. There is no idf.py involvement and no
    CMake project; this is a direct gcc invocation with a hand-written linker
    script, because the payload has to land at absolute addresses the host
    script can hand straight to the ROM bootloader.

    Produces build\iram.bin and build\dram.bin plus the ELF that holds the
    entry-point symbol.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$build = Join-Path $root 'build'

# Toolchain: the esp-14.2.0 install that ships with ESP-IDF 5.5.4.
$toolchain = 'C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin'
$cc = Join-Path $toolchain 'xtensa-esp32-elf-gcc.exe'
$objcopy = Join-Path $toolchain 'xtensa-esp32-elf-objcopy.exe'
$size = Join-Path $toolchain 'xtensa-esp32-elf-size.exe'

# The ROM symbol tables the linker pulls addresses from.
$romLd = 'C:\esp\v5.5.4\esp-idf\components\esp_rom\esp32\ld'

if (-not (Test-Path $cc)) { throw "compiler not found: $cc" }
if (-not (Test-Path "$romLd\esp32.rom.ld")) { throw "ESP-IDF ROM ld scripts not found under $romLd" }

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
)

if ($Configuration -eq 'Debug') {
    $common += @('-O0', '-g')
} else {
    $common += @('-Os', '-g')
}

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
    @{ file = 'payload.c'; args = $common }
)

foreach ($s in $srcs) {
    $in = Join-Path $root $s.file
    $out = Join-Path $build ($s.file -replace '\.', '_')
    & $cc @($s.args) '-o' $out $in
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($s.file)" }
}

$objects = $srcs | ForEach-Object { Join-Path $build ($_.file -replace '\.', '_') }

$elf = Join-Path $build 'ramtool.elf'
$map = Join-Path $build 'ramtool.map'
$ourLd = Join-Path $root 'ramtool.ld'

# Link against our script first so MEMORY/SECTIONS win, then the ROM symbol
# tables so esp_rom_* resolve to their fixed addresses.
$linkArgs = @(
    '-nostdlib'
    '-nostartfiles'
    ('-Wl,-T,' + $ourLd)
    ('-Wl,-T,' + $romLd + '\esp32.rom.api.ld')
    ('-Wl,-T,' + $romLd + '\esp32.rom.ld')
    ('-Wl,-T,' + $romLd + '\esp32.rom.libgcc.ld')
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

$iram = (Get-Item $iramBin).Length
$dram = (Get-Item $dramBin).Length
Write-Host ''
Write-Host ("iram.bin {0} bytes at 0x40080000" -f $iram) -ForegroundColor Green
Write-Host ("dram.bin {0} bytes at 0x3FFB0000" -f $dram) -ForegroundColor Green
