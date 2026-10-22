// Candidate DLL and verifier EXE have separate internal capture frame stores.
#include "VirtualCam.cpp"
#define HR(x) do{HRESULT r=(x);if(FAILED(r)){printf("FAIL %s hr=%08X\n",#x,(unsigned)r);return 2;}}while(0)
int main(int argc,char**argv){
 if(argc<4){printf("OutputProbe DLL width height [rgb|yuy2|nv12]\n");return 2;}
 int w=atoi(argv[2]),h=atoi(argv[3]);if(w<16||w>1920||h<16||h>1080||(w&3)||(h&1))return 2;
 GUID sub=MEDIASUBTYPE_RGB24;int bpp=24;
 if(argc>4 && !strcmp(argv[4],"yuy2")){sub=MEDIASUBTYPE_YUY2;bpp=16;}
 if(argc>4 && !strcmp(argv[4],"nv12")){sub=MEDIASUBTYPE_NV12;bpp=12;}
 int expected=w*h*bpp/8;
 HR(CoInitializeEx(NULL,COINIT_APARTMENTTHREADED));SharedInit();
 VirtualFree(g_sf.buf,0,MEM_RELEASE);g_sf.cap=1920*1080*3;
 g_sf.buf=(BYTE*)VirtualAlloc(NULL,g_sf.cap,MEM_COMMIT,PAGE_READWRITE);if(!g_sf.buf)return 2;
 HMODULE dll=LoadLibraryA(argv[1]);if(!dll){printf("LoadLibrary %lu\n",GetLastError());return 2;}
 typedef HRESULT(STDAPICALLTYPE* GetClass)(REFCLSID,REFIID,void**);
 GetClass get=(GetClass)GetProcAddress(dll,"DllGetClassObject");if(!get)return 2;
 IClassFactory*factory=NULL;IBaseFilter*cam=NULL;
 HR(get(CLSID_VirtualCam,IID_IClassFactory,(void**)&factory));HR(factory->CreateInstance(NULL,IID_IBaseFilter,(void**)&cam));factory->Release();
 IGraphBuilder*graph=NULL;IMediaControl*control=NULL;
 HR(CoCreateInstance(CLSID_FilterGraph,NULL,CLSCTX_INPROC_SERVER,IID_IGraphBuilder,(void**)&graph));
 HR(graph->QueryInterface(IID_IMediaControl,(void**)&control));HR(graph->AddFilter(cam,L"Candidate"));
 IPin*out=FindPin(cam,PINDIR_OUTPUT);if(!out)return 2;
 IAMStreamConfig*sc=NULL;HR(out->QueryInterface(IID_IAMStreamConfig,(void**)&sc));
 int nc=0,sz=0;HR(sc->GetNumberOfCapabilities(&nc,&sz));
 BYTE*caps=(BYTE*)malloc(sz);AM_MEDIA_TYPE*first=NULL;HR(sc->GetStreamCaps(0,&first,caps));
 VIDEOINFOHEADER*fv=(VIDEOINFOHEADER*)first->pbFormat;
 printf("caps=%d first=%ldx%ld subtype=%08X\n",nc,fv->bmiHeader.biWidth,fv->bmiHeader.biHeight,(unsigned)first->subtype.Data1);
 if(nc!=12 || fv->bmiHeader.biWidth!=640 || fv->bmiHeader.biHeight!=480 || first->subtype!=MEDIASUBTYPE_RGB24)return 3;
 FreeMediaType(first);CoTaskMemFree(first);free(caps);
 VIDEOINFOHEADER vi={};vi.AvgTimePerFrame=333333;vi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);vi.bmiHeader.biWidth=w;vi.bmiHeader.biHeight=h;vi.bmiHeader.biPlanes=1;vi.bmiHeader.biBitCount=bpp;vi.bmiHeader.biSizeImage=expected;vi.bmiHeader.biCompression=bpp==24?BI_RGB:sub.Data1;
 AM_MEDIA_TYPE mt={};mt.majortype=MEDIATYPE_Video;mt.subtype=sub;mt.formattype=FORMAT_VideoInfo;mt.bFixedSizeSamples=TRUE;mt.lSampleSize=expected;mt.cbFormat=sizeof(vi);mt.pbFormat=(BYTE*)&vi;
 HR(sc->SetFormat(&mt));sc->Release();
 CSinkFilter*sink=new CSinkFilter();HR(graph->AddFilter(sink,L"Verifier"));
 bool defaultType=argc>5 && !strcmp(argv[5],"default");
 HR(graph->ConnectDirect(out,sink->Pin(),defaultType?NULL:&mt));HR(control->Run());
 DWORD start=GetTickCount();LONG varied=0,last=0;double variance=0;
 while(GetTickCount()-start<7000){
   PumpOnce();Sleep(10);EnterCriticalSection(&g_sf.cs);
   if(g_sf.ready && g_sf.seq!=last){
     last=g_sf.seq;double sum=0,sq=0;int count=0;
     int end=bpp==12?w*h:expected;int step=bpp==16?18:17;
     for(int i=0;i<end;i+=step){double v=g_sf.buf[i];sum+=v;sq+=v*v;count++;}
     variance=sq/count-(sum/count)*(sum/count);if(variance>3)varied++;
   }
   LeaveCriticalSection(&g_sf.cs);
 }
 HR(control->Stop());
 printf("output=%dx%d bpp=%d frames=%ld varied_observations=%ld bytes=%ld variance=%.2f\n",w,h,bpp,g_sf.seq,varied,g_sf.len,variance);
 bool valid=g_sf.seq>60 && varied>30 && g_sf.len==expected;
 out->Release();control->Release();graph->RemoveFilter(cam);graph->RemoveFilter(sink);cam->Release();sink->Release();graph->Release();
 CoUninitialize();return valid?0:1;
}
