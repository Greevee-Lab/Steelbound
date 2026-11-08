/* W210: bounded asynchronous readback + worker-owned MF/WIC encoding.
 * No desktop APIs, no audio, no capture resources until explicit menu action.
 * GPU/context work belongs to caller under ctx_lock; worker sees CPU pixels only.
 */
#include "render_int.h"
#include "render_clip.h"
#include "sb_overlay.h"
#include "sb_pause.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <stdlib.h>
#include <process.h>
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#define CQ 8
#define CS 3
#define LIMIT 9500000u
static struct {
    volatile LONG active, done, stop, failed, head, tail;
    HANDLE thread, wake;
    ULONGLONG begin, next;
    unsigned w, h, sw, sh, bytes, submitted;
    unsigned xmap[1280];
    int result, force_jpg;
    char dir[600];
    struct { BYTE *rgb; unsigned at; } q[CQ];
    struct { ID3D11Texture2D *tex; int pending; unsigned at; } st[CS];
    DXGI_FORMAT fmt;
} c;
/* Delay-load manually: even machines without MF DLLs can start the client. */
static HRESULT (WINAPI *mf_start)(ULONG,DWORD);
static HRESULT (WINAPI *mf_stop)(void);
static HRESULT (WINAPI *mf_type)(IMFMediaType **);
static HRESULT (WINAPI *mf_sample)(IMFSample **);
static HRESULT (WINAPI *mf_buffer)(DWORD,IMFMediaBuffer **);
static HRESULT (WINAPI *mf_writer)(LPCWSTR,IMFByteStream *,IMFAttributes *,IMFSinkWriter **);
static void clip_path(wchar_t *out, const char *name)
{
    char path[720];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", c.dir, name);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, 720))
        MultiByteToWideChar(CP_ACP, 0, path, -1, out, 720);
}
static unsigned clip_size(const wchar_t *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &a) && !a.nFileSizeHigh ? a.nFileSizeLow : 0;
}
void sb_clip_remove(const char *dir)
{
    char path[720];
    unsigned i;
    /* Only exact filenames owned by this feature, never enumerate/delete user files. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\clip.mp4", dir); DeleteFileA(path);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\clip.txt", dir); DeleteFileA(path);
    for (i = 0; i < 50; ++i) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\clip_%03u.jpg", dir, i); DeleteFileA(path);
    }
}
static HRESULT clip_type(IMFMediaType **mt, int output)
{
    HRESULT hr = mf_type(mt);
#define MT(call) if (SUCCEEDED(hr)) { hr = (call); }
    MT(IMFMediaType_SetGUID(*mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video));
    MT(IMFMediaType_SetGUID(*mt, &MF_MT_SUBTYPE, output ? &MFVideoFormat_H264 : &MFVideoFormat_NV12));
    MT(IMFMediaType_SetUINT32(*mt, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    MT(IMFMediaType_SetUINT64(*mt, &MF_MT_FRAME_SIZE, ((UINT64)c.w << 32) | c.h));
    MT(IMFMediaType_SetUINT64(*mt, &MF_MT_FRAME_RATE, ((UINT64)30 << 32) | 1));
    MT(IMFMediaType_SetUINT64(*mt, &MF_MT_PIXEL_ASPECT_RATIO, ((UINT64)1 << 32) | 1));
    if (output) { MT(IMFMediaType_SetUINT32(*mt, &MF_MT_AVG_BITRATE, 4000000)); }
#undef MT
    return hr;
}
static BYTE clamp_byte(int x) { return (BYTE)(x < 0 ? 0 : x > 255 ? 255 : x); }
static HRESULT clip_sample(IMFSinkWriter *wr, DWORD stream, const BYTE *rgb, unsigned frame)
{
    IMFMediaBuffer *buf = NULL;
    IMFSample *s = NULL;
    BYTE *dst;
    unsigned x, y, size = c.w * c.h * 3 / 2;
    HRESULT hr = mf_buffer(size, &buf);
    if (SUCCEEDED(hr)) { hr = IMFMediaBuffer_Lock(buf, &dst, NULL, NULL); }
    if (SUCCEEDED(hr)) {
        for (y = 0; y < c.h; ++y) for (x = 0; x < c.w; ++x) {
            const BYTE *p = rgb + (y * c.w + x) * 4;
            int b = p[0], g = p[1], r = p[2];
            dst[y * c.w + x] = clamp_byte(((66*r + 129*g + 25*b + 128) >> 8) + 16);
            if (!(x & 1) && !(y & 1)) {
                unsigned k, j; int rr = 0, gg = 0, bb = 0;
                for (j = 0; j < 2; ++j) for (k = 0; k < 2; ++k) {
                    p = rgb + ((y+j)*c.w+x+k)*4; bb += p[0]; gg += p[1]; rr += p[2];
                }
                r = rr/4; g = gg/4; b = bb/4;
                dst[c.w*c.h+(y/2)*c.w+x] = clamp_byte(((-38*r-74*g+112*b+128)>>8)+128);
                dst[c.w*c.h+(y/2)*c.w+x+1] = clamp_byte(((112*r-94*g-18*b+128)>>8)+128);
            }
        }
        IMFMediaBuffer_Unlock(buf);
        hr = IMFMediaBuffer_SetCurrentLength(buf, size);
    }
    if (SUCCEEDED(hr)) { hr = mf_sample(&s); }
    if (SUCCEEDED(hr)) { hr = IMFSample_AddBuffer(s, buf); }
    if (SUCCEEDED(hr)) { hr = IMFSample_SetSampleTime(s, (LONGLONG)frame * 10000000 / 30); }
    if (SUCCEEDED(hr)) { hr = IMFSample_SetSampleDuration(s, (LONGLONG)(frame+1)*10000000/30 - (LONGLONG)frame*10000000/30); }
    if (SUCCEEDED(hr)) { hr = IMFSinkWriter_WriteSample(wr, stream, s); }
    if (s) IMFSample_Release(s);
    if (buf) IMFMediaBuffer_Release(buf);
    return hr;
}
static unsigned clip_jpg(IWICImagingFactory *fac, const BYTE *rgb, unsigned frame)
{
    IWICStream *st = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *fr = NULL;
    IPropertyBag2 *pr = NULL; WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    wchar_t path[720]; char name[40]; BYTE *bgr; unsigned i, size = 0; HRESULT hr;
    _snprintf_s(name, sizeof name, _TRUNCATE, "clip_%03u.jpg", frame/3); clip_path(path, name);
    bgr = (BYTE *)malloc(c.w*c.h*3); if (!bgr || !fac) { free(bgr); return 0; }
    for (i = 0; i < c.w*c.h; ++i) memcpy(bgr+i*3, rgb+i*4, 3);
    hr = IWICImagingFactory_CreateStream(fac, &st);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(fac, &GUID_ContainerFormatJpeg, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &fr, &pr);
    if (SUCCEEDED(hr) && pr) {
        PROPBAG2 opt = {0}; VARIANT v; opt.pstrName = L"ImageQuality";
        VariantInit(&v); v.vt = VT_R4; v.fltVal = 0.55f; hr = IPropertyBag2_Write(pr, 1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(fr, pr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(fr, c.w, c.h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(fr, &pf);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(fr, c.h, c.w*3, c.w*c.h*3, bgr);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(fr);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    if (pr) IPropertyBag2_Release(pr);
    if (fr) IWICBitmapFrameEncode_Release(fr);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    free(bgr);
    if (SUCCEEDED(hr)) size = clip_size(path);
    if (!size) DeleteFileW(path);
    return size;
}
static unsigned __stdcall clip_worker(void *unused)
{
    HMODULE plat = NULL, rw = NULL;
    IMFSinkWriter *wr = NULL; IMFMediaType *out = NULL, *in = NULL;
    IWICImagingFactory *fac = NULL;
    DWORD stream = 0; HRESULT hr = E_FAIL; int started = 0, jpg_ok = 1;
    BYTE *last = (BYTE *)malloc(c.w*c.h*4);
    unsigned frame = 0, jpg_bytes = 0; int have = 0;
    wchar_t path[720]; HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)unused;
    clip_path(path, "clip.mp4");
    if (!c.force_jpg) { plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); rw = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); }
    if (plat && rw) {
#define FN(dst,mod,name) dst = (void *)GetProcAddress(mod,name)
        FN(mf_start,plat,"MFStartup"); FN(mf_stop,plat,"MFShutdown"); FN(mf_type,plat,"MFCreateMediaType");
        FN(mf_sample,plat,"MFCreateSample"); FN(mf_buffer,plat,"MFCreateMemoryBuffer"); FN(mf_writer,rw,"MFCreateSinkWriterFromURL");
#undef FN
        if (mf_start && mf_stop && mf_type && mf_sample && mf_buffer && mf_writer) {
            hr = mf_start(MF_VERSION, MFSTARTUP_FULL); started = SUCCEEDED(hr);
            if (SUCCEEDED(hr)) hr = mf_writer(path, NULL, NULL, &wr);
            if (SUCCEEDED(hr)) hr = clip_type(&out, 1);
            if (SUCCEEDED(hr)) hr = IMFSinkWriter_AddStream(wr, out, &stream);
            if (SUCCEEDED(hr)) hr = clip_type(&in, 0);
            if (SUCCEEDED(hr)) hr = IMFSinkWriter_SetInputMediaType(wr, stream, in, NULL);
            if (SUCCEEDED(hr)) hr = IMFSinkWriter_BeginWriting(wr);
        }
    }
    HLOG_I("CLIP: encoder=%s hr=0x%08lX %ux%u 30fps 5s audio=none", SUCCEEDED(hr) ? "H264" : "JPG", (unsigned long)hr, c.w, c.h);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac);
    while (last) {
        LONG tail = c.tail;
        unsigned until;
        if (tail == c.head) {
            if (!c.stop) { WaitForSingleObject(c.wake, 50); continue; }
            if (!have) break;
            until = 150;
        } else {
            until = c.q[tail % CQ].at;
            if (!have) { memcpy(last, c.q[tail % CQ].rgb, c.w*c.h*4); have = 1; }
        }
        while (frame < until) {
            if (SUCCEEDED(hr)) hr = clip_sample(wr, stream, last, frame);
            /* Small 10fps safety sequence also covers mid-stream encoder failures. */
            if (frame % 3 == 0 && jpg_ok) {
                unsigned size = clip_jpg(fac, last, frame);
                jpg_bytes += size; if (!size || jpg_bytes >= LIMIT) jpg_ok = 0;
            }
            ++frame;
        }
        if (tail == c.head && c.stop) break;
        memcpy(last, c.q[tail % CQ].rgb, c.w*c.h*4);
        InterlockedIncrement(&c.tail);
    }
    free(last);
    if (SUCCEEDED(hr) && have) hr = IMFSinkWriter_Finalize(wr);
    if (wr) IMFSinkWriter_Release(wr);
    if (out) IMFMediaType_Release(out);
    if (in) IMFMediaType_Release(in);
    if (fac) IWICImagingFactory_Release(fac);
    if (started) mf_stop();
    if (rw) FreeLibrary(rw);
    if (plat) FreeLibrary(plat);
    if (SUCCEEDED(co)) CoUninitialize();
    c.bytes = clip_size(path);
    c.result = !c.failed && have && frame == 150 && SUCCEEDED(hr) && c.bytes && c.bytes < LIMIT ? 1 : 0;
    if (c.result) {
        unsigned i;
        for (i = 0; i < 50; ++i) { char nm[40]; _snprintf_s(nm,sizeof nm,_TRUNCATE,"clip_%03u.jpg",i); clip_path(path,nm); DeleteFileW(path); }
    } else {
        DeleteFileW(path);
        c.result = !c.failed && have && frame == 150 && jpg_ok ? 2 : -1; c.bytes = jpg_bytes;
        if (c.result < 0) { sb_clip_remove(c.dir); c.bytes = 0; }
    }
    {
        FILE *f = NULL; clip_path(path,"clip.txt");
        if (_wfopen_s(&f,path,L"wb") == 0 && f) {
            fprintf(f,"Client image only; no overlays; no audio.\r\nformat=%s width=%u height=%u duration=5.000 bytes=%u\r\n",
                c.result==1 ? "H264/MP4" : c.result==2 ? "JPG sequence" : "failed",c.w,c.h,c.bytes);
            if (c.result==2) fprintf(f,"MP4 unavailable. clip_000.jpg..clip_049.jpg: 10 fps, 100 ms per image.\r\n");
            fclose(f);
        }
    }
    HLOG_I("CLIP: complete result=%d bytes=%u size=%ux%u duration=5.000 frames=%u submitted=%u",c.result,c.bytes,c.w,c.h,frame,c.submitted);
    InterlockedExchange(&c.done,1);
    return 0;
}
int sb_clip_active(void) { return c.active != 0; }
int sb_clip_seconds(void)
{
    ULONGLONG elapsed = GetTickCount64()-c.begin;
    return elapsed >= 5000 ? 0 : 5-(int)(elapsed/1000);
}
int sb_clip_start(const char *dir)
{
    unsigned i; double scale;
    if (c.active || c.thread || g_bb_w < 2 || g_bb_h < 2) return 0;
    memset(&c,0,sizeof c); strcpy_s(c.dir,sizeof c.dir,dir);
    scale = 1280.0/g_bb_w; if (720.0/g_bb_h < scale) scale = 720.0/g_bb_h; if (scale>1) scale=1;
    c.w = ((unsigned)(g_bb_w*scale)) & ~1u; c.h = ((unsigned)(g_bb_h*scale)) & ~1u;
    if (!c.w || !c.h) return 0;
    for (i=0;i<CQ;++i) { c.q[i].rgb=(BYTE *)malloc(c.w*c.h*4); if (!c.q[i].rgb) goto fail; }
    c.wake=CreateEventW(NULL,FALSE,FALSE,NULL); if (!c.wake) goto fail;
    /* Test-only failure injection, not a recording trigger. */
    c.force_jpg = g_cfg.overlay_test[0] && GetEnvironmentVariableA("SB_CLIP_TEST_JPG",NULL,0)>0;
    c.begin=GetTickCount64(); c.next=c.begin;
    c.thread=(HANDLE)_beginthreadex(NULL,0,clip_worker,NULL,CREATE_SUSPENDED,NULL);
    if (!c.thread) goto fail;
    sb_clip_remove(dir);
    InterlockedExchange(&c.active,1);
    ResumeThread(c.thread);
    HLOG_I("CLIP: start explicit feedback action");
    return 1;
fail:
    for(i=0;i<CQ;++i) { free(c.q[i].rgb); c.q[i].rgb=NULL; }
    if(c.wake) CloseHandle(c.wake); c.wake=NULL;
    return 0;
}
void sb_clip_frame(ID3D11RenderTargetView *rtv)
{
    ID3D11Resource *src=NULL; D3D11_TEXTURE2D_DESC td; unsigned i; ULONGLONG now;
    if (!c.active || c.stop || g_sb_paused || sb_ovl_open() || !rtv) return;
    now=GetTickCount64();
    if(now-c.begin>=5000) { InterlockedExchange(&c.stop,1); SetEvent(c.wake); return; }
    ID3D11RenderTargetView_GetResource(rtv,&src); if(!src) return;
    ID3D11Texture2D_GetDesc((ID3D11Texture2D *)src,&td);
    if(td.SampleDesc.Count!=1 || (td.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && td.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && td.Format!=DXGI_FORMAT_B8G8R8X8_UNORM)) goto failed;
    if(!c.sw) {
        c.sw=td.Width; c.sh=td.Height; c.fmt=td.Format;
        for(i=0;i<c.w;++i) c.xmap[i]=(i*c.sw/c.w)*4;
        td.Usage=D3D11_USAGE_STAGING; td.BindFlags=0; td.CPUAccessFlags=D3D11_CPU_ACCESS_READ; td.MiscFlags=0;
        for(i=0;i<CS;++i) if(FAILED(ID3D11Device_CreateTexture2D(g_dev,&td,NULL,&c.st[i].tex))) goto failed;
    }
    if(td.Width!=c.sw || td.Height!=c.sh || td.Format!=c.fmt) goto failed; /* resize: ask for a fresh recording, never stretch */
    /* Drain oldest first: reusing a low ring index must not reorder timestamps. */
    for (;;) {
        unsigned j, oldest = 150;
        i = CS;
        if (c.head-c.tail >= CQ) break;
        for (j=0;j<CS;++j) if (c.st[j].pending && c.st[j].at < oldest) { i=j; oldest=c.st[j].at; }
        if (i==CS) break;
        D3D11_MAPPED_SUBRESOURCE m;
        if(SUCCEEDED(ID3D11DeviceContext_Map(g_ctx,(ID3D11Resource *)c.st[i].tex,0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&m))) {
            unsigned x,y; BYTE *dst=c.q[c.head % CQ].rgb; int bgra=c.fmt!=DXGI_FORMAT_R8G8B8A8_UNORM;
            for(y=0;y<c.h;++y) {
                const BYTE *row=(BYTE *)m.pData+(size_t)(y*c.sh/c.h)*m.RowPitch;
                BYTE *d=dst+y*c.w*4;
                for(x=0;x<c.w;++x,d+=4) {
                    const BYTE *p=row+c.xmap[x];
                    d[0]=p[bgra?0:2]; d[1]=p[1]; d[2]=p[bgra?2:0]; d[3]=255;
                }
            }
            ID3D11DeviceContext_Unmap(g_ctx,(ID3D11Resource *)c.st[i].tex,0);
            c.q[c.head % CQ].at=c.st[i].at; c.st[i].pending=0;
            InterlockedIncrement(&c.head); SetEvent(c.wake); ++c.submitted;
        } else { break; }
    }
    if(now>=c.next) for(i=0;i<CS;++i) if(!c.st[i].pending && c.st[i].tex) {
        c.st[i].at=(unsigned)((now-c.begin)*30/1000);
        ID3D11DeviceContext_CopyResource(g_ctx,(ID3D11Resource *)c.st[i].tex,src);
        c.st[i].pending=1; c.next=now+33; break;
    }
    ID3D11Resource_Release(src);
    return;
failed:
    HLOG_W("CLIP: source changed or readback unavailable");
    InterlockedExchange(&c.failed,1);
    InterlockedExchange(&c.stop,1);
    SetEvent(c.wake);
    ID3D11Resource_Release(src);
}
int sb_clip_poll(unsigned *bytes,unsigned *w,unsigned *h)
{
    unsigned i; int result;
    if(!c.active) return 0;
    if(GetTickCount64()-c.begin>=5000 && !c.stop) { InterlockedExchange(&c.stop,1); SetEvent(c.wake); }
    if(!c.done || WaitForSingleObject(c.thread,0)!=WAIT_OBJECT_0) return 0;
    result=c.result; *bytes=c.bytes; *w=c.w; *h=c.h;
    CloseHandle(c.thread); c.thread=NULL; CloseHandle(c.wake); c.wake=NULL;
    for(i=0;i<CQ;++i) { free(c.q[i].rgb); c.q[i].rgb=NULL; }
    for(i=0;i<CS;++i) if(c.st[i].tex) { ID3D11Texture2D_Release(c.st[i].tex); c.st[i].tex=NULL; }
    InterlockedExchange(&c.active,0);
    return result;
}
