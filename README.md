# MacBook Windows 兼容修复助手

**by lrylnx** · [下载 EXE / Releases](https://github.com/lrylnx/IntelMac-T2-Windows-Fix/releases)

首次公开发行版本：v0.3.1-rc1（2026-10-22）

作者与发行维护：**lrylnx** · 联系邮箱：**lryln@foxmail.com**

这是单文件中文 GUI 版本。给客户只需发送 `lrylnx-MacBook兼容修复助手-v0.3.1-rc1.exe`，不需要 CMD、ZIP 解压或附带文件夹。

程序针对 Intel T2 MacBook + Windows 10 x64 build 19041–19045。底层沿用 0.2 候选版的摄像头、蓝牙和睡眠补丁；本机已经安装旧候选补丁时，GUI 可直接检测和管理，无需重装。

## 客户使用

1. 双击 EXE，在 Windows 提示中授予管理员权限。
2. 完全退出微信、OBS、Zoom、Teams 等视频软件，点击“一键安装修复”。
3. 重开微信，在摄像头中选择 **Virtual Camera**。
4. 需要撤销时，打开“常用工具”，点击“卸载并回滚”。

“已安装”表示补丁文件、摄像头注册与唤醒任务存在，不代表每一种故障已经通过实机验证。摄像头出帧检测由用户主动点击才会临时开启摄像头，不保存照片。

## 界面功能

- 总览与修复：识别机型、系统与现有补丁状态；安装前验证文件；安装失败自动尝试回滚。
- 常用工具：摄像头出帧检查、蓝牙恢复、键盘/Touch Bar 恢复、卸载回滚。
- 关机与耗电：查看快速启动、可回滚地关闭快速启动、采集开关机记录、经用户确认后请求完整关机。
- 执行记录：显示实际执行结果与失败原因。
- 导出诊断：生成本地 ZIP，由用户选择保存位置；不会自动上传。

已有摄像头、蓝牙、睡眠候选补丁的实测结论沿用上一版：MacBookPro16,1 原生 720p/30fps 采集有效，用户确认微信画质和视野改善，并确认一次短时间合盖唤醒后三项正常。长期耗电、重复唤醒及其他机型仍需验收。

## 关于关机后耗电和自动开机

本机快速启动原本已经关闭。新选项用于有相关问题的客户机器排查，不能据此认定所有个例都有同一个原因。

先区分混合关机、关机失败/崩溃后重启、固件或电源触发启动、外设与电池硬件。完整关机测试使用 Microsoft 文档所述的 `shutdown /s /t 0`，不添加强制关闭应用参数。只有用户在界面明确确认后才请求关机。

若完整关机、断开扩展坞/USB 外设后仍大量掉电，可再做 Windows/macOS 对照并按 Apple 说明排查 T2 SMC、电池与电源硬件。Windows 补丁不能保证消除真正关机后的固件或硬件耗电，也不能承诺全部个例“杜绝”。

参考：[Microsoft：完整关机](https://learn.microsoft.com/en-us/troubleshoot/windows-client/setup-upgrade-and-drivers/fast-startup-causes-system-hibernation-shutdown-fail)、[Apple：重置 SMC](https://support.apple.com/en-us/102605)。Boot Camp“断电后自动重新启动”的官方设置针对没有内置电池的电脑，不应直接套用到 MacBook，见 [Apple 文档](https://support.apple.com/en-ca/guide/bootcamp-control-panel/bcmpb4cd9725/mac)。

## 运行与数据位置

- GUI 资源缓存在 `%ProgramData%\IntelMacT2PatchManager`，目录受管理员权限保护；客户无需打开它。
- 已安装补丁位于 `%ProgramFiles%\IntelMacT2Patch`，其状态、备份位于 `%ProgramData%\IntelMacT2Patch`。
- 快速启动选项单独备份在 `%ProgramData%\IntelMacT2Shutdown`。通过 GUI 卸载基础补丁时也会尝试恢复由 GUI 修改的快速启动设置。
- 程序可离线运行，客户不需要 Python、Zig 或 Visual Studio；需要系统自带的 64 位 PowerShell 和 .NET Framework。构建与启动在当前 .NET Framework 4.8 上验证。

本版尚未做发布者数字签名，Windows 可能显示“未知发布者”。正式商业分发应使用有效代码签名证书签名；本包不会修改 Windows 安全设置或绕过系统拦截。

## 维护者构建

维护源码包包含 C# GUI、程序清单、图标生成器、内置脚本/二进制、资源打包脚本和摄像头源码。`prepare.py` 仅用于维护者构建，客户运行不需要它。

```powershell
python prepare.py
powershell -NoProfile -ExecutionPolicy Bypass -File Build.ps1
```

构建使用 Windows 自带 .NET Framework 64 位 `csc.exe`。重新编译摄像头 DLL 后应替换 payload/bin 中的产物，再完整运行资源打包和 GUI 构建，不能只替换发布 EXE 的一部分资源。

作者与来源声明见 NOTICE.md；许可见 LICENSE。仓库：https://github.com/lrylnx/IntelMac-T2-Windows-Fix
