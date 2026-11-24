param(
    [string]$FirmwareDirectory = '',
    [string]$ProbeSerial = '',
    [switch]$ConfirmTeammateBoard,
    [Parameter(Mandatory = $true)]
    [string]$CubeCltRoot
)

$ErrorActionPreference = 'Stop'
$programmer = Join-Path $CubeCltRoot 'STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe'
if (-not (Test-Path -LiteralPath $programmer)) {
    throw "Missing programmer: $programmer"
}
if (-not $ConfirmTeammateBoard) {
    & $programmer -l st-link-only
    if ($LASTEXITCODE -ne 0) {
        throw 'ST-Link enumeration failed'
    }
    Write-Output 'Read-only listing only. To back up and program, specify -ProbeSerial <SN> -ConfirmTeammateBoard.'
    exit 0
}
if ([string]::IsNullOrWhiteSpace($ProbeSerial)) {
    throw 'Specify the ST-Link serial with -ProbeSerial to avoid programming another connected board.'
}
if ([string]::IsNullOrWhiteSpace($FirmwareDirectory)) {
    throw 'Specify the exact package with -FirmwareDirectory before programming.'
}
$manifest = Get-Content -LiteralPath (Join-Path $FirmwareDirectory 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.target -ne 'STM32H743VGT6, LQFP100, teammate wiring' -or $manifest.startAddress -ne '0x08000000') {
    throw 'Unexpected target manifest'
}
foreach ($filename in @('2017H.hex', '2017H.bin', '2017H.elf')) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $FirmwareDirectory $filename) -Algorithm SHA256).Hash
    if ($actual -ne $manifest.sha256.$filename) {
        throw "Artifact hash mismatch: $filename"
    }
}
$backupDirectory = Join-Path (Split-Path $PSScriptRoot -Parent) ('build\backups\' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $backupDirectory | Out-Null
$connection = @('-c', 'port=SWD', "sn=$ProbeSerial", 'mode=UR', 'reset=HWrst', 'freq=1000')

function Invoke-Programmer {
    param([string[]]$Arguments, [string]$LogName)
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $output = & $programmer @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousPreference
    }
    $output | Out-File -LiteralPath (Join-Path $backupDirectory $LogName) -Encoding UTF8
    $text = ($output | Out-String).Trim()
    Write-Host $text
    if ($exitCode -ne 0) {
        throw "CubeProgrammer failed ($exitCode); see $backupDirectory\$LogName"
    }
    return $text
}

$identity = Invoke-Programmer ($connection + @('-r32', '0x5C001000', '4')) 'device-id.log'
$flashSize = Invoke-Programmer ($connection + @('-r32', '0x1FF1E880', '4')) 'flash-size.log'
if ($identity -notmatch '(?im)^\s*0x5C001000\s*:\s*(?:0x)?([0-9a-f]{8})\b') {
    throw 'Cannot parse DBGMCU IDCODE; no firmware was written.'
}
$deviceId = [Convert]::ToUInt32($Matches[1], 16) -band 0xFFF
if ($deviceId -ne 0x450) {
    throw 'Unexpected device family; expected STM32H743/753 family ID 0x450.'
}
if ($flashSize -notmatch '(?im)^\s*0x1FF1E880\s*:\s*(?:0x)?([0-9a-f]{8})\b') {
    throw 'Cannot parse Flash size; no firmware was written.'
}
$flashKiB = [Convert]::ToUInt32($Matches[1], 16) -band 0xFFFF
if ($flashKiB -ne 1024) {
    throw "Expected the teammate 1 MiB part, got $flashKiB KiB. No firmware was written."
}
$backupBanks = @(
    @{ Address = '0x08000000'; File = 'bank1-before.bin' },
    @{ Address = '0x08100000'; File = 'bank2-before.bin' }
)
foreach ($bank in $backupBanks) {
    $backup = Join-Path $backupDirectory $bank.File
    Invoke-Programmer ($connection + @('-u', $bank.Address, '0x80000', $backup)) ($bank.File + '.log') | Out-Null
    if (-not (Test-Path -LiteralPath $backup) -or (Get-Item -LiteralPath $backup).Length -ne 0x80000) {
        throw 'Backup incomplete; programming cancelled.'
    }
}
Get-ChildItem -LiteralPath $backupDirectory -Filter '*.bin' | Get-FileHash -Algorithm SHA256 | Format-List | Out-File (Join-Path $backupDirectory 'backup-sha256.txt') -Encoding UTF8
$firmware = Join-Path $FirmwareDirectory '2017H.hex'
Invoke-Programmer ($connection + @('-w', $firmware, '-v', '-rst')) 'program-and-verify.log' | Out-Null
Write-Output "PROGRAM AND VERIFY OK. Original Flash banks saved in $backupDirectory"
