/* loc_dodge.h - W380 LOC-DODGE: DIAG-Zaehler fuer den Seitenschritt (siehe loc_dodge.c). */
#ifndef LOC_DODGE_H
#define LOC_DODGE_H

void loc_dodge_frame(void); /* je Lockstep-Rahmen aus dem Spiel-Thread (ls_record); ohne loc_dodge_diag ohne Wirkung */
void loc_dodge_log(void);   /* Zaehlerzeile LOCDODGE (auch am Ende) */

#endif
