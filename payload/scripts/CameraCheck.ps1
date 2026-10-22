. "$PSScriptRoot\Common.ps1"
if(-not (Get-Target).Supported){throw '不支持此机型。'}
$base=Split-Path $PSScriptRoot -Parent
$result=Run-Native (Join-Path $base 'bin\OutputProbe.exe') @((Join-Path $base 'bin\VirtualCam.dll'),'1280','720','rgb')
Write-Host $result
Write-Host '出帧检查通过。实际微信画质、方向及通话仍需在微信中检查。'
