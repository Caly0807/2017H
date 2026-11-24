param(
    [ValidateSet('Sweep', 'Detector', 'Drdy', 'Console')]
    [string]$Mode = 'Sweep',
    [ValidateRange(1, 1023)]
    [int]$AmplitudeCode = 16,
    [Parameter(Mandatory = $true)]
    [string]$CubeCltRoot,
    [ValidateRange(0, 1)]
    [int]$DetectorChannel = 0
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$cmake = Join-Path $CubeCltRoot 'CMake\bin\cmake.exe'
$ninja = Join-Path $CubeCltRoot 'Ninja\bin\ninja.exe'
$compiler = Join-Path $CubeCltRoot 'GNU-tools-for-STM32\bin'
$commit = git -C $repo rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') { throw 'Cannot resolve development base.' }
$changes = @(git -C $repo status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read development status.' }
$tag = (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + "-$Mode-" + $commit.Substring(0, 7)
$build = Join-Path $repo "build\$tag"
$package = Join-Path $repo "build\dist\dev-$tag"
$auto = if ($Mode -in @('Sweep', 'Detector')) { 'ON' } else { 'OFF' }
$diag = if ($Mode -eq 'Drdy') { 'ON' } else { 'OFF' }
$detector = if ($Mode -eq 'Detector') { 'ON' } else { 'OFF' }
$before = $env:Path
try {
    $env:Path = "$compiler;$before"
    $configure = @('-S', $repo, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                   "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$repo/cmake/gcc-arm-none-eabi.cmake",
                   "-DENABLE_AUTO_SWEEP=$auto", "-DENABLE_DRDY_DIAG=$diag", "-DENABLE_DETECTOR_TEST=$detector")
    if ($Mode -eq 'Sweep') { $configure += "-DSWEEP_AMPLITUDE_CODE=$AmplitudeCode" }
    if ($Mode -eq 'Detector') { $configure += "-DDETECTOR_ADC_CHANNEL=$DetectorChannel" }
    & $cmake @configure
    if ($LASTEXITCODE -ne 0) { throw 'Development configure failed.' }
    & $cmake --build $build --parallel 8
    if ($LASTEXITCODE -ne 0) { throw 'Development build failed.' }
} finally {
    $env:Path = $before
}

$bin = [IO.File]::ReadAllBytes((Join-Path $build '2017H.bin'))
if ($bin.Length -lt 8 -or $bin.Length -gt 0x80000) { throw 'Unexpected binary size.' }
$reset = [BitConverter]::ToUInt32($bin, 4)
if ([BitConverter]::ToUInt32($bin, 0) -ne 0x20020000 -or $reset -lt 0x08000001 -or
    $reset -ge (0x08000000 + $bin.Length) -or ($reset -band 1) -ne 1) { throw 'Unexpected vectors.' }
New-Item -ItemType Directory -Path $package | Out-Null
$hashes = [ordered]@{}
foreach ($extension in @('hex', 'bin', 'elf', 'map')) {
    $name = "2017H.$extension"
    Copy-Item -LiteralPath (Join-Path $build $name) -Destination $package
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $package $name)).Hash
}
[ordered]@{
    created = Get-Date -Format o
    sourceCommit = $commit
    worktreeChanges = $changes
    target = 'STM32H743VGT6, LQFP100, teammate wiring'
    startAddress = '0x08000000'
    configuration = 'Release'
    purpose = $Mode
    amplitudeCode = $(if ($Mode -eq 'Sweep') { $AmplitudeCode })
    detectorChannel = $(if ($Mode -eq 'Detector') { $DetectorChannel })
    binaryBytes = $bin.Length
    hardwareVerified = $false
    sha256 = $hashes
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $package 'manifest.json') -Encoding UTF8
Write-Output "Development package (not programmed): $package"
