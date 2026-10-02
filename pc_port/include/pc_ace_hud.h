/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Ace Combat style HUD: persistent flight-style overlay, "MISSILE ALERT" when an
 * enemy locks on to Harry (the whole HUD turns red), and flares on L3+R3 (or the
 * touch FLARE button) that break every lock. Config key: ace_hud. */
#ifndef PC_ACE_HUD_H
#define PC_ACE_HUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* Game-loop tick (gameplay state only): lock tracking, flare input, flare
 * physics and the warning tones. */
void Pc_AceHud_Update(void);

/* Post-capture GL draw, from DbgOverlay_Render. Self-contained GL state. */
void Pc_AceHud_Draw(void);

/* 1 while an enemy holds a lock on Harry. Other HUD elements tint red on it. */
int Pc_AceHud_AlertActive(void);

/* Queue a flare launch from a non-pad source (the touch FLARE button). */
void Pc_AceHud_FlareRequest(void);

/* 1 when the HUD (and so the flare chord) is enabled. */
int Pc_AceHud_Enabled(void);

/* A stick-click bind (Change Camera on R3, Quick Options on L3) must not also
 * fire when that click is half of the L3+R3 flare chord. Call every frame with
 * the bind's SDL controller button and its held level; returns 1 on the frame
 * the bind should act. For a stick button with the HUD on, that is the RELEASE
 * of a click during which the other stick was never pressed; otherwise it is
 * the plain press edge. `state` is caller-owned, zero-initialised. */
int Pc_AceHud_StickBindEdge(int sdlButton, int held, unsigned char* state);

/* 1 when sdlButton is a stick click that Pc_AceHud_StickBindEdge would defer. */
int Pc_AceHud_StickBindDeferred(int sdlButton);

#ifdef __cplusplus
}
#endif

#endif /* PC_ACE_HUD_H */
