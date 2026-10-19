#ifndef TIE_APP_SETTINGS_VR_OPTIONS_H
#define TIE_APP_SETTINGS_VR_OPTIONS_H

#include <stdbool.h>

typedef struct AeronUiContext AeronUiContext;

/* The VR page is shown on headsets (and with AERON_XR=1 on the desktop). */
bool TieVrOptions_Available(void);
void TieVrOptions_Draw(AeronUiContext* ui);

#endif
