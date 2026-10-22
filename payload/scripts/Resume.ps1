param([ValidateSet('Auto','Bluetooth','Input')][string]$Mode='Auto')
. "$PSScriptRoot\Common.ps1"
Require-Admin
if (-not (Get-Target).Supported) { throw '非受支持的 Intel T2 MacBook / Windows 10。' }
$mutex = New-Object Threading.Mutex($false,'Global\IntelMacT2Patch-Resume')
$locked = $false
try {
    $locked=$mutex.WaitOne(0); if(-not $locked){exit 0}
    $stamp=Join-Path $script:DataRoot 'resume.stamp'
    if($Mode -eq 'Auto') {
        if((Test-Path $stamp) -and ((Get-Date)-(Get-Item $stamp).LastWriteTime).TotalSeconds -lt 60){exit 0}
        Set-Content -LiteralPath $stamp -Value (Get-Date -Format o)
        Start-Sleep -Seconds 8
    }
    $devices=@(Get-CimInstance Win32_PnPEntity)
    $failures=0
    function Reset-One($Device) {
        $id=$Device.PNPDeviceID
        $p=Get-PnpDevice -InstanceId $id
        $code=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
        if($code -eq 22){ Log "SKIP intentionally disabled: $id"; return }
        Log "Reset begin: $id status=$($p.Status)"
        # PnP restart succeeds on the tested ACPI UART device where Disable/Enable
        # returns NotSupported. No disable operation is left pending on failure.
        # Only this selected node is restarted, never a USB host controller.
        $result=Run-Native "$env:SystemRoot\System32\pnputil.exe" @('/restart-device',$id)
        Log ("PnP restart: "+$result.Trim())
        Start-Sleep -Seconds 2
        $after=Get-PnpDevice -InstanceId $id
        Log "Reset end: $id status=$($after.Status); connection/function still requires user test"
        if($after.Status -ne 'OK'){throw "设备仍异常：$id"}
    }
    if($Mode -ne 'Input') {
        # Detect by exact hardware family + actual bound driver, never a customer's instance suffix.
        $bt=@($devices | Where-Object { $_.PNPDeviceID -match '^ACPI\\BCM2E7C\\' -and $_.Service -match '^BtwSerialBus(X)?$' })
        if($bt.Count -eq 1) {
            try { Reset-One $bt[0]; $svc=Get-Service bthserv; if($svc.Status -ne 'Running'){Start-Service bthserv}; Log 'Bluetooth reset completed; pairing was preserved.' }
            catch { $failures++; Log "Bluetooth ERROR: $_" }
        } else { Log "Bluetooth skipped: compatible UART transport count=$($bt.Count)"; if($Mode -eq 'Bluetooth'){$failures++} }
    }
    if($Mode -ne 'Bluetooth') {
        $input=@($devices | Where-Object {
            ($_.PNPDeviceID -match '^USB\\VID_05AC&PID_8302\\' -or $_.PNPDeviceID -match '^USB\\VID_05AC&PID_0340&MI_01\\') -and
            ($Mode -eq 'Input' -or ($_.ConfigManagerErrorCode -ne 0 -and $_.ConfigManagerErrorCode -ne 22))
        })
        foreach($d in $input) { try{Reset-One $d}catch{$failures++;Log "Input ERROR: $_"} }
        if($Mode -eq 'Input' -and $input.Count -eq 0){$failures++;Log 'No supported input device found; nothing was restarted.'}
        Log "Input recovery candidates=$($input.Count). OK status cannot detect every frozen keyboard/Touch Bar."
    }
    if($failures){throw "$failures 项恢复失败，请查看 patch.log。"}
} finally {if($locked){$mutex.ReleaseMutex()};$mutex.Dispose()}
