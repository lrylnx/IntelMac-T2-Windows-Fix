param([string]$OutputPath=(Join-Path $PSScriptRoot 'build\IntelMacCompatibilityAssistant.exe'))
$ErrorActionPreference='Stop'
$csc=Join-Path $env:SystemRoot 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if(-not(Test-Path (Join-Path $PSScriptRoot 'build\BuildInfo.cs'))){throw '先使用 prepare.py 准备资源。'}
& $csc /nologo /target:exe /r:System.Drawing.dll ("/out:"+(Join-Path $PSScriptRoot 'build\CreateIcon.exe')) (Join-Path $PSScriptRoot 'src\CreateIcon.cs')
if($LASTEXITCODE){throw '图标工具编译失败。'}
& (Join-Path $PSScriptRoot 'build\CreateIcon.exe') (Join-Path $PSScriptRoot 'build\app.ico')
& $csc /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 ("/out:"+$OutputPath) ("/win32manifest:"+(Join-Path $PSScriptRoot 'src\app.manifest')) ("/win32icon:"+(Join-Path $PSScriptRoot 'build\app.ico')) ("/resource:"+(Join-Path $PSScriptRoot 'build\payload.zip')+',payload.zip') /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll /r:System.Web.Extensions.dll (Join-Path $PSScriptRoot 'src\Core.cs') (Join-Path $PSScriptRoot 'src\Main.cs') (Join-Path $PSScriptRoot 'build\BuildInfo.cs')
if($LASTEXITCODE){throw 'GUI 程序编译失败。'}
Get-Item -LiteralPath $OutputPath | Select-Object FullName,Length
