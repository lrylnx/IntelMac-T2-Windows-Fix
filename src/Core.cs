// Copyright (c) 2026 lrylnx. Contact: lryln@foxmail.com. See LICENSE and NOTICE.md.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Principal;
using System.Text;
using System.Web.Script.Serialization;

[assembly: AssemblyTitle("MacBook Windows 兼容修复助手")]
[assembly: AssemblyDescription("Intel T2 MacBook Windows 10 compatibility tools")]
[assembly: AssemblyProduct("Intel Mac Compatibility Assistant")]
[assembly: AssemblyCompany("lrylnx")]
[assembly: AssemblyCopyright("Copyright © 2026 lrylnx")]
[assembly: AssemblyVersion("0.3.1.0")]
[assembly: AssemblyFileVersion("0.3.1.0")]

internal sealed class CommandResult {
    public int Code;
    public string Output;
    public string Error;
}

internal static class Core {
    public static readonly string ManagerRoot=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),"IntelMacT2PatchManager");
    public static readonly string InstallRoot=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"IntelMacT2Patch");
    public static string PackageRoot;
    public static readonly JavaScriptSerializer Json=new JavaScriptSerializer();
    public static string Hash(byte[] data) {using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(data)).Replace("-","");}
    public static string FileHash(string path) {using(var sha=SHA256.Create())using(var s=File.OpenRead(path))return BitConverter.ToString(sha.ComputeHash(s)).Replace("-","");}
    private static void CheckDirectory(string path) {
        if(Directory.Exists(path) && (File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)throw new IOException("程序目录包含重定向链接，请联系维护者检查："+path);
    }
    public static void SecureDirectory(string path) {
        CheckDirectory(path);Directory.CreateDirectory(path);
        var acl=new DirectorySecurity();acl.SetAccessRuleProtection(true,false);
        foreach(var sid in new[]{"S-1-5-18","S-1-5-32-544"})acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier(sid),FileSystemRights.FullControl,InheritanceFlags.ContainerInherit|InheritanceFlags.ObjectInherit,PropagationFlags.None,AccessControlType.Allow));
        acl.AddAccessRule(new FileSystemAccessRule(new SecurityIdentifier("S-1-5-32-545"),FileSystemRights.ReadAndExecute,InheritanceFlags.ContainerInherit|InheritanceFlags.ObjectInherit,PropagationFlags.None,AccessControlType.Allow));
        Directory.SetAccessControl(path,acl);
    }
    public static string ExtractPayload(string testRoot) {
        byte[] data;
        using(var s=Assembly.GetExecutingAssembly().GetManifestResourceStream("payload.zip")) {
            if(s==null)throw new IOException("程序内置文件缺失。");
            using(var ms=new MemoryStream()){s.CopyTo(ms);data=ms.ToArray();}
        }
        if(Hash(data)!=BuildInfo.PayloadHash)throw new IOException("程序内置文件校验失败，请重新获取完整安装程序。");
        string parent=testRoot??ManagerRoot;
        SecureDirectory(parent);
        string target=Path.Combine(parent,"payload-"+BuildInfo.PayloadHash.Substring(0,16));
        CheckDirectory(target);SecureDirectory(target);
        string prefix=Path.GetFullPath(target)+Path.DirectorySeparatorChar;
        using(var ms=new MemoryStream(data))using(var z=new ZipArchive(ms,ZipArchiveMode.Read)) {
            foreach(var entry in z.Entries) {
                string path=Path.GetFullPath(Path.Combine(target,entry.FullName.Replace('/',Path.DirectorySeparatorChar)));
                if(!path.StartsWith(prefix,StringComparison.OrdinalIgnoreCase))throw new IOException("内置文件路径无效。");
                string directory=Path.GetDirectoryName(path);
                // Every ancestor is under our protected cache. Reject a pre-existing reparse point.
                for(string d=directory;d!=null && d.StartsWith(target,StringComparison.OrdinalIgnoreCase);d=Path.GetDirectoryName(d))CheckDirectory(d);
                if(entry.FullName.EndsWith("/")){Directory.CreateDirectory(path);continue;}
                Directory.CreateDirectory(directory);
                if(File.Exists(path) && (File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)throw new IOException("缓存文件包含重定向链接。");
                byte[] expected;using(var es=entry.Open())using(var em=new MemoryStream()){es.CopyTo(em);expected=em.ToArray();}
                if(File.Exists(path)) {
                    if(FileHash(path)!=Hash(expected))throw new IOException("程序缓存校验失败，请由维护者处理："+path);
                } else using(var f=new FileStream(path,FileMode.CreateNew,FileAccess.Write,FileShare.None))f.Write(expected,0,expected.Length);
            }
        }
        VerifyPackage(target);PackageRoot=target;return target;
    }
    public static int VerifyPackage(string root) {
        var manifest=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(Path.Combine(root,"manifest.json"),Encoding.UTF8));
        var files=(System.Collections.IEnumerable)manifest["Files"];int count=0;
        foreach(var item in files) {
            var f=(Dictionary<string,object>)item;string relative=(string)f["Path"];
            string path=Path.GetFullPath(Path.Combine(root,relative.Replace('/',Path.DirectorySeparatorChar)));
            if(!path.StartsWith(Path.GetFullPath(root)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("清单路径无效。");
            if(FileHash(path)!=(string)f["SHA256"])throw new IOException("文件校验失败："+relative);count++;
        }
        if(count<8)throw new IOException("程序文件清单不完整。");return count;
    }
    public static CommandResult RunScript(string script,string args,Action<string> onLine) {
        string path=Path.GetFullPath(Path.Combine(PackageRoot,"scripts",script));
        if(!path.StartsWith(Path.GetFullPath(Path.Combine(PackageRoot,"scripts"))+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))throw new IOException("操作路径无效。");
        string command="$ErrorActionPreference='Stop'; $ProgressPreference='SilentlyContinue'; [Console]::OutputEncoding=New-Object System.Text.UTF8Encoding($false); try { & '"+path.Replace("'","''")+"' "+args+" 3>&1 4>&1 5>&1 6>&1 | ForEach-Object { [Console]::WriteLine([string]$_) }; if(-not $?){exit 1} } catch { [Console]::Error.WriteLine($_.Exception.Message); exit 1 }";
        var info=new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"System32","WindowsPowerShell","v1.0","powershell.exe"));
        info.Arguments="-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand "+Convert.ToBase64String(Encoding.Unicode.GetBytes(command));
        info.UseShellExecute=false;info.CreateNoWindow=true;info.RedirectStandardOutput=true;info.RedirectStandardError=true;info.StandardOutputEncoding=Encoding.UTF8;info.StandardErrorEncoding=Encoding.UTF8;
        var text=new StringBuilder();var errors=new StringBuilder();var sync=new object();
        using(var p=new Process()){p.StartInfo=info;
            DataReceivedEventHandler receive=delegate(object sender,DataReceivedEventArgs e){if(e.Data==null)return;lock(sync)text.AppendLine(e.Data);if(onLine!=null)onLine(e.Data);};
            p.OutputDataReceived+=receive;p.ErrorDataReceived+=delegate(object sender,DataReceivedEventArgs e){if(e.Data==null)return;lock(sync)errors.AppendLine(e.Data);if(onLine!=null)onLine(e.Data);};
            p.Start();p.BeginOutputReadLine();p.BeginErrorReadLine();p.WaitForExit();
            return new CommandResult{Code=p.ExitCode,Output=text.ToString(),Error=errors.ToString()};
        }
    }
    public static Dictionary<string,object> Status() {
        var result=RunScript("Status.ps1","",null);
        if(result.Code!=0)throw new IOException((result.Output+result.Error).Trim());
        string data=result.Output.Trim('\uFEFF',' ','\r','\n','\t');
        try{return Json.Deserialize<Dictionary<string,object>>(data);}
        catch(Exception ex){throw new IOException("状态脚本返回格式异常，长度="+data.Length+"，内容="+data.Substring(0,Math.Min(data.Length,400)),ex);}
    }
    public static string Text(Dictionary<string,object> data,string key) {return data!=null && data.ContainsKey(key) && data[key]!=null?Convert.ToString(data[key]):"";}
    public static bool Flag(Dictionary<string,object> data,string key) {return data!=null && data.ContainsKey(key) && data[key] is bool && (bool)data[key];}
    public static void Audit(string line) {
        try{string folder=Path.Combine(ManagerRoot,"logs");Directory.CreateDirectory(folder);File.AppendAllText(Path.Combine(folder,DateTime.Now.ToString("yyyyMMdd-HHmm")+".log"),DateTime.Now.ToString("o")+" "+line+Environment.NewLine,Encoding.UTF8);}catch{}
    }
}
