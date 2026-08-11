<#
    tools/vbox/run_vbox.ps1 — item 49: validate the bootable ISO on VirtualBox.

    WHY THIS RUNS ON WINDOWS AND NOT IN THE GATE HARNESS

    Every other gate runs under QEMU inside WSL. This one cannot. VirtualBox is
    a type-2 hypervisor that needs VT-x directly, and WSL2 is itself a Hyper-V
    guest; nested VT-x is not available to it. So the split is real and forced:

        WSL      builds the ISO   (make iso)
        Windows  boots the ISO    (this script)

    That is an architectural constraint, not a workaround, and it is written
    down rather than hidden — see DDR-906.

    WHAT IT PROVES

    The same NEXUS KERNEL OK sentinel every other boot path asserts, captured
    off COM1, from a real second-source firmware. VirtualBox is not SeaBIOS and
    not OVMF, so a pass here is independent evidence that the boot chain does
    not depend on one firmware's quirks — which is exactly the class of bug
    DDR-905 found on the ISO's BIOS arm.

    USAGE
        pwsh -File tools/vbox/run_vbox.ps1 -Iso <path> [-Firmware bios|efi]

    Exit code 0 only if the sentinel appeared. Anything else is a failure, and
    the captured serial log is printed so the failure is diagnosable.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [ValidateSet('bios', 'efi')][string]$Firmware = 'bios',
    [int]$TimeoutSec = 120,
    [string]$Sentinel = 'NEXUS KERNEL OK',
    [switch]$KeepVM
)

$ErrorActionPreference = 'Stop'

$VBoxManage = 'C:\Program Files\Oracle\VirtualBox\VBoxManage.exe'
if (-not (Test-Path $VBoxManage)) {
    throw "VBoxManage not found at $VBoxManage — install VirtualBox or fix the path."
}

if (-not (Test-Path $Iso)) { throw "ISO not found: $Iso" }

# Copy the ISO to a local Windows path. VirtualBox attaches media by path, and
# a \\wsl$\... UNC path is served by a 9p redirector that VBoxSVC cannot always
# open. Copying is cheap next to a boot and removes a whole class of "works on
# my shell, fails in the service" problems.
$work = Join-Path $env:TEMP ("pradyos-vbox-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Path $work | Out-Null
$localIso = Join-Path $work 'pradyos.iso'
Copy-Item -LiteralPath $Iso -Destination $localIso
$serialLog = Join-Path $work 'serial.log'

$vm = "pradyos-gate-$Firmware-" + [System.Guid]::NewGuid().ToString('N').Substring(0, 8)
$created = $false

function VBox {
    # VBoxManage reports failure via exit code, not stderr text. Check it, or a
    # broken VM silently boots nothing and the sentinel check blames the kernel.
    $out = & $VBoxManage @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "VBoxManage $($args -join ' ') failed ($LASTEXITCODE):`n$out"
    }
    return $out
}

try {
    VBox createvm --name $vm --ostype Other_64 --register --basefolder $work | Out-Null
    $created = $true

    VBox modifyvm $vm --memory 512 --cpus 1 --audio-driver none --usb off | Out-Null
    VBox modifyvm $vm --firmware $(if ($Firmware -eq 'efi') { 'efi64' } else { 'bios' }) | Out-Null

    # COM1 at the standard 0x3F8/IRQ4 the kernel's serial driver expects,
    # redirected to a file. This is the whole point of the run.
    VBox modifyvm $vm --uart1 0x3F8 4 --uartmode1 file $serialLog | Out-Null

    VBox storagectl $vm --name IDE --add ide | Out-Null
    VBox storageattach $vm --storagectl IDE --port 0 --device 0 `
        --type dvddrive --medium $localIso | Out-Null
    VBox modifyvm $vm --boot1 dvd --boot2 none --boot3 none --boot4 none | Out-Null

    Write-Host "[vbox] $vm firmware=$Firmware iso=$Iso"
    VBox startvm $vm --type headless | Out-Null

    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    $found = $false
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $serialLog) {
            # Open shared — VirtualBox holds the file open for writing.
            $fs = [System.IO.File]::Open($serialLog, 'Open', 'Read', 'ReadWrite')
            try { $text = (New-Object System.IO.StreamReader($fs)).ReadToEnd() }
            finally { $fs.Dispose() }
            if ($text -match [regex]::Escape($Sentinel)) { $found = $true; break }
        }
        Start-Sleep -Milliseconds 500
    }

    & $VBoxManage controlvm $vm poweroff 2>&1 | Out-Null
    Start-Sleep -Seconds 2

    $log = if (Test-Path $serialLog) { Get-Content -Raw $serialLog } else { '' }
    Write-Host '----- serial -----'
    Write-Host $log
    Write-Host '------------------'

    if ($found) {
        Write-Host "[vbox] PASS — '$Sentinel' on $Firmware"
        exit 0
    }
    Write-Host "[vbox] FAIL — '$Sentinel' not seen within ${TimeoutSec}s on $Firmware"
    exit 1
}
finally {
    if ($created -and -not $KeepVM) {
        & $VBoxManage unregistervm $vm --delete 2>&1 | Out-Null
    }
    if (-not $KeepVM) {
        Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
    }
}
