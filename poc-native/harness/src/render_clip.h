#pragma once
/* W210: on-demand client-only clip. All calls except active/seconds under ctx_lock. */
struct ID3D11RenderTargetView;
int sb_clip_start(const char *dir);
int sb_clip_active(void);
int sb_clip_seconds(void);
void sb_clip_frame(struct ID3D11RenderTargetView *rtv);
/* Returns 0 pending, 1 MP4, 2 JPG sequence, -1 error. Releases capture resources. */
int sb_clip_poll(unsigned *bytes, unsigned *w, unsigned *h);
void sb_clip_remove(const char *dir);
