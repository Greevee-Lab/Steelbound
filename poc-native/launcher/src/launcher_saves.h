#pragma once
#include <windows.h>
typedef struct ls_plan ls_plan;
typedef struct
{
    int files, saves, pilots, replays, conflicts;
} ls_counts;
/* Paths are UTF-16. Errors are language keys, never source paths. */
ls_plan *ls_open(const wchar_t *source, const wchar_t *hdd, ls_counts *counts, const char **error);
void ls_close(ls_plan *p);
/* Up to eight relative names, no host paths; total count is in ls_counts. */
void ls_conflict_names(const ls_plan *p, wchar_t *out, size_t cap);
/* mode: 0 cancel, 1 overwrite collisions, 2 retain existing collisions. */
int ls_import(ls_plan *p, int mode, const char **error);
int ls_export(const wchar_t *hdd, const wchar_t *zip, const char **error);
int ls_selftest(void);
/* W254 REPLAY-LAUNCHER (experimental): saved replays <hdd>\UDATA\43430002\<id>\st00_0.rep. */
typedef struct
{
    wchar_t id[33];   /* folder name (1..32 hex chars) */
    char name[64];    /* SaveMeta.xbx "Name=" (ASCII), empty = unknown */
    int mission;      /* 1.. from "MISnn" (nn + 1), 0 = unknown */
    FILETIME time;    /* last write time of the .rep (UTC) */
    unsigned size;
    int state;        /* 0 = own replay, 1 = foreign (Xbox) signature, repaired on start, 2 = unknown size */
} ls_replay;
/* Up to cap replays, newest first. Returns the count. */
int ls_replays(const wchar_t *hdd, ls_replay *out, int cap);
int ls_replay_exists(const wchar_t *hdd, const wchar_t *id);
/* Client --set part: "replay_autoplay=<id>[;replay_camera=<0..6>]" (camera < 0 or > 6 = as recorded). */
void ls_replay_set_args(const wchar_t *id, int camera, char *out, size_t cap);
int ls_replay_selftest(void); /* 0 = ok */
/* W255 VIDEO-EXPORT (experimental): resolution presets 0 = 1920x1080, 1 = 2560x1440, 2 = 3840x2160. */
#define LS_VIDEO_RES_N 3
void ls_video_res(int res, unsigned *w, unsigned *h);
/* "1080p30" / "1440p60" / "2160p30" -> res, fps. Returns 1 = ok. */
int ls_video_parse(const char *s, int *res, int *fps);
/* Client --set part: "video_export=<dir>\;video_width=W;video_height=H;video_fps=F" (dir in the
 * ANSI code page like the other paths; empty = "auto" = <photo folder>\Videos). Returns 0 = ok,
 * 1 = dir contains ';' or '"' (would break the --set chain). */
int ls_video_set_args(const char *dir_acp, int res, int fps, char *out, size_t cap);
