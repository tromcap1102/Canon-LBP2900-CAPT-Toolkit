# ============================================================================
# Design by Bruce Nguyen from CCTVWIKI.COM va Claude Code Max
# ============================================================================
# Cai may in Canon LBP2900 qua mang (IPP) tu may chu Linux da chia se.
# Tu dong nang quyen Administrator (Windows yeu cau quyen Admin de them may in).
# Chay truc tiep, hoac double-click file .bat di kem.
# ============================================================================
param(
    [string]$ServerIP = "",
    [string]$PrinterName = "LBP2900",
    [switch]$NoTestPrint
)

$ErrorActionPreference = "Stop"

# --- Tu nang quyen Administrator neu chua co ---
$currentPrincipal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $currentPrincipal.IsInRole([Security.Principal.WindowsBuiltinRole]::Administrator)) {
    Write-Host "Can quyen Administrator de them may in. Dang tu khoi dong lai voi quyen Admin..."
    $argList = @()
    if ($ServerIP) { $argList += "-ServerIP `"$ServerIP`"" }
    $argList += "-PrinterName `"$PrinterName`""
    if ($NoTestPrint) { $argList += "-NoTestPrint" }
    $joined = $argList -join " "
    Start-Process powershell -Verb RunAs -Wait -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" $joined"
    exit
}

Write-Host "===================================================="
Write-Host "  CAI MAY IN CANON LBP2900 QUA MANG (tu may chu Linux)"
Write-Host "===================================================="

if (-not $ServerIP) {
    $inputIP = Read-Host "Nhap dia chi IP may chu Linux (noi cam may in LBP2900) [192.168.1.152]"
    $ServerIP = if ([string]::IsNullOrWhiteSpace($inputIP)) { "192.168.1.152" } else { $inputIP }
}

$ippUrl = "http://${ServerIP}:631/printers/${PrinterName}"
Write-Host ""
Write-Host "Dang kiem tra ket noi toi $ippUrl ..."
try {
    $resp = Invoke-WebRequest -Uri $ippUrl -UseBasicParsing -TimeoutSec 8
    Write-Host "OK - may chu phan hoi (HTTP $($resp.StatusCode))"
} catch {
    Write-Host ""
    Write-Host "LOI: Khong ket noi duoc toi $ippUrl"
    Write-Host "Kiem tra:"
    Write-Host "  - May chu Linux ($ServerIP) co dang BAT khong?"
    Write-Host "  - May chu da chay 'Go va cai lai LBP2900' (co bat chia se LAN) chua?"
    Write-Host "  - May Windows nay va may chu co dang cung mang LAN khong?"
    Read-Host "Nhan Enter de dong"
    exit 1
}

# Xoa may in cung ten neu da ton tai, de cai lai sach (an toan chay lai nhieu lan)
Get-Printer -Name $PrinterName -ErrorAction SilentlyContinue | Remove-Printer -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "Dang them may in '$PrinterName' qua IPP..."
try {
    Add-Printer -Name $PrinterName -IppURL $ippUrl -ErrorAction Stop
    Write-Host "Da them may in thanh cong."
} catch {
    Write-Host ""
    Write-Host "LOI khi them may in: $_"
    Read-Host "Nhan Enter de dong"
    exit 1
}

Write-Host ""
Get-Printer -Name $PrinterName | Format-List Name, DriverName, PortName, PrinterStatus

if (-not $NoTestPrint) {
    Write-Host ""
    $ans = Read-Host "Ban co muon in mot trang thu ngay bay gio khong? (Y/n)"
    if ([string]::IsNullOrWhiteSpace($ans) -or $ans -match '^[Yy]') {
        $tmp = [System.IO.Path]::GetTempFileName()
        $tmp = [System.IO.Path]::ChangeExtension($tmp, ".txt")
        @"
CANON LBP2900 - TRANG IN THU TU WINDOWS
Neu ban doc duoc dong nay, ket noi mang toi may in qua may chu Linux ($ServerIP) da hoat dong!
Thoi gian: $(Get-Date)
"@ | Out-File -FilePath $tmp -Encoding UTF8

        try {
            Get-Content $tmp | Out-Printer -Name $PrinterName
            Write-Host "Da gui lenh in thu toi '$PrinterName'. Kiem tra may in."
        } catch {
            Write-Host "Gui lenh in thu that bai: $_"
        } finally {
            Remove-Item $tmp -ErrorAction SilentlyContinue
        }
    }
}

Write-Host ""
Write-Host "===================================================="
Write-Host "  HOAN TAT"
Write-Host "===================================================="
Write-Host "  Design by Bruce Nguyen from CCTVWIKI.COM va Claude Code Max"
Write-Host "===================================================="
Read-Host "Nhan Enter de dong cua so nay"
