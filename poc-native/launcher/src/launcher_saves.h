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
