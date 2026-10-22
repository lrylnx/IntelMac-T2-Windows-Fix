// ---------------------------------------------------------------------------
//  VirtualCam - a minimal DirectShow virtual camera (single DLL, ~150 KB)
//
//  Behaviour:
//    - Enumerates the real camera through DirectShow (the only path that works
//      on the tested Boot Camp machine), captures native 1280x720 with 640x480 fallback and publishes
//      a software camera with 640x480 / 1280x720 / 1920x1080 @ 30/15 fps,
//      in RGB24/YUY2 (also accepts negotiated NV12/I420). 1080p is an upscale.
//
//  License: public domain / do whatever you want.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#include <dshow.h>
#include <qedit.h>
#include <stdio.h>
#include <string.h>
#include "Resample.h"

// mingw has no swprintf_s
#define swprintf_s _snwprintf

// keep the DLL self-contained: no C++ runtime dependency at all
#include <stdlib.h>
void* operator new(size_t n) { return malloc(n); }
void* operator new[](size_t n) { return malloc(n); }
void  operator delete(void* p) noexcept { free(p); }
void  operator delete[](void* p) noexcept { free(p); }
void  operator delete(void* p, size_t) noexcept { free(p); }
void  operator delete[](void* p, size_t) noexcept { free(p); }

// ---------------------------------------------------------------------------
// own CLSIDs / ids
// ---------------------------------------------------------------------------
// {7F3C1A20-9D54-4C1B-A37E-518B2D449F01}
static const GUID CLSID_VirtualCam =
    { 0x7f3c1a20, 0x9d54, 0x4c1b, { 0xa3, 0x7e, 0x51, 0x8b, 0x2d, 0x44, 0x9f, 0x01 } };
// {7F3C1A20-9D54-4C1B-A37E-518B2D449F02}
static const GUID CLSID_VirtualCamProp =
    { 0x7f3c1a20, 0x9d54, 0x4c1b, { 0xa3, 0x7e, 0x51, 0x8b, 0x2d, 0x44, 0x9f, 0x02 } };
// Null renderer (defined here because mingw's headers do not export it)
static const GUID CLSID_NullRendererX =
    { 0xc1f400a4, 0x3f08, 0x11d3, { 0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37 } };
// System clock {E436EBB1-524F-11CE-9F53-0020AF0BA770}
static const GUID CLSID_SystemClockX =
    { 0xe436ebb1, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
// Memory allocator {1E651CC0-B199-11D0-8212-00C04FC32C45}
static const GUID CLSID_MemoryAllocatorX =
    { 0x1e651cc0, 0xb199, 0x11d0, { 0x82, 0x12, 0x00, 0xc0, 0x4f, 0xc3, 0x2c, 0x45 } };
// {9B00F101-1567-11D1-B3F1-00AA003761C5}
static const GUID AMPROPSETID_PinX =
    { 0x9b00f101, 0x1567, 0x11d1, { 0xb3, 0xf1, 0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5 } };
// {FB6C4281-0353-11D1-905F-0000C0CC16BA}
static const GUID PIN_CATEGORY_CAPTUREX =
    { 0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba } };
// {31EFAC30-515C-11D0-A9AA-00AA0061BE93}
static const GUID IID_IKsPropertySetX =
    { 0x31efac30, 0x515c, 0x11d0, { 0xa9, 0xaa, 0x00, 0xaa, 0x00, 0x61, 0xbe, 0x93 } };

// mingw 头文件未导出该错误码
#define VFW_E_NOT_NEGOTIATED_X ((HRESULT)0x8004020BL)

#define VCAM_NAME       L"Virtual Camera"
#define SRC_DEF_W       640       // 兜底采集尺寸
#define SRC_DEF_H       480

// 实际请求的采集尺寸：SrcW/SrcH 均为 0 时自动（先试 1280x720，无帧则退回 640x480）
static int  g_reqW = 0;
static int  g_reqH = 0;
#define MAX_W           2048
#define MAX_H           2048

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

#include <stdarg.h>
// 调试日志：设置环境变量 VCAM_LOG=<路径> 后写入
static char g_logFile[MAX_PATH] = { 0 };   // vcam.ini 里 Log=1 时写入的固定日志

static void VLog(const char* fmt, ...)
{
    const char* p = getenv("VCAM_LOG");
    if (!p || !*p) p = g_logFile[0] ? g_logFile : 0;
    if (!p || !*p) return;
    FILE* sizeCheck=fopen(p,"rb");
    long logBytes=0; if(sizeCheck){ fseek(sizeCheck,0,SEEK_END); logBytes=ftell(sizeCheck); fclose(sizeCheck); }
    FILE* f = fopen(p, logBytes > 2*1024*1024 ? "w" : "a");
    if (!f) return;
    fprintf(f, "[%lu] ", (unsigned long)GetCurrentProcessId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void FreeMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return;
    if (pmt->cbFormat && pmt->pbFormat) CoTaskMemFree(pmt->pbFormat);
    pmt->cbFormat = 0;
    pmt->pbFormat = NULL;
    if (pmt->pUnk) { pmt->pUnk->Release(); pmt->pUnk = NULL; }
}

// ---------------------------------------------------------------------------
// shared frame store (written by the capture thread, read by the pin thread)
// ---------------------------------------------------------------------------
struct SharedFrame
{
    CRITICAL_SECTION cs;
    BYTE*   buf;        // raw frame as delivered by the real camera
    long    cap;        // capacity in bytes
    long    len;        // valid bytes
    int     w, h;
    GUID    subtype;    // YUY2 or RGB24
    volatile LONG seq;
    volatile LONG ready;
};

static SharedFrame g_sf;
static LONG  g_sfInit = 0;
static HANDLE g_grabEvent = NULL;   // signalled on every new frame

static void SharedInit()
{
    if (InterlockedCompareExchange(&g_sfInit, 1, 0) == 0)
    {
        ZeroMemory(&g_sf, 0);
        InitializeCriticalSection(&g_sf.cs);
        g_sf.cap = 1280 * 960 * 3 + 4096;   // 够 720p 甚至 960p 用
        g_sf.buf = (BYTE*)VirtualAlloc(NULL, g_sf.cap, MEM_COMMIT, PAGE_READWRITE);
        g_grabEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    }
}

// ---------------------------------------------------------------------------
// scaling / colour conversion helpers (nearest neighbour, table driven)
// ---------------------------------------------------------------------------
static int  g_xoff[MAX_W];      // byte offset inside a source row
static int  g_xsel[MAX_W];      // 0 -> use Y0, 1 -> use Y1  (YUY2 only)
static int  g_yrow[MAX_H];      // source row index
static int  g_tblW = 0, g_tblH = 0;
static int  g_srcW = 0, g_srcH = 0;
static int  g_flipY = 0;        // 1: 源数据是 bottom-up DIB，需要垂直翻转

// ---- 运行期可调配置（vcam.ini，放在 DLL 同目录）----
static int  g_cfgFlipY   = 0;   // 1 = 上下翻转
static int  g_cfgSwapUV  = 0;   // 1 = 交换 U/V（部分驱动是 NV21）
static int  g_cfgTopDown = 0;   // 1 = 输出 biHeight 用负值（top-down）
static int  g_cfgFit     = 0;   // 0=原样 1=补边保比例 2=缩到75%居中
static int  g_cfgSrcFps  = 0;   // >0 时把它写进媒体类型，用来降低带宽换 720p
static int  g_cfgZoom    = -1;  // >=0 时设置 IAMCameraControl 的 Zoom；-1 不改
static int  g_cfgYUY2    = 1;   // 1 = 同时暴露 YUY2（与旧版枚举顺序一致）
static int g_cfgQuality = 1;
static bool g_cfgLoaded  = false;

static void LoadCfg()
{
    if (g_cfgLoaded) return;
    g_cfgLoaded = true;
    WCHAR path[MAX_PATH];
    HMODULE hm = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&LoadCfg, &hm)) return;
    if (!GetModuleFileNameW(hm, path, MAX_PATH)) return;
    WCHAR* dot = wcsrchr(path, L'.');
    if (dot) wcscpy(dot, L".ini");
    g_cfgFlipY   = GetPrivateProfileIntW(L"vCam", L"FlipY",   0, path);
    g_cfgSwapUV  = GetPrivateProfileIntW(L"vCam", L"SwapUV",  0, path);
    g_cfgTopDown = GetPrivateProfileIntW(L"vCam", L"TopDown", 0, path);
    g_cfgYUY2    = GetPrivateProfileIntW(L"vCam", L"YUY2",    1, path);
    g_cfgSrcFps = GetPrivateProfileIntW(L"vCam", L"SrcFps", 0, path);
    g_cfgQuality = GetPrivateProfileIntW(L"vCam", L"Quality", 1, path);
    g_cfgFit    = GetPrivateProfileIntW(L"vCam", L"Fit",    0, path);
    {   // Zoom：ini 里没写就当 -1（不改）
        WCHAR buf[32]; buf[0] = 0;
        GetPrivateProfileStringW(L"vCam", L"Zoom", L"-1", buf, 32, path);
        g_cfgZoom = _wtoi(buf);
    }
    g_reqW = GetPrivateProfileIntW(L"vCam", L"SrcW", 0, path);
    g_reqH = GetPrivateProfileIntW(L"vCam", L"SrcH", 0, path);
    if (g_reqW < 16 || g_reqH < 16 || g_reqW > 1920 || g_reqH > 1080 || (g_reqW&1) || (g_reqH&1)) { g_reqW = 0; g_reqH = 0; }   // 0 = 自动
    if (GetPrivateProfileIntW(L"vCam", L"Log", 0, path))
    {
        // 写到临时目录（Program Files 不可写）——
        // 记录宿主程序（微信/OBS…）到底请求了什么格式
        WCHAR tmp[MAX_PATH];
        DWORD tl = GetTempPathW(MAX_PATH, tmp);
        if (tl > 0 && tl + 12 < MAX_PATH)
        {
            wcscat(tmp, L"vcam.log");
            char ansi[MAX_PATH * 2];
            int n = WideCharToMultiByte(CP_ACP, 0, tmp, -1, ansi, sizeof(ansi) - 1, NULL, NULL);
            if (n > 0) { ansi[n] = 0; lstrcpynA(g_logFile, ansi, MAX_PATH); }
        }
    }
    VLog("Cfg: FlipY=%d SwapUV=%d TopDown=%d YUY2=%d Src=%dx%d(auto=%d)",
         g_cfgFlipY, g_cfgSwapUV, g_cfgTopDown, g_cfgYUY2, g_reqW, g_reqH, g_reqW == 0);
}
static int  g_tblFlip = -1;

static void BuildTables(int sw, int sh, int dw, int dh)
{
    if (g_tblW == dw && g_tblH == dh && g_srcW == sw && g_srcH == sh &&
        g_tblFlip == g_flipY) return;
    if (dw > MAX_W) dw = MAX_W;
    if (dh > MAX_H) dh = MAX_H;
    for (int x = 0; x < dw; x++)
    {
        int sx = x * sw / dw;
        g_xoff[x] = (sx & ~1) * 2;
        g_xsel[x] = sx & 1;
    }
    for (int y = 0; y < dh; y++)
    {
        int sy = y * sh / dh;
        if (g_flipY) sy = sh - 1 - sy;
        g_yrow[y] = sy;
    }
    g_tblW = dw; g_tblH = dh; g_srcW = sw; g_srcH = sh; g_tblFlip = g_flipY;
}

