#include "VirtualCam.cpp"
int main(int argc, char** argv) {
    int w=argc>1?atoi(argv[1]):640, h=argc>2?atoi(argv[2]):480;
    if(w<16 || w>1920 || h<16 || h>1080 || (w&1) || (h&1)) return 2;
    g_cfgSrcFps=argc>3?atoi(argv[3]):0;
    CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);
    SharedInit(); CaptureCtx ctx;
    bool ok=BuildCaptureGraph(&ctx,L"FaceTime HD Camera (Built-in)",w,h);
    LONG first=g_sf.seq; DWORD start=GetTickCount();
    while(ok && GetTickCount()-start<3000) { PumpOnce(); Sleep(10); }
    EnterCriticalSection(&g_sf.cs);
    int aw=g_sf.w, ah=g_sf.h; long len=g_sf.len; LONG frames=g_sf.seq-first;
    double sum=0, sum2=0; int n=aw*ah;
    if(g_sf.ready && n>0 && len>=n) for(int i=0;i<n;i++){double v=g_sf.buf[i];sum+=v;sum2+=v*v;}
    printf("requested=%dx%d connected=%dx%d subtype=%08X bytes=%ld frames_in_3s=%ld meanY=%.2f varianceY=%.2f\n",w,h,aw,ah,(unsigned)g_sf.subtype.Data1,len,frames,n?sum/n:0,n?sum2/n-(sum/n)*(sum/n):0);
    bool valid=ok && aw==w && ah==h && len==w*h*3/2 && frames>15;
    LeaveCriticalSection(&g_sf.cs);
    TearDownCapture(&ctx); CoUninitialize();
    return valid?0:1;
}
