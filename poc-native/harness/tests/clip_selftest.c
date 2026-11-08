/* W210: offscreen synthetic capture test, no game, window or desktop capture. */
#include "render_int.h"
#include "render_clip.h"
#include <stdarg.h>
#include <stdlib.h>
ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
unsigned g_bb_w = 1920, g_bb_h = 1080;
hcfg_t g_cfg;
volatile LONG g_sb_paused;
int sb_ovl_open(void) { return 0; }
void hlog_msg(int level, const char *fmt, ...) {
    va_list ap; (void)level; va_start(ap,fmt); vprintf(fmt,ap); va_end(ap); puts("");
}
int main(int argc, char **argv)
{
    ID3D11Texture2D *tex = NULL; ID3D11RenderTargetView *rtv = NULL;
    D3D11_TEXTURE2D_DESC td = {0}; D3D_FEATURE_LEVEL fl;
    ULONGLONG start; unsigned bytes=0,w=0,h=0; int result=0;
    int resize_test=argc>4 && strcmp(argv[4],"resize")==0, resized=0;
    const char *dir = argc>1 ? argv[1] : "clip-selftest";
    CreateDirectoryA(dir,NULL);
    if (argc>3) { g_bb_w=(unsigned)atoi(argv[2]); g_bb_h=(unsigned)atoi(argv[3]); }
    strcpy_s(g_cfg.overlay_test,sizeof g_cfg.overlay_test,"selftest");
    if(FAILED(D3D11CreateDevice(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,D3D11_SDK_VERSION,&g_dev,&fl,&g_ctx))) return 1;
    td.Width=g_bb_w; td.Height=g_bb_h; td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count=1;
    td.BindFlags=D3D11_BIND_RENDER_TARGET;
    if(FAILED(ID3D11Device_CreateTexture2D(g_dev,&td,NULL,&tex)) ||
       FAILED(ID3D11Device_CreateRenderTargetView(g_dev,(ID3D11Resource *)tex,NULL,&rtv))) return 2;
    if(!sb_clip_start(dir)) return 3;
    if(sb_clip_start(dir)) return 5; /* no concurrent recording */
    start=GetTickCount64();
    while(GetTickCount64()-start < 30000 && !result) {
        float t=(float)(GetTickCount64()-start)/5000.0f;
        float color[4]={t,0.25f,1.0f-t,1};
        if (resize_test && !resized && t>0.2f) {
            ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex);
            td.Width/=2;
            if(FAILED(ID3D11Device_CreateTexture2D(g_dev,&td,NULL,&tex)) ||
               FAILED(ID3D11Device_CreateRenderTargetView(g_dev,(ID3D11Resource *)tex,NULL,&rtv))) return 6;
            resized=1;
        }
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx,rtv,color);
        sb_clip_frame(rtv);
        /* Stand-in for REC/F2 drawn after capture: must never reach the file. */
        { float marker[4]={1,0,1,1}; ID3D11DeviceContext_ClearRenderTargetView(g_ctx,rtv,marker); }
        ID3D11DeviceContext_Flush(g_ctx);
        result=sb_clip_poll(&bytes,&w,&h);
        Sleep(8);
    }
    printf("SELFTEST result=%d bytes=%u size=%ux%u active=%d\n",result,bytes,w,h,sb_clip_active());
    ID3D11RenderTargetView_Release(rtv); ID3D11Texture2D_Release(tex);
    ID3D11DeviceContext_Release(g_ctx); ID3D11Device_Release(g_dev);
    if (resize_test) return result==-1 && bytes==0 && !sb_clip_active() ? 0 : 7;
    return result>0 && bytes>0 && bytes<9500000 && w<=1280 && h<=720 && !sb_clip_active() ? 0 : 4;
}
