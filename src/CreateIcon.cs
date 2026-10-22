// Copyright (c) 2026 lrylnx. Contact: lryln@foxmail.com. See LICENSE and NOTICE.md.
using System;using System.Drawing;using System.Drawing.Drawing2D;using System.IO;
class CreateIcon {
 static void Main(string[] a){using(var b=new Bitmap(64,64))using(var g=Graphics.FromImage(b)){
  g.SmoothingMode=SmoothingMode.AntiAlias;g.Clear(Color.Transparent);
  using(var brush=new SolidBrush(Color.FromArgb(0,126,133)))g.FillEllipse(brush,1,1,62,62);
  using(var pen=new Pen(Color.White,3)){g.DrawRectangle(pen,14,16,36,25);g.DrawLine(pen,10,46,54,46);}
  using(var pen=new Pen(Color.FromArgb(163,245,215),3.5f)){pen.StartCap=LineCap.Round;pen.EndCap=LineCap.Round;g.DrawLines(pen,new[]{new Point(24,29),new Point(30,35),new Point(41,23)});}
  using(var icon=Icon.FromHandle(b.GetHicon()))using(var f=File.Create(a[0]))icon.Save(f);
 }}
}