// YUY2 -> RGB24 with scaling
static void YUY2ToRGB(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    BuildTables(sw, sh, dw, dh);
    for (int y = 0; y < dh; y++)
    {
        const BYTE* srow = src + (size_t)g_yrow[y] * sw * 2;
        BYTE* drow = dst + (size_t)y * dw * 3;
        for (int x = 0; x < dw; x++)
        {
            const BYTE* p = srow + g_xoff[x];
            int Y = g_xsel[x] ? p[2] : p[0];
            int U = p[1];
            int V = p[3];
            int c = Y - 16, d = U - 128, e = V - 128;
            // MEDIASUBTYPE_RGB24 在 DirectShow 里是 BGR 字节序
            drow[x * 3 + 0] = (BYTE)clamp255((298 * c + 516 * d + 128) >> 8);   // B
            drow[x * 3 + 1] = (BYTE)clamp255((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            drow[x * 3 + 2] = (BYTE)clamp255((298 * c + 409 * e + 128) >> 8);   // R
        }
    }
}

// RGB24 -> RGB24 with scaling
static void RGBToRGB(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    BuildTables(sw, sh, dw, dh);
    for (int y = 0; y < dh; y++)
    {
        const BYTE* srow = src + (size_t)g_yrow[y] * sw * 3;
        BYTE* drow = dst + (size_t)y * dw * 3;
        for (int x = 0; x < dw; x++)
        {
            const BYTE* p = srow + (g_xoff[x] >> 1) * 3;
            drow[x * 3 + 0] = p[0];
            drow[x * 3 + 1] = p[1];
            drow[x * 3 + 2] = p[2];
        }
    }
}

// YUY2 -> YUY2 with scaling (copy Y, subsample U/V)
static void YUY2ToYUY2(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    BuildTables(sw, sh, dw, dh);
    for (int y = 0; y < dh; y++)
    {
        const BYTE* srow = src + (size_t)g_yrow[y] * sw * 2;
        BYTE* drow = dst + (size_t)y * dw * 2;
        for (int x = 0; x < dw; x += 2)
        {
            const BYTE* p0 = srow + g_xoff[x];
            const BYTE* p1 = srow + g_xoff[x + 1 < dw ? x + 1 : x];
            drow[x * 2 + 0] = g_xsel[x] ? p0[2] : p0[0];
            drow[x * 2 + 1] = p0[1];
            drow[x * 2 + 2] = g_xsel[x + 1 < dw ? x + 1 : x] ? p1[2] : p1[0];
            drow[x * 2 + 3] = p0[3];
        }
    }
}

// RGB24 -> YUY2 with scaling
static void RGBToYUY2(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    BuildTables(sw, sh, dw, dh);
    for (int y = 0; y < dh; y++)
    {
        const BYTE* srow = src + (size_t)g_yrow[y] * sw * 3;
        BYTE* drow = dst + (size_t)y * dw * 2;
        for (int x = 0; x < dw; x += 2)
        {
            int x2 = (x + 1 < dw) ? x + 1 : x;
            const BYTE* a = srow + (g_xoff[x] >> 1) * 3;
            const BYTE* b = srow + (g_xoff[x2] >> 1) * 3;
            int b0 = a[0], g0 = a[1], r0 = a[2];   // 源是 BGR
            int b1 = b[0], g1 = b[1], r1 = b[2];
            int ya = (66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8, ya2 = ya + 16;
            int yb = (66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8, yb2 = yb + 16;
            int rm = (r0 + r1) >> 1, gm = (g0 + g1) >> 1, bm = (b0 + b1) >> 1;
            int u = ((-38 * rm - 74 * gm + 112 * bm + 128) >> 8) + 128;
            int v = ((112 * rm - 94 * gm - 18 * bm + 128) >> 8) + 128;
            drow[x * 2 + 0] = (BYTE)clamp255(ya2);
            drow[x * 2 + 1] = (BYTE)clamp255(u);
            drow[x * 2 + 2] = (BYTE)clamp255(yb2);
            drow[x * 2 + 3] = (BYTE)clamp255(v);
        }
    }
}

// NV12 -> RGB24 with scaling  (NV12: Y 平面 + 交织 UV 平面)
static void NV12ToRGB(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    unsigned char* scaled = NULL;
    if (g_cfgQuality && (sw != dw || sh != dh)) {
        scaled=(unsigned char*)malloc((size_t)dw*dh*3/2);
        if(scaled && ScaleNV12(src,sw,sh,scaled,dw,dh)) {
            src=scaled; sw=dw; sh=dh;
        }
    }

    BuildTables(sw, sh, dw, dh);
    const BYTE* uv = src + (size_t)sw * sh;
    for (int y = 0; y < dh; y++)
    {
        int sy = g_yrow[y];
        const BYTE* yrow = src + (size_t)sy * sw;
        const BYTE* urow = uv + (size_t)(sy >> 1) * sw;
        BYTE* drow = dst + (size_t)y * dw * 3;
        for (int x = 0; x < dw; x++)
        {
            int sx = (g_xoff[x] >> 1) | g_xsel[x];
            int ux = (sx >> 1) * 2;          // UV 平面字节偏移（每 2 像素 1 个 UV 对）
            int Y = yrow[sx];
            int U, V;
            if (g_cfgSwapUV) { V = urow[ux]; U = urow[ux + 1]; }   // NV21
            else             { U = urow[ux]; V = urow[ux + 1]; }   // NV12
            int c = Y - 16, d = U - 128, e = V - 128;
            // MEDIASUBTYPE_RGB24 在 DirectShow 里是 BGR 字节序
            drow[x * 3 + 0] = (BYTE)clamp255((298 * c + 516 * d + 128) >> 8);   // B
            drow[x * 3 + 1] = (BYTE)clamp255((298 * c - 100 * d - 208 * e + 128) >> 8); // G
            drow[x * 3 + 2] = (BYTE)clamp255((298 * c + 409 * e + 128) >> 8);   // R
        }
    }
    free(scaled);
}

// NV12 -> YUY2 with scaling
static void NV12ToYUY2(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh)
{
    unsigned char* scaled = NULL;
    if (g_cfgQuality && (sw != dw || sh != dh)) {
        scaled=(unsigned char*)malloc((size_t)dw*dh*3/2);
        if(scaled && ScaleNV12(src,sw,sh,scaled,dw,dh)) {
            src=scaled; sw=dw; sh=dh;
        }
    }

    BuildTables(sw, sh, dw, dh);
    const BYTE* uv = src + (size_t)sw * sh;
    for (int y = 0; y < dh; y++)
    {
        int sy = g_yrow[y];
        const BYTE* yrow = src + (size_t)sy * sw;
        const BYTE* urow = uv + (size_t)(sy >> 1) * sw;
        BYTE* drow = dst + (size_t)y * dw * 2;
        for (int x = 0; x < dw; x += 2)
        {
            int x2 = (x + 1 < dw) ? x + 1 : x;
            int sx0 = (g_xoff[x] >> 1) | g_xsel[x];
            int sx1 = (g_xoff[x2] >> 1) | g_xsel[x2];
            int ux = (sx0 >> 1) * 2;         // UV 平面字节偏移
            drow[x * 2 + 0] = yrow[sx0];
            drow[x * 2 + 1] = g_cfgSwapUV ? urow[ux + 1] : urow[ux];
            drow[x * 2 + 2] = yrow[sx1];
            drow[x * 2 + 3] = g_cfgSwapUV ? urow[ux] : urow[ux + 1];
        }
    }
    free(scaled);
}

// ---------------------------------------------------------------------------
// capture side : opens the real camera and pumps frames into g_sf
// ---------------------------------------------------------------------------
class CGrabCB : public ISampleGrabberCB
{
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB)
        { *ppv = (ISampleGrabberCB*)this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return 2; }
    STDMETHODIMP_(ULONG) Release() { return 1; }
    STDMETHODIMP SampleCB(double, IMediaSample*) { return S_OK; }
    STDMETHODIMP BufferCB(double, BYTE* pBuffer, long lBuffer)
    {
        if (!pBuffer || lBuffer <= 0) return S_OK;
        EnterCriticalSection(&g_sf.cs);
        if (g_sf.buf && lBuffer <= g_sf.cap)
        {
            memcpy(g_sf.buf, pBuffer, lBuffer);
            g_sf.len = lBuffer;
            InterlockedIncrement(&g_sf.seq);
            g_sf.ready = 1;
        }
        LeaveCriticalSection(&g_sf.cs);
        static int cbc = 0;
        if ((cbc++ % 30) == 0) VLog("BufferCB: seq=%d len=%ld", cbc, lBuffer);
        if (g_grabEvent) SetEvent(g_grabEvent);
        return S_OK;
    }
};
static CGrabCB g_cb;

class CSinkFilter;

struct CaptureCtx
{
    IGraphBuilder* graph;
    IMediaControl* control;
    IBaseFilter*   cam;
    CSinkFilter*   sinkF;
};

static IPin* FindPin(IBaseFilter* pF, PIN_DIRECTION dir)
{
    IPin* found = NULL;
    IEnumPins* ep = NULL;
    if (!pF || FAILED(pF->EnumPins(&ep))) return NULL;
    IPin* pin = NULL;
    while (ep->Next(1, &pin, NULL) == S_OK)
    {
        PIN_DIRECTION d;
        if (SUCCEEDED(pin->QueryDirection(&d)) && d == dir) { found = pin; break; }
        pin->Release();
        pin = NULL;
    }
    ep->Release();
    return found;
}

// 优先选 PIN_CATEGORY_CAPTURE 的输出引脚（UVC 相机可能同时存在 Preview 引脚）
static IPin* FindCapturePin(IBaseFilter* pF)
{
    IPin* fallback = NULL;
    IEnumPins* ep = NULL;
    if (!pF || FAILED(pF->EnumPins(&ep))) return NULL;
    IPin* pin = NULL;
    while (ep->Next(1, &pin, NULL) == S_OK)
    {
        PIN_DIRECTION d;
        if (FAILED(pin->QueryDirection(&d)) || d != PINDIR_OUTPUT) { pin->Release(); continue; }

        bool isCapture = false;
        IKsPropertySet* ks = NULL;
        if (SUCCEEDED(pin->QueryInterface(IID_IKsPropertySetX, (void**)&ks)) && ks)
        {
            GUID cat; ZeroMemory(&cat, sizeof(cat));
            DWORD rb = 0;
            if (SUCCEEDED(ks->Get(AMPROPSETID_PinX, 0, NULL, 0, &cat, sizeof(cat), &rb)))
                isCapture = IsEqualGUID(cat, PIN_CATEGORY_CAPTUREX);
            ks->Release();
        }
        VLog("FindCapturePin: pin capture=%d", isCapture ? 1 : 0);
        if (isCapture) { ep->Release(); if (fallback) fallback->Release(); return pin; }
        if (!fallback) fallback = pin; else pin->Release();
    }
    ep->Release();
    if (fallback) VLog("FindCapturePin: fallback to first output pin");
    return fallback;
}

// ---------------------------------------------------------------------------
// 自实现的采集 sink 滤镜（替代 SampleGrabber + NullRenderer）
// —— SampleGrabber 会向下游提供自己的分配器，UVC 驱动因此不出数据；
//    ffmpeg 也是用自带 sink 才跑通的。这里只做两件事：接收样本、拷贝数据。
// ---------------------------------------------------------------------------
static bool CopyMediaType(AM_MEDIA_TYPE* dst, const AM_MEDIA_TYPE* src)
{
    *dst = *src;
    if (src->cbFormat && src->pbFormat)
    {
        dst->pbFormat = (BYTE*)CoTaskMemAlloc(src->cbFormat);
        if (!dst->pbFormat) { dst->cbFormat = 0; return false; }
        memcpy(dst->pbFormat, src->pbFormat, src->cbFormat);
    }
    else { dst->pbFormat = NULL; dst->cbFormat = 0; }
    if (dst->pUnk) dst->pUnk->AddRef();
    return true;
}

class CSinkFilter;

class CSinkPin : public IPin, public IMemInputPin
{
    LONG          m_ref;
    CSinkFilter*  m_owner;
    IPin*         m_connected;
    AM_MEDIA_TYPE m_mt;
    bool          m_hasMt;
    long          m_count;

public:
    CSinkPin(CSinkFilter* owner);
    ~CSinkPin();

    void Close();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    // IPin
    STDMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*);
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP Disconnect();
    STDMETHODIMP ConnectedTo(IPin** ppPin);
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir);
    STDMETHODIMP QueryId(LPWSTR* Id);
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum);
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin);
    STDMETHODIMP EndOfStream();
    STDMETHODIMP BeginFlush();
    STDMETHODIMP EndFlush();
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double);

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator** ppAllocator);
    STDMETHODIMP NotifyAllocator(IMemAllocator*, BOOL);
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES* pProps);
    STDMETHODIMP Receive(IMediaSample* pSample);
    STDMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nProcessed);
    STDMETHODIMP ReceiveCanBlock();
};

class CSinkFilter : public IBaseFilter
{
    LONG             m_ref;
    IFilterGraph*    m_graph;
    WCHAR            m_name[64];
    CSinkPin*        m_pin;
    FILTER_STATE     m_state;
    IReferenceClock* m_clock;

public:
    CSinkFilter();
    ~CSinkFilter();

    CSinkPin* Pin() const { return m_pin; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    { LONG r = InterlockedDecrement(&m_ref); if (!r) delete this; return r; }

    STDMETHODIMP GetClassID(CLSID* pClsID);
    STDMETHODIMP Stop();
    STDMETHODIMP Pause();
    STDMETHODIMP Run(REFERENCE_TIME tStart);
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State);
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock);
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock);
    STDMETHODIMP EnumPins(IEnumPins** ppEnum);
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin);
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo);
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName);
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo);
};

