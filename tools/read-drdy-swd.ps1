param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]+$')]
    [string]$ProbeSerial,
    [Parameter(Mandatory = $true)]
    [string]$FirmwareDirectory,
    [Parameter(Mandatory = $true)]
    [string]$CubeCltRoot
)

$ErrorActionPreference = 'Stop'
$nm = Join-Path $CubeCltRoot 'GNU-tools-for-STM32\bin\arm-none-eabi-nm.exe'
$programmer = Join-Path $CubeCltRoot 'STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$symbols = & $nm -S (Join-Path $FirmwareDirectory '2017H.elf') | Out-String
if ($LASTEXITCODE -ne 0) { throw 'Cannot read firmware symbols.' }

function Get-Symbol {
    param([string]$Name, [string]$Type, [uint32]$Size)
    $line = [regex]::Match($symbols, "(?m)^([0-9a-f]{8}) ([0-9a-f]{8}) $Type $Name\s*$")
    if (-not $line.Success -or [Convert]::ToUInt32($line.Groups[2].Value, 16) -ne $Size) {
        throw "Missing or incompatible diagnostic symbol: $Name"
    }
    return [Convert]::ToUInt32($line.Groups[1].Value, 16)
}

$tagAddress = Get-Symbol 'g_drdy_diag_tag' 'R' 4
$resultAddress = Get-Symbol 'g_drdy_diag' 'B' 40
if ($tagAddress -lt 0x08000000 -or $tagAddress -ge 0x08080000 -or
    $resultAddress -lt 0x20000000 -or $resultAddress + 40 -gt 0x20020000) {
    throw 'Unexpected Flash or DTCM address.'
}
$tag = '0x{0:X8}' -f $tagAddress
$address = '0x{0:X8}' -f $resultAddress
$text = & $programmer -c port=SWD "sn=$ProbeSerial" mode=HOTPLUG freq=1000 -r32 $tag 4 -r32 $address 40 | Out-String
if ($LASTEXITCODE -ne 0 -or $text -match '(?im)^\s*Error\s*:') { throw "SWD read failed: $text" }

function Get-Words {
    param([uint32]$Address, [int]$Count)
    $hex = '0x{0:X8}' -f $Address
    $row = [regex]::Match($text, "(?im)^\s*$hex\s*:\s*([0-9a-f]{8}(?:[ \t]+[0-9a-f]{8}){$($Count - 1)})\b")
    if (-not $row.Success) { throw "Missing SWD row $hex." }
    $row.Groups[1].Value -split '\s+' | ForEach-Object { [Convert]::ToUInt32($_, 16) }
}

if ((Get-Words $tagAddress 1) -ne 0x44524431) { throw 'DRDY diagnostic firmware is not installed.' }
$data = @(Get-Words $resultAddress 4; Get-Words ($resultAddress + 16) 4; Get-Words ($resultAddress + 32) 2)
if ($data[0] -ne 0x44524431) { throw 'Diagnostic has not started.' }
switch ($data[1]) {
    1 { throw 'Capture still running; wait a second and retry.' }
    2 { $status = 'Complete' }
    3 { throw 'Diagnostic timer clock is unsupported.' }
    4 { $status = 'Capture limit reached; observed counts are incomplete' }
    default { throw 'Unexpected diagnostic status.' }
}
if ($data[2] -ne 100000 -or $data[3] -gt 50000) { throw 'Invalid timer window.' }

[pscustomobject]@{
    Status = $status
    Window_ms = $data[3] * 1000.0 / $data[2]
    CapturedFallingEdges = $data[4]
    ObservedOvercaptureEvents = $data[5]
    MinGap_ms = $(if ($data[4] -ge 2) { $data[6] * 1000.0 / $data[2] })
    MaxGap_ms = $(if ($data[4] -ge 2) { $data[7] * 1000.0 / $data[2] })
    GapsUnder1ms = $data[8]
    GapsBetween8And12ms = $data[9]
}
