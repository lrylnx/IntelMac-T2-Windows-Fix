Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
$script:Product = 'IntelMacT2Patch'
$script:InstallRoot = Join-Path $env:ProgramFiles $script:Product
$script:DataRoot = Join-Path $env:ProgramData $script:Product
$script:StatePath = Join-Path $script:DataRoot 'state.json'
$script:TaskName = 'IntelMacT2Patch-Resume'
$script:CamKey = 'HKLM:\SOFTWARE\Classes\CLSID\{7F3C1A20-9D54-4C1B-A37E-518B2D449F01}\InprocServer32'
function Require-Admin {
    if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw '请以管理员身份运行。' }
    if (-not [Environment]::Is64BitProcess) { throw '请使用 64 位 PowerShell。' }
}
function Get-Target {
    $c = Get-CimInstance Win32_ComputerSystem
    $o = Get-CimInstance Win32_OperatingSystem
    $cpu = @(Get-CimInstance Win32_Processor)[0]
    $models = @('MacBookPro15,1','MacBookPro15,2','MacBookPro15,3','MacBookPro15,4','MacBookPro16,1','MacBookPro16,2','MacBookPro16,3','MacBookPro16,4','MacBookAir8,1','MacBookAir8,2','MacBookAir9,1')
    [pscustomobject]@{ Model=$c.Model; OS=$o.Caption; Build=[int]$o.BuildNumber; Supported=($c.Manufacturer -match 'Apple' -and $cpu.Manufacturer -eq 'GenuineIntel' -and $c.Model -in $models -and [Environment]::Is64BitOperatingSystem -and [int]$o.BuildNumber -ge 19041 -and [int]$o.BuildNumber -lt 22000) }
}
function Run-Native([string]$Exe, [string[]]$Arguments) {
    if($Exe -eq 'powercfg.exe'){$Exe=Join-Path $env:SystemRoot 'System32\powercfg.exe'}
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$Exe
    $quoted=@(foreach($a in $Arguments){
        $q=[regex]::Replace([string]$a,'(\\*)"','$1$1\"')
        $q=[regex]::Replace($q,'(\\+)$','$1$1')
        '"'+$q+'"'
    })
    $info.Arguments=$quoted -join ' '
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $p=New-Object Diagnostics.Process;$p.StartInfo=$info
    try {
        if(-not $p.Start()){throw "无法启动 $Exe"}
        $stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
        if(-not $p.WaitForExit(30000)){$p.Kill();$p.WaitForExit();throw "$Exe 超时（30秒），已停止本次命令。"}
        $out=$stdout.Result;$err=$stderr.Result
        if($p.ExitCode -ne 0){throw "$Exe failed ($($p.ExitCode)): $out $err"}
        return $out
    } finally {$p.Dispose()}
}
function Active-Plan {
    $s = Run-Native "$env:SystemRoot\System32\powercfg.exe" @('/getactivescheme')
    $m = [regex]::Match($s,'[a-fA-F0-9]{8}-(?:[a-fA-F0-9]{4}-){3}[a-fA-F0-9]{12}')
    if (-not $m.Success) { throw '无法读取当前电源方案。' }
    return $m.Value
}
function Secure-Folder([string]$Path) {
    if (Test-Path -LiteralPath $Path) { throw "目录已存在，拒绝覆盖：$Path" }
    New-Item -ItemType Directory -Path $Path | Out-Null
    Run-Native "$env:SystemRoot\System32\icacls.exe" @($Path,'/inheritance:r','/grant:r','*S-1-5-18:(OI)(CI)F','*S-1-5-32-544:(OI)(CI)F','*S-1-5-32-545:(OI)(CI)RX') | Out-Null
}
function Save-State($s) {
    $tmp = "$script:StatePath.tmp"
    $s | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $tmp -Encoding UTF8
    Move-Item -LiteralPath $tmp -Destination $script:StatePath -Force
}
function Read-State { Get-Content -LiteralPath $script:StatePath -Raw -Encoding UTF8 | ConvertFrom-Json }
function Log([string]$Message) {
    $path = Join-Path $script:DataRoot 'patch.log'
    if ((Test-Path $path) -and (Get-Item $path).Length -gt 2MB) { Move-Item -LiteralPath $path -Destination "$path.old" -Force }
    Add-Content -LiteralPath $path -Value "$(Get-Date -Format o) $Message" -Encoding UTF8
}
function Assert-NoCameraApps {
    $apps = @(Get-Process -Name Weixin,WeChat,obs64,obs32,Zoom,Teams,ms-teams -ErrorAction SilentlyContinue)
    if ($apps.Count) { throw '请先完全退出微信、OBS、Zoom、Teams（包括托盘），再重新运行。不会强制关闭你的程序。' }
}
function Camera-Registration([string]$Dll, [switch]$Remove) {
    $args = @('/s'); if($Remove){$args += '/u'}; $args += ('"'+$Dll+'"')
    $p = Start-Process -FilePath "$env:SystemRoot\System32\regsvr32.exe" -ArgumentList $args -WindowStyle Hidden -PassThru
    if(-not $p.WaitForExit(30000)){ throw '摄像头注册超时，请检查 regsvr32 进程后重试。' }
    if($p.ExitCode -ne 0){ throw "摄像头注册失败：$($p.ExitCode)" }
}