// 单引脚枚举器
class CEnumPins1 : public IEnumPins
{
    LONG  m_ref;
    IPin* m_pin;
    int   m_idx;
public:
    CEnumPins1(IPin* pin) : m_ref(1), m_pin(pin), m_idx(0) { if (m_pin) m_pin->AddRef(); }
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins)
        { *ppv = (IEnumPins*)this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) { if (m_pin) m_pin->Release(); delete this; }
        return r;
    }
    STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* fetched)
    {
        if (!pp || c < 1) return E_INVALIDARG;
        if (m_idx >= 1 || !m_pin) { if (fetched) *fetched = 0; return S_FALSE; }
        pp[0] = m_pin;
        m_pin->AddRef();
        m_idx = 1;
        if (fetched) *fetched = 1;
        return c == 1 ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_idx += (int)c; return m_idx > 1 ? S_FALSE : S_OK; }
    STDMETHODIMP Reset() { m_idx = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** ppEnum)
    {
        if (!ppEnum) return E_POINTER;
        CEnumPins1* e = new CEnumPins1(m_pin);
        e->m_idx = m_idx;
        *ppEnum = e;
        return S_OK;
    }
};

// --- CSinkFilter -----------------------------------------------------------
CSinkFilter::CSinkFilter() : m_ref(1), m_graph(NULL), m_pin(NULL),
    m_state(State_Stopped), m_clock(NULL)
{
    m_name[0] = 0;
    m_pin = new CSinkPin(this);
}

CSinkFilter::~CSinkFilter()
{
    if (m_clock) m_clock->Release();
    if (m_pin)   m_pin->Release();
}

HRESULT CSinkFilter::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IBaseFilter ||
        riid == IID_IMediaFilter || riid == IID_IPersist)
    { *ppv = (IBaseFilter*)this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}

HRESULT CSinkFilter::GetClassID(CLSID* p) { if (!p) return E_POINTER; *p = CLSID_VirtualCam; return S_OK; }
HRESULT CSinkFilter::Stop()  { m_state = State_Stopped; return S_OK; }
HRESULT CSinkFilter::Pause() { m_state = State_Paused;  return S_OK; }
HRESULT CSinkFilter::Run(REFERENCE_TIME) { m_state = State_Running; return S_OK; }
HRESULT CSinkFilter::GetState(DWORD, FILTER_STATE* s) { if (!s) return E_POINTER; *s = m_state; return S_OK; }

HRESULT CSinkFilter::SetSyncSource(IReferenceClock* c)
{
    if (m_clock) { m_clock->Release(); m_clock = NULL; }
    if (c) { m_clock = c; m_clock->AddRef(); }
    return S_OK;
}
HRESULT CSinkFilter::GetSyncSource(IReferenceClock** pp)
{
    if (!pp) return E_POINTER;
    if (m_clock) m_clock->AddRef();
    *pp = m_clock;
    return S_OK;
}

HRESULT CSinkFilter::EnumPins(IEnumPins** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumPins1(m_pin);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

HRESULT CSinkFilter::FindPin(LPCWSTR Id, IPin** ppPin)
{
    if (!ppPin || !Id) return E_POINTER;
    if (wcscmp(Id, L"In") == 0) { *ppPin = m_pin; m_pin->AddRef(); return S_OK; }
    *ppPin = NULL;
    return VFW_E_NOT_FOUND;
}

HRESULT CSinkFilter::QueryFilterInfo(FILTER_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    wcscpy(pInfo->achName, m_name[0] ? m_name : L"VcamSink");
    pInfo->pGraph = m_graph;
    if (m_graph) m_graph->AddRef();
    return S_OK;
}

HRESULT CSinkFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName)
{
    if (m_graph) { m_graph->Release(); m_graph = NULL; }
    if (pGraph) { m_graph = pGraph; m_graph->AddRef(); }
    if (pName) { wcsncpy(m_name, pName, 63); m_name[63] = 0; }
    return S_OK;
}

HRESULT CSinkFilter::QueryVendorInfo(LPWSTR* p) { if (p) *p = NULL; return E_NOTIMPL; }

// --- CSinkPin --------------------------------------------------------------
CSinkPin::CSinkPin(CSinkFilter* owner)
    : m_ref(1), m_owner(owner), m_connected(NULL), m_hasMt(false), m_count(0)
{
    ZeroMemory(&m_mt, sizeof(m_mt));
}

CSinkPin::~CSinkPin() { Close(); }

void CSinkPin::Close()
{
    if (m_connected) { m_connected->Release(); m_connected = NULL; }
    if (m_hasMt)
    {
        if (m_mt.cbFormat && m_mt.pbFormat) CoTaskMemFree(m_mt.pbFormat);
        m_mt.cbFormat = 0; m_mt.pbFormat = NULL; m_hasMt = false;
    }
}

HRESULT CSinkPin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IPin)
    { *ppv = (IPin*)this; AddRef(); return S_OK; }
    if (riid == IID_IMemInputPin)
    { *ppv = (IMemInputPin*)this; AddRef(); return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}

HRESULT CSinkPin::Connect(IPin*, const AM_MEDIA_TYPE*) { return VFW_E_ALREADY_CONNECTED; }

HRESULT CSinkPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt)
{
    if (!pConnector || !pmt) return E_POINTER;
    if (m_connected) return VFW_E_ALREADY_CONNECTED;
    HRESULT ha = QueryAccept(pmt);
    if (ha != S_OK)
    {
        VLog("Sink::ReceiveConnection rejected maj=%08X sub=%08X",
             (unsigned)pmt->majortype.Data1, (unsigned)pmt->subtype.Data1);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    Close();
    CopyMediaType(&m_mt, pmt);
    m_hasMt = true;
    m_connected = pConnector;
    m_connected->AddRef();

    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)m_mt.pbFormat;
    if (vih)
    {
        EnterCriticalSection(&g_sf.cs);
        g_sf.w = (int)vih->bmiHeader.biWidth;
        g_sf.h = abs((int)vih->bmiHeader.biHeight);
        g_sf.subtype = m_mt.subtype;
        g_flipY = g_cfgFlipY;
        g_tblFlip = -1;   // 强制重建映射表
        LeaveCriticalSection(&g_sf.cs);
    }
    VLog("Sink::ReceiveConnection ok %dx%d biH=%ld sub=%08X",
         vih ? (int)vih->bmiHeader.biWidth : 0, vih ? (int)vih->bmiHeader.biHeight : 0,
         vih ? (long)vih->bmiHeader.biHeight : 0,
         (unsigned)m_mt.subtype.Data1);
    return S_OK;
}

HRESULT CSinkPin::Disconnect() { Close(); return S_OK; }

HRESULT CSinkPin::ConnectedTo(IPin** ppPin)
{
    if (!ppPin) return E_POINTER;
    if (!m_connected) { *ppPin = NULL; return VFW_E_NOT_CONNECTED; }
    *ppPin = m_connected;
    m_connected->AddRef();
    return S_OK;
}

HRESULT CSinkPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    if (!m_hasMt) return VFW_E_NOT_CONNECTED;
    CopyMediaType(pmt, &m_mt);
    return S_OK;
}

HRESULT CSinkPin::QueryPinInfo(PIN_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = m_owner;
    if (m_owner) m_owner->AddRef();
    pInfo->dir = PINDIR_INPUT;
    wcscpy(pInfo->achName, L"In");
    return S_OK;
}

HRESULT CSinkPin::QueryDirection(PIN_DIRECTION* d) { if (!d) return E_POINTER; *d = PINDIR_INPUT; return S_OK; }

HRESULT CSinkPin::QueryId(LPWSTR* Id)
{
    if (!Id) return E_POINTER;
    const WCHAR s[] = L"In";
    *Id = (LPWSTR)CoTaskMemAlloc(sizeof(s));
    if (!*Id) return E_OUTOFMEMORY;
    memcpy(*Id, s, sizeof(s));
    return S_OK;
}

HRESULT CSinkPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;
    if (pmt->subtype != MEDIASUBTYPE_YUY2 && pmt->subtype != MEDIASUBTYPE_RGB24 &&
        pmt->subtype != MEDIASUBTYPE_NV12)
        return S_FALSE;
    if (pmt->formattype != FORMAT_VideoInfo || !pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return S_FALSE;
    const BITMAPINFOHEADER& b=((VIDEOINFOHEADER*)pmt->pbFormat)->bmiHeader;
    if(b.biWidth < 16 || b.biWidth > MAX_W || b.biHeight < -MAX_H || b.biHeight > MAX_H || abs(b.biHeight)<16 || (b.biWidth&1) || (b.biHeight&1)) return S_FALSE;
    return S_OK;
}

HRESULT CSinkPin::EnumMediaTypes(IEnumMediaTypes** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = NULL;
    return E_NOTIMPL;   // 上游负责提供格式
}

HRESULT CSinkPin::QueryInternalConnections(IPin** apPin, ULONG* nPin)
{
    if (nPin) *nPin = 0;
    return E_NOTIMPL;
}

HRESULT CSinkPin::EndOfStream() { return S_OK; }
HRESULT CSinkPin::BeginFlush()  { return S_OK; }
HRESULT CSinkPin::EndFlush()    { return S_OK; }
HRESULT CSinkPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

// 让上游（UVC 驱动）自己创建分配器 —— 这是采集源能正常出流的关键
HRESULT CSinkPin::GetAllocator(IMemAllocator** ppAllocator)
{
    if (ppAllocator) *ppAllocator = NULL;
    return E_NOTIMPL;
}

HRESULT CSinkPin::NotifyAllocator(IMemAllocator* a, BOOL)
{
    if (a)
    {
        ALLOCATOR_PROPERTIES gt;
        ZeroMemory(&gt, sizeof(gt));
        HRESULT hr = a->GetProperties(&gt);
        VLog("Sink::NotifyAllocator hr=0x%08X bufs=%ld size=%ld align=%ld",
             (unsigned)hr, gt.cBuffers, gt.cbBuffer, gt.cbAlign);
    }
    return S_OK;
}

HRESULT CSinkPin::GetAllocatorRequirements(ALLOCATOR_PROPERTIES* p)
{
    if(!p) return E_POINTER;
    ZeroMemory(p,sizeof(*p));
    if(!m_hasMt || !m_mt.pbFormat) return E_NOTIMPL;
    VIDEOINFOHEADER* v=(VIDEOINFOHEADER*)m_mt.pbFormat;
    long pixels=v->bmiHeader.biWidth*abs(v->bmiHeader.biHeight);
    p->cBuffers=4; p->cbAlign=1;
    p->cbBuffer=m_mt.subtype==MEDIASUBTYPE_NV12 ? pixels*3/2 : pixels*(m_mt.subtype==MEDIASUBTYPE_RGB24?3:2);
    return S_OK;
}

HRESULT CSinkPin::Receive(IMediaSample* pSample)
{
    if (!pSample) return E_POINTER;
    BYTE* p = NULL;
    if (FAILED(pSample->GetPointer(&p)) || !p) return S_OK;
    long len = (long)pSample->GetActualDataLength();

    LONG n = InterlockedIncrement(&m_count);
    if (n <= 3 || (n % 60) == 0) VLog("Sink::Receive n=%ld len=%ld ptr=%p", n, len, p);
    if (len <= 0) return S_OK;

    // 调试：把第 5 帧的原始缓冲落盘，用于判定驱动的真实平面顺序/行序
    if (n == 20)
    {
        const char* dp = getenv("VCAM_DUMP_IN");
        if (dp && *dp)
        {
            FILE* fp = fopen(dp, "wb");
            if (fp) { fwrite(p, 1, len, fp); fclose(fp); }
        }
    }

    {
        static LONG s_dbg = 0;
        if (InterlockedIncrement(&s_dbg) == 1)
            VLog("Sink::DBG buf=%p cap=%ld len=%ld", g_sf.buf, g_sf.cap, len);
    }
    EnterCriticalSection(&g_sf.cs);
    long expected = g_sf.w*g_sf.h*(g_sf.subtype == MEDIASUBTYPE_RGB24 ? 3 : 2);
    if(g_sf.subtype == MEDIASUBTYPE_NV12) expected=g_sf.w*g_sf.h*3/2;
    if (g_sf.buf && len == expected && len <= g_sf.cap)
    {
        memcpy(g_sf.buf, p, len);
        g_sf.len = len;
        g_sf.ready = 1;
        InterlockedIncrement(&g_sf.seq);
    }
    LeaveCriticalSection(&g_sf.cs);
    if (g_grabEvent) SetEvent(g_grabEvent);
    return S_OK;
}

