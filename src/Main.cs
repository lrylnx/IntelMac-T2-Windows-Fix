// Copyright (c) 2026 lrylnx. Contact: lryln@foxmail.com. See LICENSE and NOTICE.md.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

internal static class Program {
    [STAThread] static int Main(string[] args) {
        Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
        try {
            if(args.Length>=2 && args[0]=="--self-test") {
                string report=Path.GetFullPath(args[1]);Directory.CreateDirectory(report);
                File.WriteAllText(Path.Combine(report,"step.txt"),"extract",Encoding.UTF8);
                Core.ExtractPayload(Path.Combine(report,"cache-"+Guid.NewGuid().ToString("N")));
                File.WriteAllText(Path.Combine(report,"step.txt"),"status",Encoding.UTF8);
                var status=Core.Status();
                File.WriteAllText(Path.Combine(report,"self-test.json"),Core.Json.Serialize(new{PayloadHash=BuildInfo.PayloadHash,Files=Core.VerifyPackage(Core.PackageRoot),Status=status}),Encoding.UTF8);
                var check=Core.RunScript("Install.ps1","-CheckOnly",null);if(check.Code!=0)throw new IOException(check.Output+check.Error);
                var power=Core.RunScript("PowerTools.ps1","-Action Status",null);if(power.Code!=0)throw new IOException(power.Output+power.Error);Core.Json.DeserializeObject(power.Output.Trim('\uFEFF',' ','\r','\n','\t'));
                var diagnose=Core.RunScript("PowerTools.ps1","-Action Diagnose",null);if(diagnose.Code!=0 || !diagnose.Output.Contains("REPORT="))throw new IOException(diagnose.Output+diagnose.Error);
                string sample=Path.Combine(Core.PackageRoot,"scripts","Status.ps1");byte[] original=File.ReadAllBytes(sample);bool rejected=false;
                try{File.AppendAllText(sample,"\r\n# integrity-test\r\n",Encoding.UTF8);try{Core.VerifyPackage(Core.PackageRoot);}catch(IOException){rejected=true;}}finally{File.WriteAllBytes(sample,original);}
                if(!rejected)throw new IOException("文件篡改校验测试失败。");Core.VerifyPackage(Core.PackageRoot);
                using(var form=new MainForm()){form.SetTestStatus(status);form.CreateControl();form.Show();Application.DoEvents();form.RenderPages(report);form.Close();}
                return 0;
            }
            bool created;using(var mutex=new Mutex(true,"Global\\IntelMacCompatibilityAssistantUI",out created)) {
                if(!created){MessageBox.Show("修复助手已经打开，请切换到现有窗口。","MacBook 兼容修复助手",MessageBoxButtons.OK,MessageBoxIcon.Information);return 0;}
                Application.Run(new MainForm());
            }
            return 0;
        } catch(Exception ex){if(args.Length>=2 && args[0]=="--self-test"){try{Directory.CreateDirectory(args[1]);File.WriteAllText(Path.Combine(args[1],"error.txt"),ex.ToString(),Encoding.UTF8);}catch{}return 1;}MessageBox.Show("程序未能启动："+ex.Message,"MacBook 兼容修复助手",MessageBoxButtons.OK,MessageBoxIcon.Error);return 1;}
    }
}

internal sealed class Surface:Panel {
    public Surface(){DoubleBuffered=true;BackColor=Color.White;Padding=new Padding(20);}
    protected override void OnPaint(PaintEventArgs e){base.OnPaint(e);using(var pen=new Pen(Color.FromArgb(225,231,237)))e.Graphics.DrawRectangle(pen,0,0,Width-1,Height-1);}
}

