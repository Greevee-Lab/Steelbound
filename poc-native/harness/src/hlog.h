#pragma once
/* hlog.h - Thread-sicherer Logger mit Leveln + Call-Drosselung (Perf!). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HLOG_ERROR 0
#define HLOG_WARN 1
#define HLOG_INFO 2
#define HLOG_TRACE 3

void hlog_init(const char *path, int level);
void hlog_close(void);
int hlog_level(void);
void hlog_init_append(const char *path, int level); /* wie hlog_init, haengt aber an (kein CREATE_ALWAYS) */
long hlog_dropped(void);
const char *hlog_path(void); /* W188: Pfad der Log-Datei (leer = keiner) */
void hlog_msg(int level, const char *fmt, ...);
void hlog_async_start(void); /* W168 PERF-TICK hlog_async: Schreib-Thread starten */
void hlog_flush(void);       /* W168: Puffer leeren (bis 200 ms), ohne hlog_async wirkungslos */

/* Drossel: loggt die ersten 3 + jeden 1000. Aufruf einer Stelle */
#define HLOG_CALL_MAX_FIRST 3
int hlog_throttle(uint32_t *counter);

#define HLOG_E(...) hlog_msg(HLOG_ERROR, __VA_ARGS__)
#define HLOG_W(...) hlog_msg(HLOG_WARN, __VA_ARGS__)
#define HLOG_I(...) hlog_msg(HLOG_INFO, __VA_ARGS__)
#define HLOG_T(...) hlog_msg(HLOG_TRACE, __VA_ARGS__)

#ifdef __cplusplus
}
#endif