HRESULT CSinkPin::ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nProcessed)
{
    if (nProcessed) *nProcessed = 0;
    if (!pSamples) return E_POINTER;
    for (long i = 0; i < nSamples; i++)
    {
        Receive(pSamples[i]);
        if (nProcessed) (*nProcessed)++;
    }
    return S_OK;
}

HRESULT CSinkPin::ReceiveCanBlock() { return S_OK; }

// pick a 640x480 / 30 fps format on the real camera pin
// 该机 UVC 驱动内部按 NV12 分配缓冲区，YUY2 会导致"分配 460800 却按 614400 投递"，
// 结果只投一个空样本就停流。默认选 NV12；VCAM_FMT=yuy2 可强制回 YUY2 做对照。
static GUID PreferredSub()
{
    const char* e = getenv("VCAM_FMT");
    if (e && (e[0] == 'y' || e[0] == 'Y')) return MEDIASUBTYPE_YUY2;
    return MEDIASUBTYPE_NV12;
}

static bool ConfigureCameraFormat(IPin* pin, int wantW, int wantH)
{
    const char* skip = getenv("VCAM_NOFMT");
    if (skip && skip[0] == '1') { VLog("ConfigureCameraFormat: skipped (use camera default)"); return false; }

    IAMStreamConfig* sc = NULL;
    if (FAILED(pin->QueryInterface(IID_IAMStreamConfig, (void**)&sc))) return false;
    int n = 0, sz = 0;
    if (FAILED(sc->GetNumberOfCapabilities(&n, &sz)) || n <= 0 || sz <= 0 || sz > 65536) { sc->Release(); return false; }
    BYTE* caps = (BYTE*)malloc(sz);
    if(!caps){sc->Release();return false;}
    GUID want = PreferredSub();
    AM_MEDIA_TYPE* best = NULL;
    for (int i = 0; i < n; i++)
    {
        AM_MEDIA_TYPE* pmt = NULL;
        if (FAILED(sc->GetStreamCaps(i, &pmt, caps))) continue;
        VIDEOINFOHEADER* vih = (pmt && pmt->formattype==FORMAT_VideoInfo && pmt->cbFormat>=sizeof(VIDEOINFOHEADER)) ? (VIDEOINFOHEADER*)pmt->pbFormat : NULL;
        if (vih) VLog("CamCap %d: %dx%d fps=%d sub=%08X", i,
                      (int)vih->bmiHeader.biWidth, abs((int)vih->bmiHeader.biHeight),
                      vih->AvgTimePerFrame ? (int)(10000000LL / vih->AvgTimePerFrame) : 0,
                      (unsigned)pmt->subtype.Data1);
        if (vih && pmt->subtype == want && pmt->formattype == FORMAT_VideoInfo &&
            (int)vih->bmiHeader.biWidth == wantW &&
            abs((int)vih->bmiHeader.biHeight) == wantH)
        {
            best = pmt;
            break;
        }
        FreeMediaType(pmt);
        CoTaskMemFree(pmt);
    }
    if (caps) free(caps);
    // 探测驱动是否支持 Zoom（有些 Boot Camp 驱动默认带数码变焦，视野会变窄）
    {
        IAMCameraControl* cc = NULL;
        if (SUCCEEDED(pin->QueryInterface(IID_IAMCameraControl, (void**)&cc)) && cc)
        {
            long mn = 0, mx = 0, stp = 0, def = 0, cur = 0;
            HRESULT hg = cc->GetRange(CameraControl_Zoom, &mn, &mx, &stp, &def, NULL);
            HRESULT hc2 = cc->Get(CameraControl_Zoom, &cur, NULL);
            VLog("CamCtrl Zoom: range=%ld..%ld step=%ld def=%ld cur=%ld (hr=0x%08X/0x%08X)",
                 mn, mx, stp, def, cur, (unsigned)hg, (unsigned)hc2);
            if (g_cfgZoom >= 0 && SUCCEEDED(hg))
            {
                long v = g_cfgZoom;
                if (v < mn) v = mn;
                if (v > mx) v = mx;
                HRESULT hs = cc->Set(CameraControl_Zoom, v, VideoProcAmp_Flags_Manual);
                VLog("CamCtrl Zoom: Set(%ld) -> 0x%08X", v, (unsigned)hs);
            }
            cc->Release();
        }
        else VLog("CamCtrl: IAMCameraControl not supported");
    }

    if (best && g_cfgSrcFps > 0 && best->pbFormat)
    {
        VIDEOINFOHEADER* vh = (VIDEOINFOHEADER*)best->pbFormat;
        vh->AvgTimePerFrame = 10000000LL / g_cfgSrcFps;
        VLog("ConfigureCameraFormat: force fps=%d (%lld x100ns) for %dx%d",
             g_cfgSrcFps, (long long)vh->AvgTimePerFrame, wantW, wantH);
    }
    bool configured=false;
    if (best)
    {
        HRESULT hr = sc->SetFormat(best);
        configured=SUCCEEDED(hr);
        VLog("ConfigureCameraFormat: SetFormat sub=%08X -> 0x%08X", (unsigned)want.Data1, (unsigned)hr);
        FreeMediaType(best);
        CoTaskMemFree(best);
    }
    else VLog("ConfigureCameraFormat: no %dx%d cap for sub=%08X", wantW, wantH, (unsigned)want.Data1);
    sc->Release();
    return configured;
}

static bool BuildCaptureGraph(CaptureCtx* ctx, const wchar_t* preferredName, int wantW, int wantH)
{
    ZeroMemory(ctx, sizeof(*ctx));

    if (FAILED(CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER,
                                IID_IGraphBuilder, (void**)&ctx->graph)))
        return false;
    ctx->graph->QueryInterface(IID_IMediaControl, (void**)&ctx->control);

    ICreateDevEnum* devEnum = NULL;
    IEnumMoniker*   enumMon = NULL;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER,
                                IID_ICreateDevEnum, (void**)&devEnum)))
        return false;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0) != S_OK)
    {
        devEnum->Release();
        return false;   // no camera at all
    }

    IMoniker* mon = NULL;
    IMoniker* chosen = NULL;
    bool exact = false;
    while (enumMon->Next(1, &mon, NULL) == S_OK)
    {
        bool skip = false;
        IPropertyBag* bag = NULL;
        if (SUCCEEDED(mon->BindToStorage(NULL, NULL, IID_IPropertyBag, (void**)&bag)) && bag)
        {
            VARIANT v; VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, NULL)) && v.bstrVal)
            {
                // never pick ourselves
                if (wcsstr(v.bstrVal, L"Virtual Camera") != NULL) skip = true;
                if (!skip && preferredName && wcscmp(v.bstrVal, preferredName) == 0)
                    exact = true;
            }
            VariantClear(&v);
            bag->Release();
        }
        if (!skip && (exact || !chosen))
        {
            if (chosen) chosen->Release();
            chosen = mon;
        }
        else
        {
            mon->Release();
        }
        if (exact) break;
    }
    enumMon->Release();
    devEnum->Release();

    if (!chosen || !exact) { if(chosen) chosen->Release(); return false; }

    if (FAILED(chosen->BindToObject(NULL, NULL, IID_IBaseFilter, (void**)&ctx->cam)))
    {
        chosen->Release();
        return false;
    }
    chosen->Release();
    ctx->graph->AddFilter(ctx->cam, L"RealCamera");

    IPin* camOut = FindCapturePin(ctx->cam);
    if (!camOut) { VLog("BuildCaptureGraph: no capture pin"); return false; }
    if(!ConfigureCameraFormat(camOut, wantW, wantH)){camOut->Release();return false;}

    // 关键：驱动有时会按上一个格式的尺寸分配缓冲区（480p 的 460800），
    // 换成 720p 后仍按 460800 喂 1382400 的数据 → 只投递 1 个空样本就停流。
    // 这里显式协商分配器尺寸。
    {
        IAMBufferNegotiation* bn = NULL;
        if (SUCCEEDED(camOut->QueryInterface(IID_IAMBufferNegotiation, (void**)&bn)) && bn)
        {
            ALLOCATOR_PROPERTIES rq;
            ZeroMemory(&rq, sizeof(rq));
            rq.cBuffers = 8;
            rq.cbBuffer = (long)wantW * wantH * 3 / 2;   // NV12
            rq.cbAlign  = 1;
            rq.cbPrefix = 0;
            HRESULT hb = bn->SuggestAllocatorProperties(&rq);
            VLog("BuildCaptureGraph: SuggestAllocator(%ld) = 0x%08X", rq.cbBuffer, (unsigned)hb);
            bn->Release();
        }
        else VLog("BuildCaptureGraph: IAMBufferNegotiation unsupported");
    }

    ctx->sinkF = new CSinkFilter();
    if (!ctx->sinkF) { camOut->Release(); return false; }
    ctx->graph->AddFilter(ctx->sinkF, L"VcamSink");

    IAMStreamConfig* lockedSc=NULL; AM_MEDIA_TYPE* lockedMt=NULL;
    HRESULT hc=E_FAIL;
    if(SUCCEEDED(camOut->QueryInterface(IID_IAMStreamConfig,(void**)&lockedSc))){
        if(SUCCEEDED(lockedSc->GetFormat(&lockedMt)) && lockedMt)
            hc=ctx->graph->ConnectDirect(camOut,ctx->sinkF->Pin(),lockedMt);
        if(lockedMt){FreeMediaType(lockedMt);CoTaskMemFree(lockedMt);}lockedSc->Release();
    }
    VLog("BuildCaptureGraph: connect cam->sink=0x%08X", (unsigned)hc);
    camOut->Release();
    if (FAILED(hc)) return false;
    if(g_sf.w != wantW || g_sf.h != wantH || g_sf.subtype != PreferredSub()) return false;

    LONG seqBefore = g_sf.seq;
    VLog("BuildCaptureGraph: Run()=0x%08X", (unsigned)ctx->control->Run());

    // 首帧探测：T2 上相机启动要 1.5~3s，给足 3.5s 再判定这个尺寸不可用
    for (int i = 0; i < 350; i++)
    {
        MSG m2;
        while (PeekMessageW(&m2, NULL, 0, 0, PM_REMOVE))
        { TranslateMessage(&m2); DispatchMessageW(&m2); }
        if (g_sf.seq != seqBefore) { VLog("BuildCaptureGraph: %dx%d first frame ok", wantW, wantH); return true; }
        Sleep(10);
    }
    VLog("BuildCaptureGraph: %dx%d got no frame -> give up", wantW, wantH);
    return false;
}

static void TearDownCapture(CaptureCtx* ctx)
{
    EnterCriticalSection(&g_sf.cs); g_sf.ready=0; g_sf.len=0; LeaveCriticalSection(&g_sf.cs);
    if (ctx->control) { ctx->control->Stop(); ctx->control->Release(); }
    if (ctx->sinkF)   ctx->sinkF->Release();
    if (ctx->cam)     ctx->cam->Release();
    if (ctx->graph)   ctx->graph->Release();
    ZeroMemory(ctx, sizeof(*ctx));
}

static CaptureCtx g_cap;
static HANDLE     g_capThread = NULL;
static volatile LONG g_capRun = 0;

