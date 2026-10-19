/*
 * The game side of the VR lap console, quick wheel and side panel: the
 * player's craft as TieVrConsoleState, and console presses as TIE's own
 * input actions (the same path as the key bindings and voice commands).
 * TIE port of OpenXWA-VR's XwaPort_GetVrConsoleState /
 * XwaPort_VrConsoleAction (xwa_runtime/runtime/port.c).
 */
#ifndef TIE_RUNTIME_INPUT_VR_CONSOLE_GAME_H
#define TIE_RUNTIME_INPUT_VR_CONSOLE_GAME_H

#include "tie_runtime/input/vr_console_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* This frame's state (valid = 0 outside cockpit flight or without a
 * headset). Call after TieVrControls_Frame. */
void TieVrConsoleGame_GetState(TieVrConsoleState* out);

/* A console press. Ignored outside flight. */
void TieVrConsoleGame_Action(TieVrConsoleAction action, int arg);

#ifdef __cplusplus
}
#endif

#endif
