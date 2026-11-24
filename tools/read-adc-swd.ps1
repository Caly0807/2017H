param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]+$')]
    [string]$ProbeSerial,
    [ValidateRange(2, 100)]
    [int]$Count = 5,
    [ValidateRange(100, 10000)]
    [int]$IntervalMs = 500,
    [Parameter(Mandatory = $true)]
    [string]$CubeCltRoot,
    [Parameter(Mandatory = $true)]
    [string]$BaselineHex
)

$ErrorActionPreference = 'Stop'
$programmer = Join-Path $CubeCltRoot 'STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
if ((Get-FileHash -LiteralPath $baselineHex -Algorithm SHA256).Hash -ne 'FFF012F743236E390BA5A9A55CEA35ADA005D67F9A5AF2EAC260669A0376257B') {
    throw 'The baseline HEX hash does not match the supported firmware.'
}
$connection = @('-c', 'port=SWD', "sn=$ProbeSerial", 'mode=HOTPLUG', 'freq=1000')

function Invoke-Read {
    param([string[]]$ReadArgs)
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $result = & $programmer @connection @ReadArgs 2>&1 | Out-String
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0 -or $result -match '(?im)^\s*Error\s*:') {
        throw "CubeProgrammer read failed ($exitCode): $result"
    }
    return $result
}

$readback = Join-Path $env:TEMP ('2017H-swd-' + [guid]::NewGuid().ToString('N') + '.bin')
try {
    $null = Invoke-Read @('-u', '0x08000000', '31556', $readback)
    $bytes = [IO.File]::ReadAllBytes($readback)
    if ($bytes.Length -ne 31556) { throw 'Incomplete Flash readback.' }
    $upper = 0
    foreach ($line in [IO.File]::ReadLines($baselineHex)) {
        switch ($line.Substring(7, 2)) {
            '04' { $upper = [Convert]::ToInt32($line.Substring(9, 4), 16) * 65536 }
            '00' {
                $length = [Convert]::ToInt32($line.Substring(1, 2), 16)
                $offset = $upper + [Convert]::ToInt32($line.Substring(3, 4), 16) - 0x08000000
                if ($offset -lt 0 -or $offset + $length -gt $bytes.Length) { throw 'HEX data outside readback.' }
                for ($index = 0; $index -lt $length; ++$index) {
                    if ($bytes[$offset + $index] -ne [Convert]::ToByte($line.Substring(9 + 2 * $index, 2), 16)) {
                        throw ('Flash does not match baseline at 0x{0:X8}; ADC address not used.' -f (0x08000000 + $offset + $index))
                    }
                }
            }
            '01' {}
            '05' {}
            default { throw 'Unexpected baseline HEX record type.' }
        }
    }
}
finally {
    if (Test-Path -LiteralPath $readback) { Remove-Item -LiteralPath $readback }
}

function Get-Words {
    param([string]$Text, [string]$Address, [int]$WordCount)
    $line = [regex]::Match($Text, "(?im)^\s*$Address\s*:\s*([0-9a-f]{8}(?:[ \t]+[0-9a-f]{8}){$($WordCount - 1)})\b")
    if (-not $line.Success) { throw "Missing ADC readback row $Address." }
    $line.Groups[1].Value -split '\s+' | ForEach-Object { [Convert]::ToUInt32($_, 16) }
}

for ($index = 0; $index -lt $Count; ++$index) {
    $text = Invoke-Read @('-r32', '0x20000028', '44', '-r32', '0x200000CC', '4')
    $settings = @(Get-Words $text '0x20000028' 4)
    $sample = @(Get-Words $text '0x20000038' 4)
    $state = @(Get-Words $text '0x20000048' 3)
    $tick = Get-Words $text '0x200000CC' 1
    $initialized = (($state[1] -shr 24) -eq 1)
    $hasSample = (($state[2] -band 0xFF) -eq 1)
    $age = ([long]$tick - [long]$sample[0] + 4294967296) % 4294967296
    $fresh = $initialized -and $hasSample -and ($age -le 1000)
    $raw = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$sample[2]), 0)
    $voltage = [BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]$sample[3]), 0)
    [pscustomobject]@{
        Time = Get-Date -Format o
        Input = 'AIN{0}-AINCOM' -f ($state[1] -band 0xFF)
        Gain = ($state[1] -shr 8) -band 0xFF
        RateSettingSps = $settings[2]
        SoftwareRef_mV = $settings[1] / 1000.0
        Initialized = $initialized
        HasSample = $hasSample
        Tick_ms = $tick
        SampleAge_ms = $(if ($hasSample) { $age })
        FirmwareCount = $settings[3]
        DrdyEdges = $sample[1]
        RAW = $(if ($fresh) { $raw })
        Firmware_mV = $(if ($fresh) { $voltage / 1000.0 })
    }
    if ($index + 1 -lt $Count) { Start-Sleep -Milliseconds $IntervalMs }
}