static void PumpOnce()
{
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

static DWORD WINAPI CaptureThread(LPVOID)
{
    // 必须用 STA + 消息泵：相机滤镜会在创建线程上建隐藏窗口收发消息
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    VLog("CaptureThread: start");

    bool useFallback=false;
    while (g_capRun)
    {
        int w = useFallback ? 640 : (g_reqW > 0 ? g_reqW : 1280);
        int h = useFallback ? 480 : (g_reqH > 0 ? g_reqH : 720);
        bool built = BuildCaptureGraph(&g_cap, L"FaceTime HD Camera (Built-in)", w, h);
        if(!built && (w!=640 || h!=480)) {
            TearDownCapture(&g_cap);useFallback=true;w=640;h=480;
            VLog("CaptureThread: native HD unavailable, trying 640x480");
            built=BuildCaptureGraph(&g_cap,L"FaceTime HD Camera (Built-in)",w,h);
        }
        if (built) VLog("CaptureThread: capture source = %dx%d", w, h);
        else       TearDownCapture(&g_cap);

        if (!built)
        {
            // 相机可能被别的程序占用，等一会儿再试
            for (int i = 0; i < 150 && g_capRun; i++)   // ~1.5s
            { PumpOnce(); Sleep(10); }
            continue;
        }

        // 运行中监视帧计数，长时间无新帧就重建整条采集链
        LONG lastSeq = g_sf.seq;
        DWORD lastChange = GetTickCount();
        while (g_capRun)
        {
            PumpOnce();
            Sleep(10);
            if (g_sf.seq != lastSeq)
            {
                lastSeq = g_sf.seq;
                lastChange = GetTickCount();
            }
            else if (GetTickCount() - lastChange > 5000)
            {
                VLog("CaptureThread: no new frame for 5s -> rebuild");
                break;
            }
        }
        TearDownCapture(&g_cap);
    }

    CoUninitialize();
    return 0;
}

static void StartCapture()
{
    if (g_capRun) return;
    SharedInit();
    g_capRun = 1;
    g_capThread = CreateThread(NULL, 0, CaptureThread, NULL, 0, NULL);
}

static void StopCapture()
{
    if (!g_capRun) return;
    g_capRun = 0;
    if (g_capThread)
    {
        WaitForSingleObject(g_capThread, 5000);
        CloseHandle(g_capThread);
        g_capThread = NULL;
    }
    EnterCriticalSection(&g_sf.cs);
    g_sf.ready = 0;
    LeaveCriticalSection(&g_sf.cs);
    ResetEvent(g_grabEvent);
}

// NV12 源 -> NV12(交织 UV) 或 I420(平面 UV)，最近邻缩放
static void NV12ToNV12(const BYTE* src, int sw, int sh, BYTE* dst, int dw, int dh, bool planar)
{
    unsigned char* scaled = NULL;
    if (g_cfgQuality && (sw != dw || sh != dh)) {
        scaled=(unsigned char*)malloc((size_t)dw*dh*3/2);
        if(scaled && ScaleNV12(src,sw,sh,scaled,dw,dh)) {
            src=scaled; sw=dw; sh=dh;
        }
    }

    const BYTE* sy  = src;
    const BYTE* suv = src + (long)sw * sh;

    // Y 平面
    for (int y = 0; y < dh; y++)
    {
        int srow = (int)((long)y * sh / dh);
        if (g_flipY) srow = sh - 1 - srow;
        const BYTE* s = sy + (long)srow * sw;
        BYTE* d = dst + (long)y * dw;
        long acc = 0, step = ((long)sw << 16) / dw;
        for (int x = 0; x < dw; x++) { d[x] = s[acc >> 16]; acc += step; }
    }

    int cw = dw / 2, ch = dh / 2;
    int scw = sw / 2, sch = sh / 2;
    if (cw < 1 || ch < 1 || scw < 1 || sch < 1) { free(scaled); return; }

    if (!planar)
    {
        BYTE* duv = dst + (long)dw * dh;
        for (int cy = 0; cy < ch; cy++)
        {
            int scy = (int)((long)cy * sch / ch);
            if (g_flipY) scy = sch - 1 - scy;
            const BYTE* s = suv + (long)scy * sw;   // UV 行：每行 sw 字节
            BYTE* d = duv + (long)cy * dw;
            long acc = 0, step = ((long)scw << 16) / cw;
            for (int cx = 0; cx < cw; cx++)
            {
                int so = (acc >> 16) * 2;
                d[cx * 2]     = s[so + (g_cfgSwapUV ? 1 : 0)];       // U
                d[cx * 2 + 1] = s[so + (g_cfgSwapUV ? 0 : 1)];   // V
                acc += step;
            }
        }
    }
    else
    {
        BYTE* du = dst + (long)dw * dh;
        BYTE* dv = du + (long)cw * ch;
        for (int cy = 0; cy < ch; cy++)
        {
            int scy = (int)((long)cy * sch / ch);
            if (g_flipY) scy = sch - 1 - scy;
            const BYTE* s = suv + (long)scy * sw;
            BYTE* pu = du + (long)cy * cw;
            BYTE* pv = dv + (long)cy * cw;
            long acc = 0, step = ((long)scw << 16) / cw;
            for (int cx = 0; cx < cw; cx++)
            {
                int so = (acc >> 16) * 2;
                pu[cx] = s[so + (g_cfgSwapUV ? 1 : 0)];
                pv[cx] = s[so + (g_cfgSwapUV ? 0 : 1)];
                acc += step;
            }
        }
    }
    free(scaled);
}

// ---------------------------------------------------------------------------
// media types we publish
// ---------------------------------------------------------------------------
struct FmtEntry { int w, h, fps; };
static const FmtEntry g_fmts[] = {
    {  640,  480, 30 }, {  640,  480, 15 },
    { 1280,  720, 30 }, { 1280,  720, 15 },
    { 1920, 1080, 30 }, { 1920, 1080, 15 },
};
static const int g_fmtCount = sizeof(g_fmts) / sizeof(g_fmts[0]);

// I420 在 mingw 头文件里由 DEFINE_GUID 导出，不能直接用，这里自己定义
static const GUID MEDIASUBTYPE_I420X =
    { 0x30323449, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

// 回到 v1 能力表形态：RGB24 / YUY2 交替，共 12 项，RGB24 排第一。
// （v1 是唯一在微信里成功出画的一版，先保持接口面一致，再谈增强。）
static const GUID g_subs[2] = { MEDIASUBTYPE_RGB24, MEDIASUBTYPE_YUY2 };
static const int  g_subCount = 2;

static int CapsCount() { return g_fmtCount * g_subCount; }

// 每帧字节数
static long FrameBytes(int w, int h, const GUID& sub)
{
    if (sub == MEDIASUBTYPE_YUY2) return (long)w * h * 2;
    if (sub == MEDIASUBTYPE_NV12 || sub == MEDIASUBTYPE_I420X) return (long)w * h * 3 / 2;
    return (long)w * h * 3;
}

static bool IsYuvSub(const GUID& sub)
{
    return sub == MEDIASUBTYPE_NV12 || sub == MEDIASUBTYPE_I420X || sub == MEDIASUBTYPE_YUY2;
}

// 索引 -> (格式, 子类型)
static void CapsIdx(int idx, FmtEntry* pf, GUID* ps)
{
    *pf = g_fmts[idx / g_subCount];
    *ps = g_subs[idx % g_subCount];
}

static void FillMediaType(const FmtEntry& f, const GUID& sub, AM_MEDIA_TYPE* pmt)
{
    ZeroMemory(pmt, sizeof(*pmt));
    pmt->majortype  = MEDIATYPE_Video;
    pmt->subtype    = sub;
    pmt->bFixedSizeSamples    = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->lSampleSize = FrameBytes(f.w, f.h, sub);
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pbFormat = (BYTE*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!pmt->pbFormat) { pmt->cbFormat = 0; return; }
    ZeroMemory(pmt->pbFormat, sizeof(VIDEOINFOHEADER));
    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
    vih->AvgTimePerFrame = 10000000LL / f.fps;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = f.w;
    vih->bmiHeader.biHeight = g_cfgTopDown ? -(LONG)f.h : (LONG)f.h;
    vih->bmiHeader.biPlanes = 1;
    if (sub == MEDIASUBTYPE_YUY2)
    { vih->bmiHeader.biBitCount = 16; vih->bmiHeader.biCompression = MAKEFOURCC('Y','U','Y','2'); }
    else if (sub == MEDIASUBTYPE_NV12)
    { vih->bmiHeader.biBitCount = 12; vih->bmiHeader.biCompression = MAKEFOURCC('N','V','1','2'); }
    else if (sub == MEDIASUBTYPE_I420X)
    { vih->bmiHeader.biBitCount = 12; vih->bmiHeader.biCompression = MAKEFOURCC('I','4','2','0'); }
    else
    { vih->bmiHeader.biBitCount = 24; vih->bmiHeader.biCompression = BI_RGB; }
    vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    vih->rcTarget.right = f.w;
    vih->rcTarget.bottom = f.h;
}

// mingw 头文件没有 VIDEOINFOHEADER2，这里按 Windows 定义镜像一份
struct VIH2
{
    RECT            rcSource;
    RECT            rcTarget;
    DWORD           dwBitRate;
    DWORD           dwBitErrorRate;
    REFERENCE_TIME  AvgTimePerFrame;
    DWORD           dwInterlaceFlags;
    DWORD           dwCopyProtectFlags;
    DWORD           dwPictAspectRatioX;
    DWORD           dwPictAspectRatioY;
    DWORD           dwControlFlags;
    DWORD           dwReserved1;
    DWORD           dwReserved2;
    BITMAPINFOHEADER bmiHeader;
};

static bool ParseMediaType(const AM_MEDIA_TYPE* pmt, int* pw, int* ph, int* pfps, GUID* psub)
{
    if (!pmt || pmt->majortype != MEDIATYPE_Video) return false;
    if (!pmt->pbFormat) return false;

    const BITMAPINFOHEADER* bmi = NULL;
    REFERENCE_TIME avg = 0;
    if (pmt->formattype == FORMAT_VideoInfo)
    {
        if(pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return false;
        const VIDEOINFOHEADER* vih = (const VIDEOINFOHEADER*)pmt->pbFormat;
        bmi = &vih->bmiHeader; avg = vih->AvgTimePerFrame;
    }
    else if (pmt->formattype == FORMAT_VideoInfo2)
    {
        if(pmt->cbFormat < sizeof(VIH2)) return false;
        const VIH2* vih2 = (const VIH2*)pmt->pbFormat;
        bmi = &vih2->bmiHeader; avg = vih2->AvgTimePerFrame;
    }
    else return false;

    // 只认 4 种我们能输出的亚型；其余一律拒绝
    if (pmt->subtype != MEDIASUBTYPE_RGB24 && pmt->subtype != MEDIASUBTYPE_YUY2 &&
        pmt->subtype != MEDIASUBTYPE_NV12  && pmt->subtype != MEDIASUBTYPE_I420X)
        return false;

    int w = (int)bmi->biWidth;
    if(bmi->biHeight < -1080 || bmi->biHeight > 1080) return false;
    int h = abs((int)bmi->biHeight);
    // 放宽上限到 1920x1080，并强制偶数（YUV 色度按 2x2 采样）
    if (w < 16 || h < 16 || w > 1920 || h > 1080) return false;
    if ((w & 1) || (h & 1)) return false;
    if (pmt->subtype == MEDIASUBTYPE_RGB24 && (w & 3)) return false;
    if (avg < 0) return false;
    int fps = avg ? (int)(10000000LL / avg) : 30;
    if (fps < 1) fps = 1;
    if (fps > 60) fps = 60;
    if (pw) *pw = w;
    if (ph) *ph = h;
    if (pfps) *pfps = fps;
    if (psub) *psub = pmt->subtype;
    return true;
}

// ---------------------------------------------------------------------------
// enumerators
// ---------------------------------------------------------------------------
class CEnumMediaTypes : public IEnumMediaTypes
{
    LONG m_ref;
    int  m_pos;
public:
    CEnumMediaTypes() : m_ref(1), m_pos(0) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes)
        { *ppv = (IEnumMediaTypes*)this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, AM_MEDIA_TYPE** pp, ULONG* fetched)
    {
        ULONG i = 0;
        for (; i < c; i++)
        {
            int idx = m_pos + (int)i;
            if (idx >= CapsCount()) break;
            AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
            if (!pmt) break;
            FmtEntry fe; GUID se;
            CapsIdx(idx, &fe, &se);
            FillMediaType(fe, se, pmt);
            pp[i] = pmt;
        }
        m_pos += (int)i;
        if (fetched) *fetched = i;
        return (i == c) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG c) { m_pos += (int)c; return S_OK; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** pp)
    {
        if (!pp) return E_POINTER;
        CEnumMediaTypes* e = new CEnumMediaTypes();
        if (!e) return E_OUTOFMEMORY;
        e->m_pos = m_pos;
        *pp = e;
        return S_OK;
    }
};

class CVcamPin;
class CEnumPins : public IEnumPins
{
    LONG m_ref;
    int  m_pos;
    CVcamPin* m_pin;
public:
    CEnumPins(CVcamPin* pin) : m_ref(1), m_pos(0), m_pin(pin) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG c, IPin** pp, ULONG* fetched);
    STDMETHODIMP Skip(ULONG c) { m_pos += (int)c; return S_OK; }
    STDMETHODIMP Reset() { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** pp)
    {
        if (!pp) return E_POINTER;
        CEnumPins* e = new CEnumPins(m_pin);
        if (!e) return E_OUTOFMEMORY;
        e->m_pos = m_pos;
        *pp = e;
        return S_OK;
    }
};

// ---------------------------------------------------------------------------
// out pin
// ---------------------------------------------------------------------------
class CVcam;
// ---------------------------------------------------------------------------
// IKsPropertySet —— 必须实现，否则 ffmpeg / 各类采集程序认不出这是视频采集引脚
// （它们读 AMPROPERTY_PIN_CATEGORY，要求等于 PIN_CATEGORY_CAPTURE）
// ---------------------------------------------------------------------------
#ifndef E_PROP_SET_UNSUPPORTED
#define E_PROP_SET_UNSUPPORTED ((HRESULT)0x80070492L)
#endif
#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED  ((HRESULT)0x80070490L)
#endif

class CPinKsProps : public IKsPropertySet
{
    LONG m_ref;
public:
    CPinKsProps() : m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IKsPropertySetX)
        { *ppv = (IKsPropertySet*)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&m_ref); } // 归引脚所有

    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD)
    {
        return E_PROP_SET_UNSUPPORTED;
    }

    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID,
                     LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned)
    {
        VLog("KsProps::Get set=%08X id=%d cb=%d", (unsigned)guidPropSet.Data1, (int)dwPropID, (int)cbPropData);
        if (!IsEqualGUID(guidPropSet, AMPROPSETID_PinX)) return E_PROP_SET_UNSUPPORTED;
        if (dwPropID != 0 /* AMPROPERTY_PIN_CATEGORY */)  return E_PROP_ID_UNSUPPORTED;
        if (pcbReturned) *pcbReturned = sizeof(GUID);
        if (!pPropData) return S_OK;
        if (cbPropData < sizeof(GUID)) return E_PROP_SET_UNSUPPORTED;
        *(GUID*)pPropData = PIN_CATEGORY_CAPTUREX;
        return S_OK;
    }

    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport)
    {
        if (guidPropSet == AMPROPSETID_PinX && dwPropID == 0)
        {
            if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
            return S_OK;
        }
        return E_PROP_SET_UNSUPPORTED;
    }
};

