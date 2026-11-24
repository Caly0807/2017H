param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]+$')]
    [string]$ProbeSerial,
    [Parameter(Mandatory = $true)]
    [string]$FirmwareDirectory,
    [string]$OutputDirectory = '',
    [Parameter(Mandatory = $true)]
    [string]$CubeCltRoot,
    [switch]$DetectorTest
)

$ErrorActionPreference = 'Stop'
$nm = Join-Path $CubeCltRoot 'GNU-tools-for-STM32\bin\arm-none-eabi-nm.exe'
$programmer = Join-Path $CubeCltRoot 'STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
$symbols = & $nm -S (Join-Path $FirmwareDirectory '2017H.elf') | Out-String
if ($LASTEXITCODE -ne 0) { throw 'Cannot read sweep firmware symbols.' }

function Get-Symbol {
    param([string]$Name, [string]$Type, [uint32]$Size)
    $line = [regex]::Match($symbols, "(?m)^([0-9a-f]{8}) ([0-9a-f]{8}) $Type $Name\s*$")
    if (-not $line.Success -or [Convert]::ToUInt32($line.Groups[2].Value, 16) -ne $Size) {
        throw "Missing or incompatible firmware symbol: $Name"
    }
    [Convert]::ToUInt32($line.Groups[1].Value, 16)
}

$tag = Get-Symbol 'g_auto_sweep_tag' 'R' 4
$address = Get-Symbol 'g_auto_sweep' 'B' 1032
if ($tag -lt 0x08000000 -or $tag -ge 0x08080000 -or
    $address -lt 0x20000000 -or $address + 1032 -gt 0x20020000) {
    throw 'Unexpected Flash or DTCM address.'
}
$connection = @('-c', 'port=SWD', "sn=$ProbeSerial", 'mode=HOTPLUG', 'freq=1000')

function Read-Words {
    param([uint32]$Start, [int]$Count)
    $text = & $programmer @connection -r32 ('0x{0:X8}' -f $Start) ($Count * 4) | Out-String
    if ($LASTEXITCODE -ne 0 -or $text -match '(?im)^\s*Error\s*:') { throw "SWD read failed: $text" }
    $next = [uint32]$Start
    foreach ($row in [regex]::Matches($text, '(?im)^\s*0x([0-9a-f]{8})\s*:\s*([0-9a-f]{8}(?:[ \t]+[0-9a-f]{8})*)\s*$')) {
        if ([Convert]::ToUInt32($row.Groups[1].Value, 16) -ne $next) { throw 'Unexpected SWD row address.' }
        foreach ($word in ($row.Groups[2].Value -split '\s+')) {
            [Convert]::ToUInt32($word, 16)
            $next += 4
        }
    }
    if ($next -ne $Start + 4 * $Count) { throw 'Incomplete SWD readout.' }
}

$expectedTag = if ($DetectorTest) { 0x44544331 } else { 0x53575031 }
$expectedRecords = if ($DetectorTest) { 25 } else { 41 }
if ((Read-Words $tag 1) -ne $expectedTag) { throw 'The selected firmware type is not installed.' }
$header = @(Read-Words $address 12)
if ($header[0] -ne $expectedTag) { throw 'The selected firmware has not started.' }
if ($header[1] -ne 2 -or $header[2] -ne $expectedRecords) {
    $states = @{ 1='running'; 2='inconsistent record count'; 3='DDS init failed'; 4='ADC init failed'; 5='DDS write failed'; 6='ADC read failed'; 7='ADC timeout' }
    $state = $states[[int]$header[1]]
    throw ('Capture: {0} (state {1}); {2}/{3} rows; current or failed frequency {4} Hz; mute SPI command OK: {5}.' -f $state,$header[1],$header[2],$expectedRecords,$header[8],$header[11])
}
$data = @(for ($record = 0; $record -lt $expectedRecords; $record += 8) {
    Read-Words ($address + 48 + 24 * $record) (6 * [Math]::Min(8, $expectedRecords - $record))
})

function To-Signed {
    param([uint32]$Value)
    [BitConverter]::ToInt32([BitConverter]::GetBytes($Value), 0)
}

$baseline = To-Signed $data[1]
$points = @(
    $schedule = @(16,64,128,256,256,128,64,16,64,256,16,128)
    for ($index = 0; $index -lt $expectedRecords; ++$index) {
        $offset = $index * 6
        $frequency = 1000000 * $index
        $amplitude = if ($index) { $header[3] } else { 0 }
        if ($DetectorTest) {
            $frequency = 100000
            $amplitude = if ($index % 2) { $schedule[($index -shr 1)] } else { 0 }
            if ($data[$offset] -ne $amplitude) { throw "Unexpected amplitude at detector row $index." }
        } elseif ($data[$offset] -ne $frequency) { throw "Unexpected frequency at sweep row $index." }
        $mean = To-Signed $data[$offset + 1]
        $min = To-Signed $data[$offset + 2]
        $max = To-Signed $data[$offset + 3]
        if ($min -gt $mean -or $mean -gt $max -or $min -lt -8388608 -or $max -gt 8388607) {
            throw "Invalid ADC statistics at sweep row $index."
        }
        [pscustomobject]@{
            Frequency_Hz = $frequency
            DDS_Output_Command = $(if ($amplitude) { 'Tone' } else { 'Off' })
            DDS_Amplitude_Code = $amplitude
            Mean_RAW = $mean
            Min_RAW = $min
            Max_RAW = $max
            Delta_From_Off_RAW = $mean - $baseline
            Uncalibrated_Input_uV = To-Signed $data[$offset + 4]
            Sample_Span_ms = $data[$offset + 5]
        }
    }
)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) ('build\sweeps\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$basename = if ($DetectorTest) { 'detector' } else { 'sweep' }
$points | Export-Csv -LiteralPath (Join-Path $OutputDirectory "$basename.csv") -Encoding UTF8 -NoTypeInformation
[ordered]@{
    capturedAt = Get-Date -Format o
    deviceSerial = $ProbeSerial
    status = 'complete'
    source = 'swd'
    tones = $(if ($DetectorTest) { 12 } else { 40 })
    records = $header[2]
    elapsed_ms = $header[7]
    settle_ms = $header[5]
    samplesPerRecord = $header[6]
    amplitudeCode = $header[3]
    adcRateSettingSps = $header[9]
    adcChannel = $header[10]
    softwareRef_uV = $header[4]
    muteCommandOK = [bool]$header[11]
    analogCalibrated = $false
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory "$basename.json") -Encoding UTF8
Write-Output (Get-Item -LiteralPath $OutputDirectory).FullName
