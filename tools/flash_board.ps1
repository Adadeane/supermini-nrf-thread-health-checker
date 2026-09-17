<#
.SYNOPSIS
    Flashes the SuperMini nRF52840 with the Thread Network Health Checker firmware.
.DESCRIPTION
    Monitors for the NICENANO / NRF52BOOT drive and copies the UF2 binary as soon as it appears.
#>

$binDir = "$PSScriptRoot\..\bin"
$uf2Files = Get-ChildItem -Path $binDir -Filter "supermini_openthread_cli*.uf2"

if (-not $uf2Files) {
    Write-Host "No UF2 binary found in $binDir. Please build or download the artifact first." -ForegroundColor Red
    exit 1
}

$targetUf2 = $uf2Files[0].FullName
Write-Host "Firmware file ready: $($targetUf2)" -ForegroundColor Cyan
Write-Host "Double-tap the RESET button on your SuperMini nRF52840 now..." -ForegroundColor Yellow

$flashed = $false
while (-not $flashed) {
    $drives = Get-PSDrive -PSProvider FileSystem | Where-Object { Test-Path "$($_.Root)INFO_UF2.TXT" }
    if ($drives) {
        $driveRoot = $drives[0].Root
        Write-Host "Detected bootloader drive at $driveRoot!" -ForegroundColor Green
        Write-Host "Copying UF2 firmware to $driveRoot..." -ForegroundColor Cyan
        cmd.exe /c "copy /b `"$targetUf2`" `"$driveRoot\firmware.uf2`""
        Write-Host "`nFlash complete! The bootloader is writing to flash and rebooting." -ForegroundColor Green
        $flashed = $true
        break
    }
    Start-Sleep -Milliseconds 500
}