class CVcamPin : public IPin, public IAMStreamConfig
{
    LONG      m_ref;
    CVcam*    m_owner;
    IPin*     m_connected;
    IMemInputPin* m_memInput;
    IMemAllocator* m_alloc;
    CPinKsProps*  m_ks;
    int       m_w, m_h, m_fps;
    GUID      m_sub;
    BYTE*     m_outBuf;
    long      m_outLen;
    HANDLE    m_thread;
    volatile LONG m_running;

    DWORD ThreadProc();
    static DWORD WINAPI ThreadEntry(LPVOID p)
    { return ((CVcamPin*)p)->ThreadProc(); }
    void FillOutput(BYTE* dst);

public:
    CVcamPin(CVcam* owner);
    ~CVcamPin();

    void StartDelivery();
    void StopDelivery();
    void ReleaseConnections();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP Disconnect();
    STDMETHODIMP ConnectedTo(IPin** ppPin);
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo);
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir);
    STDMETHODIMP QueryId(LPWSTR* Id);
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt);
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum);
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin);
    STDMETHODIMP EndOfStream();
    STDMETHODIMP BeginFlush();
    STDMETHODIMP EndFlush();
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double);

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt);
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt);
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize);
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC);
};

// ---------------------------------------------------------------------------
// IAMFilterMiscFlags 辅助对象（单独 vtable，不能和 IBaseFilter 共用）
// ---------------------------------------------------------------------------
class CMiscFlags : public IAMFilterMiscFlags
{
    LONG m_ref;
public:
    CMiscFlags() : m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IAMFilterMiscFlags)
        { *ppv = (IAMFilterMiscFlags*)this; AddRef(); return S_OK; }
        *ppv = NULL;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release() { return InterlockedDecrement(&m_ref); } // 单例，永不销毁
    STDMETHODIMP_(ULONG) GetMiscFlags() { return AM_FILTER_MISC_FLAGS_IS_SOURCE; }
};
static CMiscFlags g_miscFlags;

// ---------------------------------------------------------------------------
// the filter
// ---------------------------------------------------------------------------
class CVcam : public IBaseFilter
{
    LONG  m_ref;
    IFilterGraph* m_graph;
    CVcamPin*     m_pin;
    FILTER_STATE  m_state;
    IReferenceClock* m_clock;

public:
    CVcam();
    IReferenceClock* Clock() { return m_clock; }
    ~CVcam();
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv);
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClsID);

    // IMediaFilter
    STDMETHODIMP Stop();
    STDMETHODIMP Pause();
    STDMETHODIMP Run(REFERENCE_TIME tStart);
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State);
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock);
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock);

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum);
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin);
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo);
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName);
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo);
};

// ===========================================================================
// CVcamPin implementation
// ===========================================================================
CVcamPin::CVcamPin(CVcam* owner)
    : m_ref(1), m_owner(owner), m_connected(NULL), m_memInput(NULL), m_alloc(NULL),
      m_ks(new CPinKsProps()),
      m_w(1280), m_h(720), m_fps(30), m_sub(MEDIASUBTYPE_RGB24),
      m_outBuf(NULL), m_outLen(0), m_thread(NULL), m_running(0)
{
}

CVcamPin::~CVcamPin()
{
    StopDelivery();
    ReleaseConnections();
    if (m_outBuf) VirtualFree(m_outBuf, 0, MEM_RELEASE);
    if (m_ks) { delete m_ks; m_ks = NULL; }
}

void CVcamPin::ReleaseConnections()
{
    if (m_alloc)     { m_alloc->Release();     m_alloc = NULL; }
    if (m_memInput)  { m_memInput->Release();  m_memInput = NULL; }
    if (m_connected) { m_connected->Release(); m_connected = NULL; }
}

HRESULT CVcamPin::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    VLog("Pin::QI riid=%08X-%04X-%04X", (unsigned)riid.Data1, riid.Data2, riid.Data3);
    if (riid == IID_IUnknown || riid == IID_IPin)
        *ppv = (IPin*)this;
    else if (riid == IID_IAMStreamConfig)
        *ppv = (IAMStreamConfig*)this;
    else if (riid == IID_IKsPropertySetX && m_ks)
    {
        *ppv = (IKsPropertySet*)m_ks;
        m_ks->AddRef();
        return S_OK;
    }
    else
    { *ppv = NULL; return E_NOINTERFACE; }
    AddRef();
    return S_OK;
}

void CVcamPin::FillOutput(BYTE* dst)
{
    // 调试用：设置环境变量 VCAM_DUMP=<路径> 时，把第 20 帧写到文件
    static LONG s_dumpCount = 0;

    static BYTE* snap = NULL;
    static long  snapCap = 0;
    if (!snap || snapCap < g_sf.cap)
    {
        if (snap) VirtualFree(snap, 0, MEM_RELEASE);
        snap = (BYTE*)VirtualAlloc(NULL, g_sf.cap, MEM_COMMIT, PAGE_READWRITE);
        snapCap = g_sf.cap;
    }

    EnterCriticalSection(&g_sf.cs);
    long  len = g_sf.len;
    int   sw = g_sf.w, sh = g_sf.h;
    GUID  sub = g_sf.subtype;
    bool  ready = g_sf.ready != 0;
    if (ready && snap && len > 0 && len <= snapCap) memcpy(snap, g_sf.buf, len);
    LeaveCriticalSection(&g_sf.cs);

    long need = FrameBytes(m_w, m_h, m_sub);
    if (!ready || !snap || sw <= 0 || sh <= 0)
    {
        // YUV 的黑是 Y=16 / UV=128，不是全 0
        if (IsYuvSub(m_sub))
        {
            long ysz = (long)m_w * m_h;
            memset(dst, 16, ysz);
            if (m_sub == MEDIASUBTYPE_YUY2)
                for (long i = 0; i < ysz; i++) { dst[i * 2] = 16; dst[i * 2 + 1] = 128; }
            else
                memset(dst + ysz, 128, need - ysz);
        }
        else memset(dst, 0, need);
        return;
    }

    // Fit=2：把画面缩到 75% 居中留黑边。
    // 用途：微信若把 4:3 的画面裁成 16:9（上下各切掉 12.5%），这样内容仍然完整可见。
    if (g_cfgFit == 2 && m_sub == MEDIASUBTYPE_RGB24 && m_w > 8 && m_h > 8)
    {
        static BYTE* tbuf = NULL;
        static long  tcap = 0;
        // 满宽 + 高度按 16:9 收缩：微信若把画面裁成 16:9，正好一点都不丢
        int ew = m_w;
        int eh = (int)((long)m_w * 9 / 16) & ~1;
        if (eh > m_h) eh = m_h & ~1;
        long tn = (long)ew * eh * 3;
        if (!tbuf || tcap < tn)
        {
            if (tbuf) VirtualFree(tbuf, 0, MEM_RELEASE);
            tbuf = (BYTE*)VirtualAlloc(NULL, tn, MEM_COMMIT, PAGE_READWRITE);
            tcap = tbuf ? tn : 0;
        }
        if (tbuf)
        {
            if (sub == MEDIASUBTYPE_NV12)      NV12ToRGB(snap, sw, sh, tbuf, ew, eh);
            else if (sub == MEDIASUBTYPE_YUY2) YUY2ToRGB(snap, sw, sh, tbuf, ew, eh);
            else                                RGBToRGB(snap, sw, sh, tbuf, ew, eh);
            memset(dst, 0, FrameBytes(m_w, m_h, m_sub));
            int offY = (m_h - eh) / 2;
            int offX = (m_w - ew) / 2;
            for (int y = 0; y < eh; y++)
                memcpy(dst + ((long)(y + offY) * m_w + offX) * 3,
                       tbuf + (long)y * ew * 3, (long)ew * 3);
            return;
        }
    }

    // 源与目标宽高比不同时补黑边（保持视野完整，不拉伸也不裁切）
    const BYTE* srcP = snap;
    int sW = sw, sH = sh;
    if (g_cfgFit == 1 && sw > 0 && sh > 0 && m_w > 0 && m_h > 0 &&
        (long)sw * m_h != (long)m_w * sh &&
        (sub == MEDIASUBTYPE_NV12 || sub == MEDIASUBTYPE_RGB24))
    {
        static BYTE* g_padBuf = NULL;
        static long  g_padCap = 0;

        int newW = sw, newH = sh;
        if ((long)sw * m_h > (long)m_w * sh)          // 源更宽 → 上下补
            newH = (int)((long)sw * m_h / m_w);
        else                                           // 源更高/更窄 → 左右补
            newW = (int)((long)sh * m_w / m_h);
        newW = (newW + 1) & ~1;
        newH = (newH + 1) & ~1;

        long needPad = (newW <= MAX_W && newH <= MAX_H) ? FrameBytes(newW, newH, sub) : 0;
        if (needPad > 0 && (!g_padBuf || g_padCap < needPad))
        {
            if (g_padBuf) VirtualFree(g_padBuf, 0, MEM_RELEASE);
            g_padBuf = (BYTE*)VirtualAlloc(NULL, needPad, MEM_COMMIT, PAGE_READWRITE);
            g_padCap = g_padBuf ? needPad : 0;
        }
        if (needPad > 0 && g_padBuf && newW <= MAX_W && newH <= MAX_H)
        {
            int offX = ((newW - sw) / 2) & ~1;
            int offY = ((newH - sh) / 2) & ~1;

            if (sub == MEDIASUBTYPE_NV12)
            {
                // Y 平面
                memset(g_padBuf, 16, (long)newW * newH);
                for (int y = 0; y < sh; y++)
                    memcpy(g_padBuf + (long)(y + offY) * newW + offX,
                           snap + (long)y * sw, sw);
                // UV 平面
                BYTE* uvD = g_padBuf + (long)newW * newH;
                BYTE* uvS = (BYTE*)snap + (long)sw * sh;
                memset(uvD, 128, (long)newW * newH / 2);
                for (int y = 0; y < sh / 2; y++)
                    memcpy(uvD + (long)(y + offY / 2) * newW + offX,
                           uvS + (long)y * sw, sw);
                srcP = g_padBuf; sW = newW; sH = newH;
            }
            else   // RGB24
            {
                memset(g_padBuf, 0, (long)newW * newH * 3);
                for (int y = 0; y < sh; y++)
                    memcpy(g_padBuf + ((long)(y + offY) * newW + offX) * 3,
                           snap + (long)y * sw * 3, (long)sw * 3);
                srcP = g_padBuf; sW = newW; sH = newH;
            }
            static LONG s_logged = 0;
            if (InterlockedIncrement(&s_logged) == 1)
                VLog("FillOutput: pad %dx%d -> %dx%d for target %dx%d", sw, sh, newW, newH, m_w, m_h);
        }
    }

    if (sub == MEDIASUBTYPE_NV12)
    {
        if (m_sub == MEDIASUBTYPE_NV12)      NV12ToNV12(srcP, sW, sH, dst, m_w, m_h, false);
        else if (m_sub == MEDIASUBTYPE_I420X) NV12ToNV12(srcP, sW, sH, dst, m_w, m_h, true);
        else if (m_sub == MEDIASUBTYPE_YUY2) NV12ToYUY2(srcP, sW, sH, dst, m_w, m_h);
        else                                 NV12ToRGB (srcP, sW, sH, dst, m_w, m_h);
    }
    else if (sub == MEDIASUBTYPE_I420X)
    {
        // 源几乎总是 NV12（ConfigureCameraFormat 强制），走快速路径
        if (m_sub == MEDIASUBTYPE_I420X)      NV12ToNV12(srcP, sW, sH, dst, m_w, m_h, true);
        else if (m_sub == MEDIASUBTYPE_NV12)  NV12ToNV12(srcP, sW, sH, dst, m_w, m_h, true);
        else                                  NV12ToNV12(srcP, sW, sH, dst, m_w, m_h, true);
    }
    else if (sub == MEDIASUBTYPE_YUY2)
    {
        if (m_sub == MEDIASUBTYPE_YUY2) YUY2ToYUY2(srcP, sW, sH, dst, m_w, m_h);
        else if (m_sub == MEDIASUBTYPE_NV12) NV12ToYUY2(srcP, sW, sH, dst, m_w, m_h);
        else                            YUY2ToRGB (srcP, sW, sH, dst, m_w, m_h);
    }
    else
    {
        if (m_sub == MEDIASUBTYPE_YUY2) YUY2ToRGB(snap, sw, sh, dst, m_w, m_h);
        else if (m_sub == MEDIASUBTYPE_NV12) NV12ToRGB(srcP, sW, sH, dst, m_w, m_h);
        else                            RGBToRGB (snap, sw, sh, dst, m_w, m_h);
    }

    // 调试：把第 20 帧落盘
    if (InterlockedIncrement(&s_dumpCount) == 20)
    {
        const char* p = getenv("VCAM_DUMP");
        if (p && *p)
        {
            FILE* fp = fopen(p, "wb");
            if (fp) { fwrite(dst, 1, need, fp); fclose(fp); }
        }
    }
}

