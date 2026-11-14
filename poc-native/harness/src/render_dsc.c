/* render_dsc.c - W253 DRAW-OPT: Schatten des Immediate-Contexts (render_dsc.h). */
#include "render_int.h"

sb_dsc_t g_dsc;

void sb_dsc_reset(void)
{
    dsc_inval_all();
}