internal sealed class MainForm:Form {
    private readonly Color ink=Color.FromArgb(25,42,59),muted=Color.FromArgb(96,113,129),teal=Color.FromArgb(0,126,133),navy=Color.FromArgb(19,34,50);
    private readonly Panel content=new Panel(),rail=new Panel(),footer=new Panel();
    private readonly Label heading=new Label(),subtitle=new Label(),activity=new Label(),machine=new Label(),os=new Label(),badge=new Label();
    private readonly ProgressBar progress=new ProgressBar();
    private readonly RichTextBox logs=new RichTextBox();
    private readonly Dictionary<string,Panel> pages=new Dictionary<string,Panel>();
    private readonly Dictionary<string,Button> nav=new Dictionary<string,Button>();
    private readonly List<Button> actions=new List<Button>();
    private Button install,restore,camera,bt,input,powerApply,powerRestore;
    private readonly Label[] cardStates=new Label[3];
    private Label fastState;
    private bool busy,ready,testMode;
    private string currentPage="home",lastReport="";
    private float uiScale=1f;
    private bool arranging;
    private Dictionary<string,object> state;

    public MainForm(){
        SuspendLayout();
        Text="lrylnx · MacBook Windows 兼容修复助手";StartPosition=FormStartPosition.CenterScreen;ClientSize=new Size(1040,720);MinimumSize=new Size(940,680);
        AutoScaleMode=AutoScaleMode.None;Font=new Font("Microsoft YaHei UI",9.5f);BackColor=Color.FromArgb(244,247,250);ForeColor=ink;
        try{Icon=Icon.ExtractAssociatedIcon(Application.ExecutablePath);}catch{}
        var root=new TableLayoutPanel{Dock=DockStyle.Fill,ColumnCount=2,RowCount=1,Margin=Padding.Empty};root.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute,208));root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,100));Controls.Add(root);
        rail.Dock=DockStyle.Fill;rail.BackColor=navy;root.Controls.Add(rail,0,0);
        var body=new Panel{Dock=DockStyle.Fill};root.Controls.Add(body,1,0);
        var header=new Panel{Dock=DockStyle.Top,Height=100,Padding=new Padding(30,22,30,10),BackColor=Color.White};
        heading.SetBounds(30,18,720,35);heading.Font=new Font(Font.FontFamily,20,FontStyle.Bold);heading.ForeColor=ink;heading.Anchor=AnchorStyles.Left|AnchorStyles.Top|AnchorStyles.Right;
        subtitle.SetBounds(32,59,720,24);subtitle.ForeColor=muted;subtitle.Anchor=AnchorStyles.Left|AnchorStyles.Top|AnchorStyles.Right;
        header.Controls.AddRange(new Control[]{heading,subtitle});
        footer.Dock=DockStyle.Bottom;footer.Height=78;footer.BackColor=Color.White;activity.SetBounds(30,13,700,25);activity.Anchor=AnchorStyles.Left|AnchorStyles.Right|AnchorStyles.Top;activity.ForeColor=muted;activity.Text="正在读取机器信息…";
        progress.SetBounds(30,47,700,5);progress.Anchor=AnchorStyles.Left|AnchorStyles.Top|AnchorStyles.Right;progress.Style=ProgressBarStyle.Marquee;progress.MarqueeAnimationSpeed=25;footer.Controls.AddRange(new Control[]{activity,progress});
        content.Dock=DockStyle.Fill;content.Padding=new Padding(28,22,28,18);body.Controls.Add(content);body.Controls.Add(footer);body.Controls.Add(header);
        var logo=new Label{Text="M",BackColor=teal,ForeColor=Color.White,TextAlign=ContentAlignment.MiddleCenter,Font=new Font("Segoe UI",24,FontStyle.Bold)};logo.SetBounds(24,25,48,48);rail.Controls.Add(logo);
        AddLabel(rail,"MacBook",84,25,110,24,14,Color.White,true);AddLabel(rail,"兼容修复助手",84,51,115,23,10,Color.FromArgb(165,186,202),false);
        AddLabel(rail,"0.3.1 · lrylnx",26,95,165,24,9,Color.FromArgb(135,166,185),false);
        string[] keys={"home","tools","power","logs","about"};string[] titles={"总览与修复","常用工具","关机与耗电","执行记录","使用帮助"};
        for(int i=0;i<keys.Length;i++){string key=keys[i];var b=new Button{Text="  "+titles[i],FlatStyle=FlatStyle.Flat,TextAlign=ContentAlignment.MiddleLeft,ForeColor=Color.FromArgb(188,206,219),BackColor=navy,Cursor=Cursors.Hand,Font=new Font(Font.FontFamily,11)};b.FlatAppearance.BorderSize=0;b.SetBounds(12,149+i*53,184,45);b.Click+=delegate{Navigate(key);};rail.Controls.Add(b);nav[key]=b;}
        var railNote=new Label{Text="作者与发行：lrylnx\nlryln@foxmail.com\n\nIntel T2 · Windows 10",ForeColor=Color.FromArgb(133,158,177),Font=new Font(Font.FontFamily,9),AutoSize=false};railNote.SetBounds(25,Height-180,170,120);railNote.Anchor=AnchorStyles.Left|AnchorStyles.Bottom;rail.Controls.Add(railNote);
        BuildHome();BuildTools();BuildPower();BuildLogs();BuildHelp();Navigate("home");
        Shown+=async delegate{ResponsiveLayout();if(!testMode)await Initialize();};
        FormClosing+=delegate(object sender,FormClosingEventArgs e){if(busy){e.Cancel=true;MessageBox.Show(this,"正在执行操作，请等待完成后再关闭，以免中断备份或回滚。",Text,MessageBoxButtons.OK,MessageBoxIcon.Information);}};
        ResumeLayout(true);
        float dpiScale;using(var g=CreateGraphics())dpiScale=g.DpiX/96f;uiScale=dpiScale;
        if(dpiScale>1.01f)ScaleUi(dpiScale);
        Rectangle area=Screen.FromControl(this).WorkingArea;
        if(Width>area.Width-24 || Height>area.Height-24){MinimumSize=new Size(Math.Min(MinimumSize.Width,area.Width-24),Math.Min(MinimumSize.Height,area.Height-24));Size=new Size(Math.Min(Width,area.Width-24),Math.Min(Height,area.Height-24));}
        content.SizeChanged+=delegate{ResponsiveLayout();};ResponsiveLayout();
    }
    private void ResponsiveLayout(){
        if(arranging || pages.Count==0)return;arranging=true;
        try {
            foreach(var pair in pages){var page=pair.Value;int width=page.ClientSize.Width;if(width<100)continue;foreach(Control child in page.Controls){var surface=child as Surface;if(surface!=null){surface.Width=width;foreach(Control sub in surface.Controls){var b=sub as Button;if(b!=null && (b.Anchor&AnchorStyles.Right)!=0)b.Left=width-b.Width-(int)(23*uiScale);}}}}
            var home=pages["home"];var cards=home.Controls.OfType<TableLayoutPanel>().FirstOrDefault();
            if(cards!=null){cards.Width=home.ClientSize.Width;cards.ColumnStyles.Clear();for(int i=0;i<3;i++)cards.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,33.333f));cards.PerformLayout();}
            if(badge.Parent!=null)badge.Left=badge.Parent.ClientSize.Width-badge.Width-(int)(22*uiScale);
            progress.Width=footer.ClientSize.Width-(int)(60*uiScale);
        } finally {arranging=false;}
    }
    private sealed class LayoutItem {public Control Control;public Rectangle Bounds;public DockStyle Dock;public AnchorStyles Anchor;public Padding Padding;public Padding Margin;}
    private static Padding Scaled(Padding p,float f){return new Padding((int)Math.Round(p.Left*f),(int)Math.Round(p.Top*f),(int)Math.Round(p.Right*f),(int)Math.Round(p.Bottom*f));}
    private void ScaleUi(float factor){
        var items=new List<LayoutItem>();
        Action<Control> collect=null;collect=delegate(Control parent){foreach(Control c in parent.Controls){items.Add(new LayoutItem{Control=c,Bounds=c.Bounds,Dock=c.Dock,Anchor=c.Anchor,Padding=c.Padding,Margin=c.Margin});collect(c);}};collect(this);
        SuspendLayout();foreach(var i in items){i.Control.SuspendLayout();i.Control.Dock=DockStyle.None;i.Control.Anchor=AnchorStyles.Top|AnchorStyles.Left;}
        ClientSize=new Size((int)Math.Round(ClientSize.Width*factor),(int)Math.Round(ClientSize.Height*factor));MinimumSize=new Size((int)Math.Round(MinimumSize.Width*factor),(int)Math.Round(MinimumSize.Height*factor));
        foreach(var i in items){var r=i.Bounds;i.Control.Bounds=new Rectangle((int)Math.Round(r.X*factor),(int)Math.Round(r.Y*factor),(int)Math.Round(r.Width*factor),(int)Math.Round(r.Height*factor));i.Control.Padding=Scaled(i.Padding,factor);i.Control.Margin=Scaled(i.Margin,factor);var table=i.Control as TableLayoutPanel;if(table!=null){foreach(ColumnStyle col in table.ColumnStyles)if(col.SizeType==SizeType.Absolute)col.Width*=factor;foreach(RowStyle row in table.RowStyles)if(row.SizeType==SizeType.Absolute)row.Height*=factor;}}
        foreach(var i in items){i.Control.Dock=i.Dock;i.Control.Anchor=i.Anchor;}
        for(int i=items.Count-1;i>=0;i--)items[i].Control.ResumeLayout(true);ResumeLayout(true);
    }
    private Label AddLabel(Control parent,string text,int x,int y,int w,int h,float size,Color color,bool bold){var l=new Label{Text=text,ForeColor=color,Font=new Font(Font.FontFamily,size,bold?FontStyle.Bold:FontStyle.Regular),AutoSize=false};l.SetBounds(x,y,w,h);parent.Controls.Add(l);return l;}
    private Button ButtonAt(Control parent,string text,int x,int y,int w,bool primary,Action click){var b=new Button{Text=text,FlatStyle=FlatStyle.Flat,BackColor=primary?teal:Color.White,ForeColor=primary?Color.White:ink,Cursor=Cursors.Hand,Font=new Font(Font.FontFamily,10,FontStyle.Bold)};b.FlatAppearance.BorderColor=primary?teal:Color.FromArgb(209,220,229);b.SetBounds(x,y,w,43);b.Click+=delegate{click();};parent.Controls.Add(b);actions.Add(b);return b;}
    private Panel Page(string key){var p=new Panel{Dock=DockStyle.Fill,AutoScroll=true,BackColor=BackColor};pages[key]=p;content.Controls.Add(p);return p;}
    private void BuildHome(){
        var p=Page("home");var info=new Surface{Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right};info.SetBounds(0,0,768,116);p.Controls.Add(info);
        machine.SetBounds(22,20,520,28);machine.Font=new Font(Font.FontFamily,14,FontStyle.Bold);machine.Text="正在识别机型";info.Controls.Add(machine);
        os.SetBounds(23,59,590,24);os.ForeColor=muted;os.Text="读取系统和补丁状态";info.Controls.Add(os);
        badge.SetBounds(617,25,125,32);badge.Anchor=AnchorStyles.Top|AnchorStyles.Right;badge.Text="检测中";badge.TextAlign=ContentAlignment.MiddleCenter;badge.BackColor=Color.FromArgb(232,245,244);badge.ForeColor=teal;info.Controls.Add(badge);
        var cards=new TableLayoutPanel{ColumnCount=3,RowCount=1,Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right,Margin=Padding.Empty};cards.SetBounds(0,134,768,153);p.Controls.Add(cards);
        string[] names={"摄像头","蓝牙","睡眠与唤醒"};string[] descriptions={"原生 720p 优先采集\n兼容微信虚拟摄像头","唤醒后自动恢复\n支持的 Broadcom 设备","保留合盖睡眠\n优化唤醒与设备恢复"};
        for(int i=0;i<3;i++){cards.ColumnStyles.Add(new ColumnStyle(SizeType.Percent,33.333f));var card=new Surface{Dock=DockStyle.Fill,Margin=new Padding(i==0?0:7,0,i==2?0:7,0)};cards.Controls.Add(card,i,0);AddLabel(card,names[i],18,16,210,28,13,ink,true);AddLabel(card,descriptions[i],18,51,220,49,9.5f,muted,false);cardStates[i]=AddLabel(card,"等待检测",18,113,205,23,9,teal,true);}
        install=ButtonAt(p,"一键安装修复",0,309,218,true,Install);ButtonAt(p,"重新检测",233,309,135,false,RefreshState);ButtonAt(p,"导出诊断",383,309,135,false,ExportDiagnostic);
        AddLabel(p,"安装前请完全退出微信、OBS 等视频软件。\n完成后重开微信，在摄像头中选择 Virtual Camera。",2,369,730,52,9.5f,muted,false);
        var note=AddLabel(p,"已在 MacBookPro16,1 完成摄像头及一次短时间唤醒验证。\n长时间睡眠耗电和其他机型需要继续实机验收。",2,436,730,48,9,muted,false);note.Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right;
    }
    private void ToolRow(Control p,int y,string title,string description,string action,Action run,out Button button){var card=new Surface{Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right};card.SetBounds(0,y,768,101);p.Controls.Add(card);AddLabel(card,title,20,15,510,28,12,ink,true);AddLabel(card,description,21,49,530,35,9.5f,muted,false);button=ButtonAt(card,action,596,28,149,false,run);button.Anchor=AnchorStyles.Top|AnchorStyles.Right;}
    private void BuildTools(){var p=Page("tools");ToolRow(p,0,"摄像头出帧检测","临时开启摄像头约 7 秒，不保存照片。","检查摄像头",CheckCamera,out camera);ToolRow(p,119,"手动恢复蓝牙","重启匹配的蓝牙设备，连接会短暂断开。","恢复蓝牙",delegate{RunRepair("Bluetooth");},out bt);ToolRow(p,238,"恢复键盘 / Touch Bar","输入设备异常时使用，匹配设备会短暂重启。","恢复输入设备",delegate{RunRepair("Input");},out input);ToolRow(p,357,"恢复安装前设置","撤销本补丁的摄像头、电源方案和唤醒任务。","卸载并回滚",Uninstall,out restore);}
    private void BuildPower(){
        var p=Page("power");var top=new Surface{Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right};top.SetBounds(0,0,768,110);p.Controls.Add(top);AddLabel(top,"关机耗电与自动开机",20,14,620,29,14,ink,true);fastState=AddLabel(top,"正在读取快速启动设置…",21,54,690,32,10,muted,false);
        AddLabel(p,"快速启动可能影响关机兼容性。关闭它可以作为排查的一步，\n仍需区分关机失败、固件启动、外设、电池与硬件问题。",2,129,740,53,10,muted,false);
        powerApply=ButtonAt(p,"关闭快速启动",0,198,186,true,delegate{PowerAction("Apply");});powerRestore=ButtonAt(p,"恢复原设置",202,198,160,false,delegate{PowerAction("Restore");});ButtonAt(p,"采集关机日志",378,198,173,false,delegate{PowerAction("Diagnose");});
        var test=new Surface{Anchor=AnchorStyles.Top|AnchorStyles.Left|AnchorStyles.Right};test.SetBounds(0,269,768,139);p.Controls.Add(test);AddLabel(test,"完整关机对照测试",20,15,620,30,12,ink,true);AddLabel(test,"先保存文档并退出程序。关机前会保存电量和开关机记录，\n下次开机后可再次采集日志，核对是否发生了异常重启。",21,53,552,57,9.5f,muted,false);var shut=ButtonAt(test,"完整关机…",596,52,149,false,FullShutdown);shut.Anchor=AnchorStyles.Top|AnchorStyles.Right;
        AddLabel(p,"固件或硬件导致的问题无法保证由 Windows 补丁消除。\n一夜耗尽电量，需要对问题机器做完整关机、断开外设及跨系统对照。",2,433,735,55,9.5f,muted,false);
    }
    private void BuildLogs(){var p=Page("logs");logs.Dock=DockStyle.Fill;logs.ReadOnly=true;logs.BackColor=Color.White;logs.ForeColor=ink;logs.BorderStyle=BorderStyle.FixedSingle;logs.Font=new Font("Consolas",10);logs.DetectUrls=false;p.Controls.Add(logs);}
    private void BuildHelp(){var p=Page("about");var card=new Surface{Anchor=AnchorStyles.Left|AnchorStyles.Top|AnchorStyles.Right};card.SetBounds(0,0,768,472);p.Controls.Add(card);
        AddLabel(card,"给客户的使用步骤",22,19,650,31,16,ink,true);
        AddLabel(card,"1   双击本 EXE，在 Windows 提示中授予管理员权限。\n\n2   完全退出微信等视频软件，点击“一键安装修复”。\n\n3   重开微信，选择 Virtual Camera，检查画面。\n\n4   合盖唤醒后，检查蓝牙、键盘和 Touch Bar。\n\n5   如需撤销，在“常用工具”中选择“卸载并回滚”。",23,75,695,234,11,ink,false);
        AddLabel(card,"作者与发行：lrylnx · 联系邮箱：lryln@foxmail.com\nCopyright © 2026 lrylnx · 请保留作者署名\n适用于 Intel T2 MacBook + Windows 10 x64（19041–19045）。\n已有 0.2 候选补丁可直接检测和管理，无需重复安装。\n独立维护的候选版，非 Apple 官方软件。",23,333,700,121,9.5f,muted,false);
        ButtonAt(p,"查看 Apple 电源排查说明",0,496,263,false,delegate{Process.Start(new ProcessStartInfo("https://support.apple.com/102605"){UseShellExecute=true});});
    }
    private void Navigate(string key){currentPage=key;foreach(var pair in pages)pair.Value.Visible=pair.Key==key;foreach(var pair in nav){pair.Value.BackColor=pair.Key==key?Color.FromArgb(35,64,81):navy;pair.Value.ForeColor=pair.Key==key?Color.White:Color.FromArgb(188,206,219);}string title=key=="home"?"MacBook Windows 兼容修复":key=="tools"?"常用恢复工具":key=="power"?"关机与耗电排查":key=="logs"?"执行记录":"使用帮助";heading.Text=title;subtitle.Text=key=="home"?"一次安装，集中管理摄像头、蓝牙与睡眠兼容性。":key=="logs"?"执行过程保留在本机，诊断报告由你选择保存。":"Intel T2 MacBook · 本地处理 · 可回滚";pages[key].BringToFront();}
    private void Append(string line){if(IsDisposed)return;if(InvokeRequired){try{BeginInvoke(new Action<string>(Append),line);}catch{}return;}logs.AppendText(line+Environment.NewLine);logs.SelectionStart=logs.TextLength;logs.ScrollToCaret();Core.Audit(line);}
    private void SetBusy(bool value,string message){busy=value;activity.Text=message;activity.ForeColor=value?muted:ink;progress.Style=value?ProgressBarStyle.Marquee:ProgressBarStyle.Blocks;progress.MarqueeAnimationSpeed=value?25:0;progress.Value=value?0:100;UpdateButtons();}
    private void UpdateButtons(){foreach(var b in actions)b.Enabled=ready&&!busy;bool supported=Core.Flag(state,"Supported");bool installed=Core.Text(state,"InstallStatus")=="Installed";if(install!=null){install.Enabled=ready&&!busy&&supported;install.Text=installed?"检查修复状态":"一键安装修复";}if(restore!=null)restore.Enabled=ready&&!busy&&Core.Text(state,"InstallStatus")!="None";if(bt!=null)bt.Enabled=ready&&!busy&&installed;if(input!=null)input.Enabled=ready&&!busy&&installed;if(camera!=null)camera.Enabled=ready&&!busy&&supported;if(powerApply!=null)powerApply.Enabled=ready&&!busy&&supported;if(powerRestore!=null)powerRestore.Enabled=ready&&!busy&&Core.Flag(state,"PowerBackup");}
    private void DisplayState(Dictionary<string,object> value){state=value;machine.Text=Core.Text(state,"Model");os.Text=Core.Text(state,"OS")+" · Build "+Core.Text(state,"Build");bool supported=Core.Flag(state,"Supported");bool installed=Core.Text(state,"InstallStatus")=="Installed";bool complete=installed&&Core.Flag(state,"TaskPresent")&&Core.Text(state,"CameraPath").StartsWith(Core.InstallRoot,StringComparison.OrdinalIgnoreCase);badge.Text=!supported?"机型不支持":complete?"已安装":installed?"需要检查":"可安装";badge.ForeColor=supported?teal:Color.FromArgb(155,79,31);for(int i=0;i<3;i++)cardStates[i].Text=complete?"补丁已安装":installed?"请检查状态":"尚未安装";fastState.Text=Core.Text(state,"FastStartup")=="0"?"当前快速启动：已关闭":"当前快速启动：开启或未明确设置";string report=Core.Text(state,"NewestReport");if(!String.IsNullOrEmpty(report))lastReport=report;UpdateButtons();}
    public void SetTestStatus(Dictionary<string,object> s){testMode=true;ready=true;DisplayState(s);SetBusy(false,"检测完成 · 本机已安装兼容性补丁");}
    public void RenderPages(string folder){foreach(var key in new[]{"home","tools","power","logs","about"}){Navigate(key);ResponsiveLayout();Application.DoEvents();using(var bitmap=new Bitmap(Width,Height)){DrawToBitmap(bitmap,new Rectangle(Point.Empty,Size));bitmap.Save(Path.Combine(folder,key=="home"?"gui-preview.png":"gui-"+key+".png"));}}}
    private async Task Initialize(){SetBusy(true,"正在验证程序并读取本机状态…");try{var s=await Task.Run(delegate{Core.ExtractPayload(null);return Core.Status();});ready=true;DisplayState(s);Append("程序文件校验通过。机型："+Core.Text(s,"Model"));SetBusy(false,Core.Flag(s,"Supported")?"检测完成，可选择需要的操作。":"此机型不在安装范围内，可查看帮助。 ");}catch(Exception ex){Append(ex.Message);SetBusy(false,"初始化失败，请查看执行记录。");MessageBox.Show(this,ex.Message,Text,MessageBoxButtons.OK,MessageBoxIcon.Error);}}
    private async void RefreshState(){if(busy||!ready)return;SetBusy(true,"正在重新检测状态…");try{DisplayState(await Task.Run(new Func<Dictionary<string,object>>(Core.Status)));Append("状态检测完成。");SetBusy(false,"状态已更新。");}catch(Exception ex){Fail(ex.Message);}}
    private async Task<bool> Operation(string title,string script,string args){if(busy||!ready)return false;SetBusy(true,title+"，请稍候…");Append("\n["+DateTime.Now.ToString("HH:mm:ss")+"] "+title);try{var r=await Task.Run(delegate{return Core.RunScript(script,args,Append);});if(r.Code!=0){Fail((r.Output+r.Error).Trim());return false;}DisplayState(await Task.Run(new Func<Dictionary<string,object>>(Core.Status)));SetBusy(false,title+"完成。");return true;}catch(Exception ex){Fail(ex.Message);return false;}}
    private void Fail(string detail){Append(detail);SetBusy(false,"操作未完成，原因为："+(detail.Length>52?detail.Substring(0,52)+"…":detail));MessageBox.Show(this,detail.Length>1600?detail.Substring(0,1600):detail,Text,MessageBoxButtons.OK,MessageBoxIcon.Warning);}
    private async void Install(){if(Core.Text(state,"InstallStatus")=="Installed"){RefreshState();return;}if(await Operation("安装兼容性补丁","Install.ps1",""))MessageBox.Show(this,"安装已完成。\n\n请重开微信，在摄像头列表中选择 Virtual Camera。\n随后测试一次合盖唤醒，检查蓝牙、键盘与 Touch Bar。",Text,MessageBoxButtons.OK,MessageBoxIcon.Information);}
    private async void Uninstall(){if(MessageBox.Show(this,"恢复安装前的摄像头注册、电源方案和蓝牙任务？\n如通过本助手修改过快速启动，也会恢复其原设置。\n\n请先完全退出微信等视频软件。", "卸载并回滚",MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)return;if(Core.Flag(state,"PowerBackup")){if(!await Operation("恢复关机原设置","PowerTools.ps1","-Action Restore"))return;}await Operation("卸载并回滚","Uninstall.ps1","");}
    private async void RunRepair(string mode){string label=mode=="Bluetooth"?"蓝牙":"键盘 / Touch Bar";if(MessageBox.Show(this,"将短暂重启匹配的"+label+"设备，现有连接可能暂时断开。\n\n是否继续？", "恢复"+label,MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)return;await Operation("恢复"+label,"Resume.ps1","-Mode "+mode);}
    private async void CheckCamera(){if(MessageBox.Show(this,"检测会临时开启内置摄像头约 7 秒，不保存照片。\n请先结束微信预览等其他摄像头使用。", "摄像头出帧检测",MessageBoxButtons.OKCancel,MessageBoxIcon.Information)!=DialogResult.OK)return;await Operation("摄像头出帧检测","CameraCheck.ps1","");}
    private async void PowerAction(string action){string label=action=="Apply"?"关闭快速启动":action=="Restore"?"恢复快速启动原设置":"采集关机日志";if(await Operation(label,"PowerTools.ps1","-Action "+action)){if(action=="Diagnose")SaveReport();else MessageBox.Show(this,action=="Apply"?"已关闭快速启动，并保存原设置。\n\n这是一项关机兼容性措施，仍需在问题机器验证是否改善。":"已恢复原设置。",Text,MessageBoxButtons.OK,MessageBoxIcon.Information);}}
    private async void ExportDiagnostic(){if(await Operation("收集诊断记录","Diagnose.ps1",""))SaveReport();}
    private void SaveReport(){if(String.IsNullOrEmpty(lastReport)||!Directory.Exists(lastReport)){MessageBox.Show(this,"尚未生成诊断记录。",Text);return;}using(var dialog=new SaveFileDialog{Filter="ZIP 诊断压缩包|*.zip",FileName="MacBook诊断-"+DateTime.Now.ToString("yyyyMMdd-HHmmss")+".zip",OverwritePrompt=true}){if(dialog.ShowDialog(this)!=DialogResult.OK)return;try{if(File.Exists(dialog.FileName))File.Delete(dialog.FileName);ZipFile.CreateFromDirectory(lastReport,dialog.FileName,CompressionLevel.Optimal,false);Append("诊断已保存："+dialog.FileName);MessageBox.Show(this,"诊断文件已保存，可以自行交给售后。\n程序没有自动上传。",Text,MessageBoxButtons.OK,MessageBoxIcon.Information);}catch(Exception ex){Fail(ex.Message);}}}
    private async void FullShutdown(){if(MessageBox.Show(this,"这会立即请求完整关机。请先保存文档并退出程序。\n\n程序会先保存关机前的电量与事件记录。\n是否现在关机？", "完整关机测试",MessageBoxButtons.YesNo,MessageBoxIcon.Warning,MessageBoxDefaultButton.Button2)!=DialogResult.Yes)return;if(await Operation("保存关机前记录","PowerTools.ps1","-Action MarkShutdown")){Core.Audit("用户确认完整关机。");Process.Start(new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows),"System32","shutdown.exe"),"/s /t 0"){UseShellExecute=false,CreateNoWindow=true});}}
}