DWORD CVcamPin::ThreadProc()
{
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // 优先用下游提供的分配器；拿不到就自建（真实采集卡都是自建再通知下游）
    HRESULT hga = E_FAIL;
    for (int i = 0; i < 25 && m_running && !m_alloc; i++)
    {
        if (m_memInput) hga = m_memInput->GetAllocator(&m_alloc);
        if (!m_alloc) Sleep(20);
    }
    VLog("ThreadProc: GetAllocator=0x%08X alloc=%p running=%ld memInput=%p",
         (unsigned)hga, m_alloc, (long)m_running, m_memInput);

    if (!m_alloc)
    {
        IMemAllocator* own = NULL;
        HRESULT hca = CoCreateInstance(CLSID_MemoryAllocatorX, NULL, CLSCTX_INPROC_SERVER,
                                       IID_IMemAllocator, (void**)&own);
        VLog("ThreadProc: create own allocator=0x%08X ptr=%p", (unsigned)hca, own);
        if (SUCCEEDED(hca) && own)
        {
            m_alloc = own;
            if (m_memInput)
                VLog("ThreadProc: NotifyAllocator(own)=0x%08X",
                     (unsigned)m_memInput->NotifyAllocator(m_alloc, FALSE));
        }
    }
    if (!m_alloc) { VLog("ThreadProc: no allocator -> exit"); if (SUCCEEDED(hrCo)) CoUninitialize(); return 0; }
    ALLOCATOR_PROPERTIES req, got;
    ZeroMemory(&req, sizeof(req));
    req.cBuffers = 8;
    req.cbBuffer = FrameBytes(m_w, m_h, m_sub);
    req.cbAlign  = 1;
    req.cbPrefix = 0;
    ZeroMemory(&got, sizeof(got));
    HRESULT hsp = m_alloc->SetProperties(&req, &got);
    if (m_memInput) m_memInput->NotifyAllocator(m_alloc, FALSE);
    HRESULT hrc = m_alloc->Commit();
    VLog("ThreadProc: SetProperties=0x%08X (buf=%ld size=%ld) Commit=0x%08X target=%dx%d",
         (unsigned)hsp, got.cBuffers, got.cbBuffer, (unsigned)hrc, m_w, m_h);

    long need = FrameBytes(m_w, m_h, m_sub);
    if(FAILED(hsp) || FAILED(hrc) || got.cbBuffer < need) { m_alloc->Decommit();if(SUCCEEDED(hrCo))CoUninitialize();return 0; }
    LONGLONG period = 10000000LL / (m_fps > 0 ? m_fps : 30);
    LONGLONG t = 0;
    IReferenceClock* clk = m_owner ? m_owner->Clock() : NULL;
    VLog("ThreadProc: clock=%p", clk);
    LARGE_INTEGER qfreq,qstart,qnow;QueryPerformanceFrequency(&qfreq);QueryPerformanceCounter(&qstart);
    LONGLONG nextDue=qstart.QuadPart;
    LONGLONG tickPeriod=qfreq.QuadPart/(m_fps>0?m_fps:30);

    while (m_running)
    {
        IMediaSample* s = NULL;
        if (FAILED(m_alloc->GetBuffer(&s, NULL, NULL, 0)) || !s)
        {
            static int failc = 0;
            if ((failc++ % 50) == 0) VLog("ThreadProc: GetBuffer FAILED x%d", failc);
            Sleep(5);
            continue;
        }
        BYTE* p = NULL;
        if (SUCCEEDED(s->GetPointer(&p)) && p && s->GetSize() >= need)
        {
            FillOutput(p);
            s->SetActualDataLength(need);
            s->SetSyncPoint(TRUE);
            REFERENCE_TIME rt1 = t, rt2 = t + period;
            QueryPerformanceCounter(&qnow); rt1=(qnow.QuadPart-qstart.QuadPart)*10000000LL/qfreq.QuadPart; rt2=rt1+period;
            s->SetTime(&rt1, &rt2);
            t += period;
            HRESULT hrr = m_memInput ? m_memInput->Receive(s) : E_FAIL;
            static int okc = 0;
            if ((okc++ % 30) == 0) VLog("ThreadProc: delivered=%d Receive=0x%08X ready=%ld", okc, (unsigned)hrr, (long)g_sf.ready);
        }
        s->Release();

        nextDue+=tickPeriod;
        QueryPerformanceCounter(&qnow);
        if(qnow.QuadPart>nextDue+tickPeriod) nextDue=qnow.QuadPart;
        while(m_running && qnow.QuadPart<nextDue) {
            DWORD wait=(DWORD)((nextDue-qnow.QuadPart)*1000/qfreq.QuadPart);
            Sleep(wait>10?10:(wait?wait:1));QueryPerformanceCounter(&qnow);
        }
    }

    m_alloc->Decommit();
    if (SUCCEEDED(hrCo)) CoUninitialize();
    return 0;
}

void CVcamPin::StartDelivery()
{
    VLog("StartDelivery: already=%ld connected=%p", (long)m_running, m_connected);
    if (m_running) return;
    m_running = 1;
    DWORD id = 0;
    m_thread = CreateThread(NULL, 0, ThreadEntry, this, 0, &id);
}

void CVcamPin::StopDelivery()
{
    if (!m_running) return;
    m_running = 0;
    if (m_thread)
    {
        WaitForSingleObject(m_thread, 5000);
        CloseHandle(m_thread);
        m_thread = NULL;
    }
}

STDMETHODIMP CVcamPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt)
{
    if (!pReceivePin) return E_POINTER;
    if (m_connected) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE local;
    const AM_MEDIA_TYPE* use = pmt;
    if (!use)
    {
        FmtEntry f = { m_w, m_h, m_fps };
        FillMediaType(f, m_sub, &local);
        use = &local;
    }
    if (!ParseMediaType(use, NULL, NULL, NULL, NULL))
    {
        if (!pmt) FreeMediaType(&local);
        return VFW_E_TYPE_NOT_ACCEPTED;
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, use);
    if (FAILED(hr)) { if (!pmt) FreeMediaType(&local); return hr; }

    // 记录本侧连接信息 —— 少了这一步就拿不到下游分配器
    m_connected = pReceivePin;
    m_connected->AddRef();
    if (FAILED(pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&m_memInput)))
        m_memInput = NULL;

    int w, h, fps; GUID sub;
    if (ParseMediaType(use, &w, &h, &fps, &sub)) SetFormat((AM_MEDIA_TYPE*)use);
    if (!pmt) FreeMediaType(&local);

    VLog("Pin::Connect ok memInput=%p size=%dx%d", m_memInput, m_w, m_h);
    return S_OK;
}

STDMETHODIMP CVcamPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt)
{
    if (!pConnector) return E_POINTER;
    if (m_connected) return VFW_E_ALREADY_CONNECTED;
    int w, h, fps;
    GUID sub;
    if (!ParseMediaType(pmt, &w, &h, &fps, &sub)) return VFW_E_TYPE_NOT_ACCEPTED;

    m_w = w; m_h = h; m_fps = fps; m_sub = sub;
    m_outLen = FrameBytes(w, h, sub);
    if (m_outBuf) { VirtualFree(m_outBuf, 0, MEM_RELEASE); m_outBuf = NULL; }
    m_outBuf = (BYTE*)VirtualAlloc(NULL, m_outLen, MEM_COMMIT, PAGE_READWRITE);

    m_connected = pConnector;
    m_connected->AddRef();
    if (FAILED(pConnector->QueryInterface(IID_IMemInputPin, (void**)&m_memInput)))
        m_memInput = NULL;
    return S_OK;
}

STDMETHODIMP CVcamPin::Disconnect()
{
    StopDelivery();
    ReleaseConnections();
    return S_OK;
}

STDMETHODIMP CVcamPin::ConnectedTo(IPin** ppPin)
{
    if (!ppPin) return E_POINTER;
    if (!m_connected) { *ppPin = NULL; return VFW_E_NOT_CONNECTED; }
    *ppPin = m_connected;
    m_connected->AddRef();
    return S_OK;
}

STDMETHODIMP CVcamPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt)
{
    if (!pmt) return E_POINTER;
    if (!m_connected) return VFW_E_NOT_CONNECTED;
    ZeroMemory(pmt, sizeof(*pmt));
    FmtEntry f = { m_w, m_h, m_fps };
    FillMediaType(f, m_sub, pmt);
    return S_OK;
}

STDMETHODIMP CVcamPin::QueryPinInfo(PIN_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = (IBaseFilter*)m_owner;
    if (m_owner) m_owner->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    lstrcpynW(pInfo->achName, L"Capture", 128);
    return S_OK;
}

STDMETHODIMP CVcamPin::QueryDirection(PIN_DIRECTION* pPinDir)
{
    VLog("Pin::QueryDirection");
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

STDMETHODIMP CVcamPin::QueryId(LPWSTR* Id)
{
    if (!Id) return E_POINTER;
    size_t len = (wcslen(L"Capture") + 1) * sizeof(wchar_t);
    *Id = (LPWSTR)CoTaskMemAlloc(len);
    if (!*Id) return E_OUTOFMEMORY;
    memcpy(*Id, L"Capture", len);
    return S_OK;
}

STDMETHODIMP CVcamPin::QueryAccept(const AM_MEDIA_TYPE* pmt)
{
    return ParseMediaType(pmt, NULL, NULL, NULL, NULL) ? S_OK : S_FALSE;
}

STDMETHODIMP CVcamPin::EnumMediaTypes(IEnumMediaTypes** ppEnum)
{
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumMediaTypes();
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CVcamPin::QueryInternalConnections(IPin** apPin, ULONG* nPin)
{
    if (nPin) *nPin = 0;
    return E_NOTIMPL;
}

STDMETHODIMP CVcamPin::EndOfStream() { return S_OK; }
STDMETHODIMP CVcamPin::BeginFlush()  { return S_OK; }
STDMETHODIMP CVcamPin::EndFlush()    { return S_OK; }
STDMETHODIMP CVcamPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

STDMETHODIMP CVcamPin::SetFormat(AM_MEDIA_TYPE* pmt)
{
    if(m_running) return VFW_E_NOT_STOPPED;
    int w, h, fps;
    GUID sub;
    if (!ParseMediaType(pmt, &w, &h, &fps, &sub))
    {
        VLog("Pin::SetFormat REJECT maj=%08X fmt=%08X sub=%08X",
             pmt ? (unsigned)pmt->majortype.Data1 : 0,
             pmt ? (unsigned)pmt->formattype.Data1 : 0,
             pmt ? (unsigned)pmt->subtype.Data1 : 0);
        return E_INVALIDARG;
    }
    VLog("Pin::SetFormat %dx%d fps=%d sub=%08X", w, h, fps, (unsigned)sub.Data1);
    m_w = w; m_h = h; m_fps = fps; m_sub = sub;
    m_outLen = FrameBytes(w, h, sub);
    if (m_outBuf) { VirtualFree(m_outBuf, 0, MEM_RELEASE); m_outBuf = NULL; }
    m_outBuf = (BYTE*)VirtualAlloc(NULL, m_outLen, MEM_COMMIT, PAGE_READWRITE);
    return S_OK;
}

STDMETHODIMP CVcamPin::GetFormat(AM_MEDIA_TYPE** ppmt)
{
    if (!ppmt) return E_POINTER;
    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!pmt) return E_OUTOFMEMORY;
    FmtEntry f = { m_w, m_h, m_fps };
    FillMediaType(f, m_sub, pmt);
    *ppmt = pmt;
    return S_OK;
}

STDMETHODIMP CVcamPin::GetNumberOfCapabilities(int* piCount, int* piSize)
{
    if (!piCount || !piSize) return E_POINTER;
    *piCount = CapsCount();
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    VLog("Pin::GetNumberOfCapabilities -> count=%d size=%d", *piCount, *piSize);
    return S_OK;
}

STDMETHODIMP CVcamPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC)
{
    if (!ppmt) return E_POINTER;
    if (iIndex < 0 || iIndex >= CapsCount()) return S_FALSE;
    VLog("Pin::GetStreamCaps idx=%d", iIndex);

    FmtEntry f; GUID sub;
    CapsIdx(iIndex, &f, &sub);

    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!pmt) return E_OUTOFMEMORY;
    FillMediaType(f, sub, pmt);
    *ppmt = pmt;

    if (pSCC)
    {
        ZeroMemory(pSCC, sizeof(VIDEO_STREAM_CONFIG_CAPS));
        VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
        caps->guid = FORMAT_VideoInfo;
        caps->VideoStandard = 0;
        caps->InputSize.cx = f.w;
        caps->InputSize.cy = f.h;
        caps->MinCroppingSize.cx = f.w;
        caps->MinCroppingSize.cy = f.h;
        caps->MaxCroppingSize.cx = f.w;
        caps->MaxCroppingSize.cy = f.h;
        caps->CropGranularityX = 1;
        caps->CropGranularityY = 1;
        caps->CropAlignX = 1;
        caps->CropAlignY = 1;
        caps->MinOutputSize.cx = f.w;
        caps->MinOutputSize.cy = f.h;
        caps->MaxOutputSize.cx = f.w;
        caps->MaxOutputSize.cy = f.h;
        caps->OutputGranularityX = 2;   // YUV 要求偶数尺寸
        caps->OutputGranularityY = 2;
        caps->StretchTapsX = 0;
        caps->StretchTapsY = 0;
        caps->ShrinkTapsX = 0;
        caps->ShrinkTapsY = 0;
        caps->MinFrameInterval = 10000000LL / f.fps;
        caps->MaxFrameInterval = 10000000LL / f.fps;
        caps->MinBitsPerSecond = (LONG)f.w * f.h * 8 * f.fps;
        caps->MaxBitsPerSecond = caps->MinBitsPerSecond;
    }
    return S_OK;
}

// ===========================================================================
// CEnumPins implementation
// ===========================================================================
STDMETHODIMP CEnumPins::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IEnumPins)
    { *ppv = (IEnumPins*)this; AddRef(); return S_OK; }
    *ppv = NULL; return E_NOINTERFACE;
}

STDMETHODIMP CEnumPins::Next(ULONG c, IPin** pp, ULONG* fetched)
{
    if (!pp) return E_POINTER;
    ULONG i = 0;
    if (c > 0 && m_pos == 0 && m_pin)
    {
        pp[0] = m_pin;
        m_pin->AddRef();
        m_pos = 1;
        i = 1;
    }
    if (fetched) *fetched = i;
    return (i == c) ? S_OK : S_FALSE;
}

// ===========================================================================
// CVcam implementation
// ===========================================================================
CVcam::CVcam() : m_ref(1), m_graph(NULL), m_state(State_Stopped), m_clock(NULL)
{
    m_pin = new CVcamPin(this);
}

CVcam::~CVcam()
{
    StopCapture();
    if (m_pin) { m_pin->StopDelivery(); m_pin->Release(); m_pin = NULL; }
    if (m_graph) { m_graph->Release(); m_graph = NULL; }
    if (m_clock) { m_clock->Release(); m_clock = NULL; }
}

STDMETHODIMP CVcam::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    VLog("CVcam::QI riid=%08X-%04X-%04X", (unsigned)riid.Data1, riid.Data2, riid.Data3);
    if (riid == IID_IUnknown || riid == IID_IBaseFilter ||
        riid == IID_IMediaFilter || riid == IID_IPersist)
    {
        *ppv = (IBaseFilter*)this;
        AddRef();
        return S_OK;
    }
    if (riid == IID_IAMFilterMiscFlags)
    {
        *ppv = (IAMFilterMiscFlags*)&g_miscFlags;
        g_miscFlags.AddRef();
        return S_OK;
    }
    *ppv = NULL;
    return E_NOINTERFACE;
}

STDMETHODIMP CVcam::GetClassID(CLSID* pClsID)
{
    if (!pClsID) return E_POINTER;
    *pClsID = CLSID_VirtualCam;
    return S_OK;
}

STDMETHODIMP CVcam::Stop()
{
    VLog("CVcam::Stop called");
    m_state = State_Stopped;
    if (m_pin) m_pin->StopDelivery();
    StopCapture();
    return S_OK;
}

STDMETHODIMP CVcam::Pause()
{
    VLog("CVcam::Pause called");
    m_state = State_Paused;
    StartCapture();
    if (m_pin) m_pin->StartDelivery();
    return S_OK;
}

STDMETHODIMP CVcam::Run(REFERENCE_TIME)
{
    VLog("CVcam::Run called");
    m_state = State_Running;
    StartCapture();
    if (m_pin) m_pin->StartDelivery();
    return S_OK;
}

STDMETHODIMP CVcam::GetState(DWORD, FILTER_STATE* State)
{
    if (!State) return E_POINTER;
    *State = m_state;
    return S_OK;
}

STDMETHODIMP CVcam::SetSyncSource(IReferenceClock* pClock)
{
    if (m_clock) { m_clock->Release(); m_clock = NULL; }
    if (pClock) { m_clock = pClock; m_clock->AddRef(); }
    VLog("CVcam::SetSyncSource clock=%p", pClock);
    return S_OK;
}

STDMETHODIMP CVcam::GetSyncSource(IReferenceClock** ppClock)
{
    if (!ppClock) return E_POINTER;
    // 没有宿主要求的时钟时，提供一个系统时钟 —— 下游没有时钟就排不出样本
    if (!m_clock)
    {
        IReferenceClock* c = NULL;
        if (SUCCEEDED(CoCreateInstance(CLSID_SystemClockX, NULL, CLSCTX_INPROC_SERVER,
                                       IID_IReferenceClock, (void**)&c)) && c)
            m_clock = c;
    }
    if (m_clock) m_clock->AddRef();
    *ppClock = m_clock;
    return S_OK;
}

STDMETHODIMP CVcam::EnumPins(IEnumPins** ppEnum)
{
    VLog("CVcam::EnumPins called");
    if (!ppEnum) return E_POINTER;
    *ppEnum = new CEnumPins(m_pin);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

STDMETHODIMP CVcam::FindPin(LPCWSTR Id, IPin** ppPin)
{
    if (!ppPin || !Id) return E_POINTER;
    if (wcscmp(Id, L"Capture") == 0)
    {
        *ppPin = m_pin;
        m_pin->AddRef();
        return S_OK;
    }
    *ppPin = NULL;
    return VFW_E_NOT_FOUND;
}

STDMETHODIMP CVcam::QueryFilterInfo(FILTER_INFO* pInfo)
{
    if (!pInfo) return E_POINTER;
    lstrcpynW(pInfo->achName, VCAM_NAME, MAX_FILTER_NAME);
    pInfo->pGraph = m_graph;
    if (m_graph) m_graph->AddRef();
    return S_OK;
}

STDMETHODIMP CVcam::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR)
{
    if (m_graph) { m_graph->Release(); m_graph = NULL; }
    if (pGraph) { m_graph = pGraph; m_graph->AddRef(); }
    return S_OK;
}

STDMETHODIMP CVcam::QueryVendorInfo(LPWSTR* pVendorInfo)
{
    if (pVendorInfo) *pVendorInfo = NULL;
    return E_NOTIMPL;
}

// ===========================================================================
// class factory + exports
// ===========================================================================
class CClassFactory : public IClassFactory
{
    LONG m_ref;
public:
    CClassFactory() : m_ref(1) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory)
        { *ppv = (IClassFactory*)this; AddRef(); return S_OK; }
        *ppv = NULL; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv)
    {
        if (ppv == NULL) return E_POINTER;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CVcam* f = new CVcam();
        if (!f) return E_OUTOFMEMORY;
        HRESULT hr = f->QueryInterface(riid, ppv);
        f->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL b) { if (b) InterlockedIncrement(&m_ref); else InterlockedDecrement(&m_ref); return S_OK; }
};

HMODULE g_hModule = NULL;

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) { g_hModule = h; DisableThreadLibraryCalls(h); }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    LoadCfg();          // 不能在 DllMain 里做文件 IO，会死锁宿主进程
    if (!ppv) return E_POINTER;
    if (!IsEqualCLSID(rclsid, CLSID_VirtualCam)) return CLASS_E_CLASSNOTAVAILABLE;
    CClassFactory* cf = new CClassFactory();
    if (!cf) return E_OUTOFMEMORY;
    HRESULT hr = cf->QueryInterface(riid, ppv);
    cf->Release();
    return hr;
}

STDAPI DllCanUnloadNow()
{
    return S_FALSE;   // keep loaded, simpler and safer
}

// ---------------------------------------------------------------------------
// registration
// ---------------------------------------------------------------------------
static LONG RegSetStr(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* val)
{
    HKEY k;
    LONG r = RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS) return r;
    r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r;
}

static void RegDeleteTree(HKEY root, const wchar_t* sub)
{
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_ALL_ACCESS, &k) == ERROR_SUCCESS)
    {
        RegDeleteTreeW(k, NULL);
        RegCloseKey(k);
        RegDeleteKeyW(root, sub);
    }
}

STDAPI DllRegisterServer()
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, path, MAX_PATH)) return SELFREG_E_CLASS;

    wchar_t clsid[64];
    StringFromGUID2(CLSID_VirtualCam, clsid, 64);

    wchar_t sub[512];

    // filter CLSID
    swprintf_s(sub, 512, L"CLSID\\%s", clsid);
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, NULL, VCAM_NAME); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }
    swprintf_s(sub, 512, L"CLSID\\%s\\InprocServer32", clsid);
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, NULL, path); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, L"ThreadingModel", L"Both"); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }

    // video input device category
    wchar_t catClsid[64];
    StringFromGUID2(CLSID_VideoInputDeviceCategory, catClsid, 64);
    swprintf_s(sub, 512, L"CLSID\\%s\\Instance\\%s", catClsid, clsid);
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, NULL, VCAM_NAME); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, L"CLSID", clsid); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }
    { LONG err=RegSetStr(HKEY_CLASSES_ROOT, sub, L"FriendlyName", VCAM_NAME); if(err!=ERROR_SUCCESS) return HRESULT_FROM_WIN32(err); }

    return S_OK;
}


STDAPI DllUnregisterServer()
{
    wchar_t clsid[64];
    StringFromGUID2(CLSID_VirtualCam, clsid, 64);
    wchar_t catClsid[64];
    StringFromGUID2(CLSID_VideoInputDeviceCategory, catClsid, 64);

    wchar_t sub[512];
    swprintf_s(sub, 512, L"CLSID\\%s\\Instance\\%s", catClsid, clsid);
    RegDeleteTree(HKEY_CLASSES_ROOT, sub);
    swprintf_s(sub, 512, L"CLSID\\%s", clsid);
    RegDeleteTree(HKEY_CLASSES_ROOT, sub);
    return S_OK;
}
