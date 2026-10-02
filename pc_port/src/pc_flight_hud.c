/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * pc_flight_hud.c - fighter-jet style HUD (config key: flight_hud).
 *
 *  - A persistent flight-style overlay: heading tape, speed and altitude tapes,
 *    boresight, target containers on enemies, radar, damage and weapon panels,
 *    time and score.
 *  - Lock warning: an enemy that keeps Harry inside its facing cone, in range,
 *    for AH_LOCK_TIME seconds has a lock. While any lock is held "MISSILE ALERT"
 *    flashes in the middle of the screen and every HUD element turns red (the
 *    crosshair and the touch overlay read Pc_FlightHud_AlertActive too).
 *  - Flares: L3+R3 together (or the touch FLARE button) pops a salvo that breaks
 *    every lock and jams new ones for AH_JAM_TIME. Stock recharges over time.
 *
 * Logic runs from the gameplay state (Pc_FlightHud_Update, game time, so a pause
 * freezes it); drawing runs from the post-capture hook with its own GL program
 * and a full state save/restore, the same arrangement as pc_ra_toast.c. Text is
 * a built-in stroke font, so nothing is read from disk and every platform gets
 * the same HUD.
 */
#include "game.h"
#include "bodyprog/bodyprog.h"
#include "bodyprog/sys/joy.h"
#include "bodyprog/sound/sfx_id_enum.h"
#include "bodyprog/sound/sound_system.h"

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <SDL.h>
#include <PsyX/common/glad.h>
#include <PsyX/PsyX_backend.h>

#include "sh_log.h"
#include "pc_config.h"
#include "pc_touch.h"
#include "pc_quick_options.h"
#include "pc_flight_hud.h"

extern MATRIX VbWvsMatrix;
extern long   ReadGeomScreen(void);
extern void   vcGetNowCamPos(VECTOR3* cam_pos);
extern int    PsyX_RawControllerBindHeld(int buttonOrAxis);
extern int    g_PcConsoleInputActive;

#define AH_PI 3.14159265f

/* ------------------------------------------------------------------ */
/* Tuning                                                              */
/* ------------------------------------------------------------------ */

#define AH_LOCK_RANGE     12.0f  /* m */
#define AH_LOCK_CONE      30.0f  /* deg, half-angle of the enemy's facing cone */
#define AH_LOCK_TIME       0.8f  /* s of continuous tracking before the lock */
#define AH_TRACK_DECAY     2.0f  /* tracking time lost per second out of the cone */
#define AH_JAM_TIME        3.0f  /* s a salvo keeps every seeker blind */
#define AH_FLARE_MAX       4
#define AH_FLARE_RECHARGE 10.0f  /* s per flare */
#define AH_SALVO_PAIRS     4
#define AH_SALVO_STEP      0.08f /* s between pairs */
#define AH_TARGET_RANGE   40.0f  /* m, containers beyond this are not drawn */
#define AH_RADAR_RANGE    25.0f  /* m at the radar rim */

#define AH_GRAVITY         5.5f  /* m/s^2, +Y is down */
#define AH_FLARE_DRAG      0.6f
#define AH_TRAIL_N         8
#define AH_TRAIL_STEP      0.035f
#define AH_PARTICLES_MAX   (AH_SALVO_PAIRS * 2 * 3)

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    int   alive;
    float x, y, z;       /* m, game axes (Y down) */
    float vx, vy, vz;
    float floorY;
    float life, lifeMax;
    float trailT;
    int   trailN;
    float tx[AH_TRAIL_N], ty[AH_TRAIL_N], tz[AH_TRAIL_N];
} s_AhFlare;

static float s_lockT[NPC_COUNT_MAX];
static int   s_lockChara[NPC_COUNT_MAX];
static int   s_lockState[NPC_COUNT_MAX]; /* 0 none, 1 tracking, 2 locked */
static int   s_anyTrack, s_anyLock;
static int   s_alert;

static int   s_flareStock = AH_FLARE_MAX;
static float s_rechargeT;
static float s_jamT;
static int   s_salvoLeft;
static float s_salvoT;
static float s_flareMsgT, s_emptyMsgT;
static int   s_flareReq;

static float s_beepT;
static int   s_prevAnyLock;

static s_AhFlare s_flares[AH_PARTICLES_MAX];
static unsigned  s_rng = 0x1234567u;

static float Ah_Rand(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 8) & 0xFFFF) / 65535.0f;
}

static float Ah_Turns(q3_12 a)
{
    return (float)(a & 0xFFF) / 4096.0f;
}

static float Ah_Q12f(s32 v)
{
    return (float)v / 4096.0f;
}

int Pc_FlightHud_Enabled(void)
{
    return g_PcConfig.flightHud != 0;
}

int Pc_FlightHud_AlertActive(void)
{
    return Pc_FlightHud_Enabled() && s_alert;
}

void Pc_FlightHud_FlareRequest(void)
{
    s_flareReq = 1;
}

/* ------------------------------------------------------------------ */
/* Stick-click chord guard                                             */
/* ------------------------------------------------------------------ */

int Pc_FlightHud_StickBindDeferred(int sdlButton)
{
    return Pc_FlightHud_Enabled() &&
           (sdlButton == SDL_CONTROLLER_BUTTON_LEFTSTICK || sdlButton == SDL_CONTROLLER_BUTTON_RIGHTSTICK);
}

int Pc_FlightHud_StickBindEdge(int sdlButton, int held, unsigned char* state)
{
    const int prev  = (*state & 1) != 0;
    int       chord = (*state & 2) != 0;
    int       fire;

    if (!Pc_FlightHud_StickBindDeferred(sdlButton))
    {
        *state = (unsigned char)(held ? 1 : 0);
        return held && !prev;
    }

    if (held)
    {
        int other = (sdlButton == SDL_CONTROLLER_BUTTON_LEFTSTICK) ? SDL_CONTROLLER_BUTTON_RIGHTSTICK
                                                                   : SDL_CONTROLLER_BUTTON_LEFTSTICK;
        if (PsyX_RawControllerBindHeld(other))
            chord = 1;
        *state = (unsigned char)(1 | (chord ? 2 : 0));
        return 0;
    }

    fire   = prev && !chord;
    *state = 0;
    return fire;
}

/* ------------------------------------------------------------------ */
/* Logic                                                               */
/* ------------------------------------------------------------------ */

static int Ah_IsEnemy(int charaId)
{
    if (charaId < Chara_AirScreamer || charaId > Chara_MonsterCybil)
        return 0;
    /* Unused, and the two model-less logic twins of the nurse and doctor. */
    return charaId != Chara_Chicken && charaId != Chara_DummyNurse && charaId != Chara_DummyDoctor;
}

static int Ah_NpcLive(const s_SubCharacter* npc)
{
    return Ah_IsEnemy(npc->model.charaId) && npc->health > Q12(0.0f) &&
           npc->collision.state != 0 && npc->collision.state != 1;
}

static int Ah_InGameplay(void)
{
    return g_GameWork.gameState == GameState_InGame && g_SysWork.sysState == SysState_Gameplay &&
           !(g_SysWork.sysFlags & SysFlag_DemoActive);
}

static void Ah_ResetLocks(void)
{
    int i;
    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        s_lockT[i]     = 0.0f;
        s_lockState[i] = 0;
    }
    s_anyTrack = s_anyLock = s_alert = 0;
}

static void Ah_SpawnOne(int side)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float yaw = Ah_Turns(pl->rotation.vy) * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), py = Ah_Q12f(pl->position.vy), pz = Ah_Q12f(pl->position.vz);
    float back, out, up;
    int   i;

    for (i = 0; i < AH_PARTICLES_MAX && s_flares[i].alive; i++)
        ;
    if (i == AH_PARTICLES_MAX)
        return;

    back = 1.2f + Ah_Rand() * 1.4f;
    out  = 2.2f + Ah_Rand() * 1.6f;
    up   = 3.2f + Ah_Rand() * 1.6f;

    s_flares[i].alive   = 1;
    s_flares[i].x       = px - fx * 0.25f + rx * side * 0.2f;
    s_flares[i].y       = py - 1.25f;
    s_flares[i].z       = pz - fz * 0.25f + rz * side * 0.2f;
    s_flares[i].vx      = -fx * back + rx * side * out;
    s_flares[i].vy      = -up;
    s_flares[i].vz      = -fz * back + rz * side * out;
    s_flares[i].floorY  = py;
    s_flares[i].lifeMax = 2.8f + Ah_Rand() * 0.9f;
    s_flares[i].life    = s_flares[i].lifeMax;
    s_flares[i].trailT  = 0.0f;
    s_flares[i].trailN  = 0;
}

static void Ah_FlareSim(float dt)
{
    int i, k;

    for (i = 0; i < AH_PARTICLES_MAX; i++)
    {
        s_AhFlare* f = &s_flares[i];
        float      drag;
        if (!f->alive)
            continue;

        f->life -= dt;
        if (f->life <= 0.0f)
        {
            f->alive = 0;
            continue;
        }

        f->trailT += dt;
        if (f->trailT >= AH_TRAIL_STEP)
        {
            f->trailT = 0.0f;
            for (k = AH_TRAIL_N - 1; k > 0; k--)
            {
                f->tx[k] = f->tx[k - 1];
                f->ty[k] = f->ty[k - 1];
                f->tz[k] = f->tz[k - 1];
            }
            f->tx[0] = f->x;
            f->ty[0] = f->y;
            f->tz[0] = f->z;
            if (f->trailN < AH_TRAIL_N)
                f->trailN++;
        }

        drag   = 1.0f - AH_FLARE_DRAG * dt;
        f->vx *= drag;
        f->vz *= drag;
        f->vy  = f->vy * drag + AH_GRAVITY * dt;
        f->x  += f->vx * dt;
        f->y  += f->vy * dt;
        f->z  += f->vz * dt;

        if (f->y > f->floorY)
        {
            f->y   = f->floorY;
            f->vy  = -f->vy * 0.25f;
            f->vx *= 0.5f;
            f->vz *= 0.5f;
        }
    }
}

static void Ah_TryLaunch(void)
{
    if (s_salvoLeft > 0)
        return;

    if (s_flareStock <= 0)
    {
        s_emptyMsgT = 1.2f;
        SD_Call(Sfx_MenuError);
        return;
    }

    s_flareStock--;
    s_salvoLeft = AH_SALVO_PAIRS;
    s_salvoT    = 0.0f;
    s_jamT      = AH_JAM_TIME;
    s_flareMsgT = 1.6f;
    Ah_ResetLocks();
    SD_Call(Sfx_MenuConfirm);
    SH_DBG("[FLIGHTHUD] flares away, %d left", s_flareStock);
}

static void Ah_LockScan(float dt)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float cone = AH_LOCK_CONE / 360.0f;
    int i;

    s_anyTrack = s_anyLock = 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        int   tracking = 0;

        if (s_lockChara[i] != npc->model.charaId)
        {
            s_lockChara[i] = npc->model.charaId;
            s_lockT[i]     = 0.0f;
        }

        if (Ah_NpcLive(npc) && s_jamT <= 0.0f && pl->health > Q12(0.0f))
        {
            float dx = Ah_Q12f(pl->position.vx - npc->position.vx);
            float dz = Ah_Q12f(pl->position.vz - npc->position.vz);
            float dist = sqrtf(dx * dx + dz * dz);

            if (dist < AH_LOCK_RANGE)
            {
                float to   = atan2f(dx, dz) / (2.0f * AH_PI);
                float diff = to - Ah_Turns(npc->rotation.vy);
                diff -= floorf(diff + 0.5f);
                tracking = fabsf(diff) <= cone;
            }
        }

        if (tracking)
        {
            s_lockT[i] += dt;
            if (s_lockT[i] > AH_LOCK_TIME)
                s_lockT[i] = AH_LOCK_TIME;
        }
        else
        {
            s_lockT[i] -= dt * AH_TRACK_DECAY;
            if (s_lockT[i] < 0.0f)
                s_lockT[i] = 0.0f;
        }

        if (s_lockT[i] >= AH_LOCK_TIME)
            s_lockState[i] = 2;
        else if (tracking)
            s_lockState[i] = 1;
        else if (s_lockState[i] == 2 && s_lockT[i] > 0.0f)
            s_lockState[i] = 2; /* a lock survives a brief break, like a seeker coasting */
        else
            s_lockState[i] = 0;

        if (s_lockState[i] == 2) s_anyLock = 1;
        else if (s_lockState[i] == 1) s_anyTrack = 1;
    }

    s_alert = s_anyLock;
}

static void Ah_Tones(float dt)
{
    float period;

    if (!g_PcConfig.flightHudSound || (!s_anyLock && !s_anyTrack))
    {
        s_beepT       = 0.0f;
        s_prevAnyLock = s_anyLock;
        return;
    }

    if (s_anyLock && !s_prevAnyLock)
        s_beepT = 0.0f;
    s_prevAnyLock = s_anyLock;

    period = s_anyLock ? 0.16f : 0.55f;
    s_beepT -= dt;
    if (s_beepT <= 0.0f)
    {
        SD_Call(Sfx_MenuMove);
        s_beepT += period;
        if (s_beepT <= 0.0f)
            s_beepT = period;
    }
}

void Pc_FlightHud_Update(void)
{
    float dt;
    int   held, clicked;

    if (!Pc_FlightHud_Enabled())
    {
        Ah_ResetLocks();
        s_flareReq = 0;
        return;
    }

    if (!Ah_InGameplay())
    {
        s_flareReq = 0;
        return;
    }

    dt = Ah_Q12f(g_DeltaTime);
    if (dt < 0.0f) dt = 0.0f;
    if (dt > 0.1f) dt = 0.1f;

    if (s_flareStock < AH_FLARE_MAX)
    {
        s_rechargeT += dt;
        if (s_rechargeT >= AH_FLARE_RECHARGE)
        {
            s_rechargeT = 0.0f;
            s_flareStock++;
        }
    }
    else
    {
        s_rechargeT = 0.0f;
    }

    held    = (int)g_Controller0->heldBtnFlags;
    clicked = (int)g_Controller0->clickedBtnFlags;
    if (!g_PcConsoleInputActive && !g_PcQuickOptionsActive)
    {
        int chord = (held & ControllerFlag_L3) && (held & ControllerFlag_R3) &&
                    (clicked & (ControllerFlag_L3 | ControllerFlag_R3));
        if (chord || s_flareReq)
            Ah_TryLaunch();
    }
    s_flareReq = 0;

    if (s_salvoLeft > 0)
    {
        s_salvoT -= dt;
        if (s_salvoT <= 0.0f)
        {
            Ah_SpawnOne(-1);
            Ah_SpawnOne(1);
            s_salvoLeft--;
            s_salvoT = AH_SALVO_STEP;
        }
    }

    if (s_jamT > 0.0f)       s_jamT -= dt;
    if (s_flareMsgT > 0.0f)  s_flareMsgT -= dt;
    if (s_emptyMsgT > 0.0f)  s_emptyMsgT -= dt;

    Ah_FlareSim(dt);
    Ah_LockScan(dt);
    Ah_Tones(dt);
}

/* ------------------------------------------------------------------ */
/* Geometry batch                                                      */
/* ------------------------------------------------------------------ */

/* HUD space: 480 units tall whatever the resolution, origin at the centre of the
 * picture, +y down. s_w2 is the half width in those units. */
#define AH_VERT_FLOATS 6
#define AH_HUD_VERTS   48000
#define AH_GLOW_VERTS  8000
#define AH_FILL_VERTS  600

static float s_hudV[AH_HUD_VERTS * AH_VERT_FLOATS];
static float s_glowV[AH_GLOW_VERTS * AH_VERT_FLOATS];
static float s_fillV[AH_FILL_VERTS * AH_VERT_FLOATS];

typedef struct
{
    float* v;
    int    n, cap;
} s_AhBatch;

static s_AhBatch s_hud  = { s_hudV, 0, AH_HUD_VERTS };
static s_AhBatch s_glow = { s_glowV, 0, AH_GLOW_VERTS };
static s_AhBatch s_fill = { s_fillV, 0, AH_FILL_VERTS }; /* translucent panels: no shadow pass */
static s_AhBatch* s_cur = &s_hud;

static float s_w2 = 320.0f;
static float s_col[4];

static void Ah_Color(float r, float g, float b, float a)
{
    s_col[0] = r; s_col[1] = g; s_col[2] = b; s_col[3] = a;
}

static void Ah_V(float x, float y, const float* c)
{
    float* p;
    if (s_cur->n >= s_cur->cap)
        return;
    p    = &s_cur->v[s_cur->n * AH_VERT_FLOATS];
    p[0] = x / s_w2;
    p[1] = -y / 240.0f;
    p[2] = c[0]; p[3] = c[1]; p[4] = c[2]; p[5] = c[3];
    s_cur->n++;
}

static void Ah_Tri(float x0, float y0, float x1, float y1, float x2, float y2)
{
    if (s_cur->n + 3 > s_cur->cap)
        return;
    Ah_V(x0, y0, s_col);
    Ah_V(x1, y1, s_col);
    Ah_V(x2, y2, s_col);
}

static void Ah_Quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3)
{
    Ah_Tri(x0, y0, x1, y1, x2, y2);
    Ah_Tri(x0, y0, x2, y2, x3, y3);
}

static void Ah_Rect(float l, float t, float r, float b)
{
    Ah_Quad(l, t, r, t, r, b, l, b);
}

static void Ah_Line(float x0, float y0, float x1, float y1, float th)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    float nx, ny, ex, ey;

    if (len < 0.001f)
    {
        Ah_Rect(x0 - th * 0.5f, y0 - th * 0.5f, x0 + th * 0.5f, y0 + th * 0.5f);
        return;
    }
    nx = -dy / len * th * 0.5f;
    ny =  dx / len * th * 0.5f;
    /* Square caps, so strokes meet without notches at the joints. */
    ex = dx / len * th * 0.5f;
    ey = dy / len * th * 0.5f;
    Ah_Quad(x0 - ex + nx, y0 - ey + ny, x1 + ex + nx, y1 + ey + ny,
            x1 + ex - nx, y1 + ey - ny, x0 - ex - nx, y0 - ey - ny);
}

static void Ah_Box(float l, float t, float r, float b, float th)
{
    Ah_Line(l, t, r, t, th);
    Ah_Line(r, t, r, b, th);
    Ah_Line(r, b, l, b, th);
    Ah_Line(l, b, l, t, th);
}

static void Ah_Circle(float cx, float cy, float rad, float th, int seg)
{
    int i;
    for (i = 0; i < seg; i++)
    {
        float a0 = (float)i / seg * 2.0f * AH_PI;
        float a1 = (float)(i + 1) / seg * 2.0f * AH_PI;
        Ah_Line(cx + cosf(a0) * rad, cy + sinf(a0) * rad, cx + cosf(a1) * rad, cy + sinf(a1) * rad, th);
    }
}

/* Soft disc: opaque centre fading to a transparent rim. */
static void Ah_Glow(float cx, float cy, float rad, const float* inner, const float* outer)
{
    const int seg = 14;
    int i;
    if (s_cur->n + seg * 3 > s_cur->cap)
        return;
    for (i = 0; i < seg; i++)
    {
        float a0 = (float)i / seg * 2.0f * AH_PI;
        float a1 = (float)(i + 1) / seg * 2.0f * AH_PI;
        Ah_V(cx, cy, inner);
        Ah_V(cx + cosf(a0) * rad, cy + sinf(a0) * rad, outer);
        Ah_V(cx + cosf(a1) * rad, cy + sinf(a1) * rad, outer);
    }
}

/* ------------------------------------------------------------------ */
/* Stroke font                                                         */
/* ------------------------------------------------------------------ */

/* Each glyph is a run of segments "x0y0x1y1" on a 4x6 grid, top-left origin.
 * Thin vector strokes are what the real HUD uses, and they scale to any
 * resolution without a texture. */
static const char* const s_glyph[128] = {
    ['A'] = "06020220204242460343",
    ['B'] = "0006003030414142423303333344444545363606",
    ['C'] = "40101001010505161646",
    ['D'] = "000600303041414545363606",
    ['E'] = "4000000606460333",
    ['F'] = "400000060333",
    ['G'] = "413030101001010505161636364545434323",
    ['H'] = "000640460343",
    ['I'] = "103020261636",
    ['J'] = "4045453636161605",
    ['K'] = "000640030346",
    ['L'] = "00060646",
    ['M'] = "0600002323404046",
    ['N'] = "060000464640",
    ['O'] = "10303041414545363616160505010110",
    ['P'] = "060000303041414242333303",
    ['Q'] = "103030414145453636161605050101102446",
    ['R'] = "0600003030414142423333032346",
    ['S'] = "41303010100101020213133333444445453636161605",
    ['T'] = "00402026",
    ['U'] = "00050516163636454540",
    ['V'] = "00262640",
    ['W'] = "0016162323363640",
    ['X'] = "00464006",
    ['Y'] = "002340232326",
    ['Z'] = "004040060646",
    ['0'] = "103030414145453636161605050101104105",
    ['1'] = "112020261636",
    ['2'] = "011010303041414242060646",
    ['3'] = "01101030304141424233331333444445453636161605",
    ['4'] = "363030040444",
    ['5'] = "4000000303333344444545363606",
    ['6'] = "301010010105051616363645454444333303",
    ['7'] = "00404016",
    ['8'] = "103030414142423333131302020101103344444545363616160505040413",
    ['9'] = "431313020201011010303041414545363616",
    ['-'] = "1333",
    ['.'] = "2526",
    ['/'] = "4006",
    ['%'] = "064000101011110101003545454646363635",
    [':'] = "21222425",
    ['<'] = "40030346",
    ['>'] = "00434306",
    ['+'] = "21250343",
    ['['] = "301010161636",
    [']'] = "103030363616",
    ['!'] = "20242526",
    ['^'] = "03202043",
    ['|'] = "2026",
    ['='] = "02420444",
    ['\''] = "2021",
};

#define AH_ADV 5.6f /* grid units per character */

static float Ah_TextWidth(const char* s, float size)
{
    int n = (int)strlen(s);
    return (n > 0) ? ((n * AH_ADV) - (AH_ADV - 4.0f)) * (size / 6.0f) : 0.0f;
}

/* align: 0 left, 1 centre, 2 right. y is the top of the glyphs. */
static void Ah_Text(const char* s, float x, float y, float size, int align)
{
    const float u  = size / 6.0f;
    const float th = (size * 0.12f < 1.1f) ? 1.1f : size * 0.12f;
    float       pen;

    if (align == 1)      x -= Ah_TextWidth(s, size) * 0.5f;
    else if (align == 2) x -= Ah_TextWidth(s, size);
    pen = x;

    for (; *s; s++)
    {
        int         c = (unsigned char)*s;
        const char* g;
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        g = (c < 128) ? s_glyph[c] : NULL;
        for (; g && g[0] && g[1] && g[2] && g[3]; g += 4)
        {
            Ah_Line(pen + (g[0] - '0') * u, y + (g[1] - '0') * u,
                    pen + (g[2] - '0') * u, y + (g[3] - '0') * u, th);
        }
        pen += AH_ADV * u;
    }
}

/* ------------------------------------------------------------------ */
/* Projection                                                          */
/* ------------------------------------------------------------------ */

static VECTOR3 s_cam;
static float   s_camH;
static float   s_halfW; /* PSX units across half the picture (Hor+ widens it) */

/* World (m, game axes) -> HUD units. Returns 0 behind the camera. *outDepth is
 * the view depth in metres, for sizing. */
static int Ah_Project(float wx, float wy, float wz, float* hx, float* hy, float* outDepth)
{
    float dx = wx - Ah_Q12f(s_cam.vx);
    float dy = wy - Ah_Q12f(s_cam.vy);
    float dz = wz - Ah_Q12f(s_cam.vz);
    float vx = (VbWvsMatrix.m[0][0] * dx + VbWvsMatrix.m[0][1] * dy + VbWvsMatrix.m[0][2] * dz) / 4096.0f;
    float vy = (VbWvsMatrix.m[1][0] * dx + VbWvsMatrix.m[1][1] * dy + VbWvsMatrix.m[1][2] * dz) / 4096.0f;
    float vz = (VbWvsMatrix.m[2][0] * dx + VbWvsMatrix.m[2][1] * dy + VbWvsMatrix.m[2][2] * dz) / 4096.0f;

    if (vz < 0.3f)
        return 0;
    *hx = (vx * s_camH / vz) / s_halfW * s_w2;
    *hy = (vy * s_camH / vz) * 2.0f;
    if (outDepth)
        *outDepth = vz;
    return 1;
}

/* ------------------------------------------------------------------ */
/* HUD elements                                                        */
/* ------------------------------------------------------------------ */

static float s_main[4], s_dim[4], s_hi[4];
static float s_th; /* standard stroke */

static void Ah_UseMain(void) { memcpy(s_col, s_main, sizeof(s_col)); }
static void Ah_UseDim(void)  { memcpy(s_col, s_dim,  sizeof(s_col)); }
static void Ah_UseHi(void)   { memcpy(s_col, s_hi,   sizeof(s_col)); }

static const char* Ah_EnemyName(int id)
{
    switch (id)
    {
        case Chara_AirScreamer:     return "AIR SCREAMER";
        case Chara_NightFlutter:    return "NIGHT FLUTTER";
        case Chara_Groaner:         return "GROANER";
        case Chara_Wormhead:        return "WORMHEAD";
        case Chara_LarvalStalker:   return "LARVAL STALKER";
        case Chara_Stalker:         return "STALKER";
        case Chara_GreyChild:       return "GREY CHILD";
        case Chara_Mumbler:         return "MUMBLER";
        case Chara_HangedScratcher: return "HANGED SCRATCHER";
        case Chara_Creeper:         return "CREEPER";
        case Chara_Romper:          return "ROMPER";
        case Chara_SplitHead:       return "SPLIT HEAD";
        case Chara_Floatstinger:    return "FLOATSTINGER";
        case Chara_PuppetNurse:     return "PUPPET NURSE";
        case Chara_PuppetDoctor:    return "PUPPET DOCTOR";
        case Chara_Twinfeeler:      return "TWINFEELER";
        case Chara_Bloodsucker:     return "BLOODSUCKER";
        case Chara_Incubus:         return "INCUBUS";
        case Chara_MonsterCybil:    return "CYBIL";
        default:                    return "BOGEY";
    }
}

static const char* Ah_WeaponName(int id)
{
    switch (id)
    {
        case InvItemId_KitchenKnife: return "KNIFE";
        case InvItemId_SteelPipe:    return "STEEL PIPE";
        case InvItemId_RockDrill:    return "ROCK DRILL";
        case InvItemId_Hammer:       return "HAMMER";
        case InvItemId_Chainsaw:     return "CHAINSAW";
        case InvItemId_Katana:       return "KATANA";
        case InvItemId_Axe:          return "AXE";
        case InvItemId_Handgun:      return "HANDGUN";
        case InvItemId_HuntingRifle: return "RIFLE";
        case InvItemId_Shotgun:      return "SHOTGUN";
        case InvItemId_HyperBlaster: return "HYPER BLASTER";
        default:                     return "UNARMED";
    }
}

/* Box with one end drawn as a point (dir -1 left, +1 right), the shape used
 * for the speed and altitude readouts. */
static void Ah_PointerBox(float cx, float cy, float w, float h, int dir)
{
    const float hw = w * 0.5f, hh = h * 0.5f, tip = hh * 0.8f;
    float l = cx - hw, r = cx + hw, t = cy - hh, b = cy + hh;

    if (dir > 0)
    {
        Ah_Line(l, t, r, t, s_th);
        Ah_Line(r, t, r + tip, cy, s_th);
        Ah_Line(r + tip, cy, r, b, s_th);
        Ah_Line(r, b, l, b, s_th);
        Ah_Line(l, b, l, t, s_th);
    }
    else
    {
        Ah_Line(r, t, l, t, s_th);
        Ah_Line(l, t, l - tip, cy, s_th);
        Ah_Line(l - tip, cy, l, b, s_th);
        Ah_Line(l, b, r, b, s_th);
        Ah_Line(r, b, r, t, s_th);
    }
}

static void Ah_Readout(float cx, const char* title, const char* value, int dir)
{
    Ah_UseMain();
    Ah_Text(title, cx, -24.0f, 8.0f, 1);
    Ah_PointerBox(cx, -3.0f, 46.0f, 15.0f, dir);
    Ah_Text(value, cx, -7.5f, 9.0f, 1);
}

static void Ah_GunReticle(void)
{
    Ah_UseMain();
    Ah_Circle(0.0f, 0.0f, 13.0f, s_th, 28);
    Ah_Line(0.0f, -13.0f, 0.0f, -18.0f, s_th);
    Ah_Line(0.0f, 13.0f, 0.0f, 18.0f, s_th);
    Ah_Line(-13.0f, 0.0f, -18.0f, 0.0f, s_th);
    Ah_Line(13.0f, 0.0f, 18.0f, 0.0f, s_th);
    Ah_Rect(-1.2f, -1.2f, 1.2f, 1.2f);
}

/* Draws every live enemy's container and returns the index of the nearest one
 * (the "TGT"), or -1. *outShoot is set when that target sits under the reticle
 * while Harry is aiming. */
static int Ah_Targets(float nowS, int* outShoot)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float hx[NPC_COUNT_MAX], hy[NPC_COUNT_MAX], dist[NPC_COUNT_MAX];
    int   vis[NPC_COUNT_MAX];
    int   i, best = -1;

    *outShoot = 0;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float wx, wy, wz, dx, dz, depth;

        vis[i] = 0;
        if (!Ah_NpcLive(npc))
            continue;

        wx = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
        wz = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
        wy = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
        dx = wx - Ah_Q12f(pl->position.vx);
        dz = wz - Ah_Q12f(pl->position.vz);
        dist[i] = sqrtf(dx * dx + dz * dz);
        if (dist[i] > AH_TARGET_RANGE)
            continue;
        if (!Ah_Project(wx, wy, wz, &hx[i], &hy[i], &depth))
            continue;
        if (hx[i] < -s_w2 - 40.0f || hx[i] > s_w2 + 40.0f || hy[i] < -280.0f || hy[i] > 280.0f)
            continue;

        vis[i] = 1;
        if (best < 0 || dist[i] < dist[best])
            best = i;
    }

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const float half = 7.0f;
        char  buf[16];
        float x, y;

        if (!vis[i])
            continue;
        x = hx[i];
        y = hy[i];

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();

        Ah_Box(x - half, y - half, x + half, y + half, s_th);

        if (i == best)
        {
            const float d = half + 5.0f;
            Ah_Line(x, y - d, x + d, y, s_th);
            Ah_Line(x + d, y, x, y + d, s_th);
            Ah_Line(x, y + d, x - d, y, s_th);
            Ah_Line(x - d, y, x, y - d, s_th);
        }

        Ah_Text(s_lockState[i] == 2 ? "LOCK" : "TGT", x - half - 4.0f, y - half - 8.0f, 6.0f, 2);
        snprintf(buf, sizeof(buf), "%d", (int)(dist[i] + 0.5f));
        Ah_Text(buf, x + half + 5.0f, y - half, 6.0f, 0);
        if (i == best)
            Ah_Text(Ah_EnemyName(g_SysWork.npcs[i].model.charaId), x + half + 5.0f, y - half + 8.0f, 6.0f, 0);

        if (i == best && g_SysWork.playerCombat.isAiming && dist[i] < 20.0f &&
            fabsf(x) < 60.0f && fabsf(y) < 60.0f)
            *outShoot = 1;
    }

    return best;
}

/* Radar: a dark square window, range rings, heading-up, cardinal letters
 * riding the outer ring. */
static void Ah_Radar(float l, float t, float w, float h, float yawTurns, float nowS)
{
    static const float enemy[4] = { 1.0f, 0.3f, 0.25f, 0.95f };
    static const char* const card[4] = { "N", "E", "S", "W" };
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const float cx = l + w * 0.5f, cy = t + h * 0.55f, r = h * 0.42f;
    float yaw = yawTurns * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), pz = Ah_Q12f(pl->position.vz);
    int   i;

    s_cur = &s_fill;
    Ah_Color(0.0f, 0.18f, 0.05f, 0.45f * s_main[3]);
    Ah_Rect(l, t, l + w, t + h);
    s_cur = &s_hud;

    Ah_UseMain();
    Ah_Box(l, t, l + w, t + h, s_th);
    Ah_UseDim();
    Ah_Circle(cx, cy, r, s_th, 40);
    Ah_Circle(cx, cy, r * 0.5f, s_th * 0.8f, 28);
    Ah_Line(cx - r, cy, cx + r, cy, s_th * 0.7f);
    Ah_Line(cx, cy - r, cx, cy + r, s_th * 0.7f);

    Ah_UseMain();
    for (i = 0; i < 4; i++)
    {
        float a = (i * 0.25f - yawTurns) * 2.0f * AH_PI;
        float lx = cx + sinf(a) * (r - 8.0f), ly = cy - cosf(a) * (r - 8.0f);
        if (lx < l + 4.0f || lx > l + w - 4.0f || ly < t + 4.0f || ly > t + h - 4.0f)
            continue;
        Ah_Text(card[i], lx, ly - 3.5f, 7.0f, 1);
    }

    Ah_Tri(cx, cy - 6.0f, cx - 4.5f, cy + 4.0f, cx + 4.5f, cy + 4.0f);

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float dx, dz, lx, ly, len, bx, by;
        if (!Ah_NpcLive(npc))
            continue;

        dx  = Ah_Q12f(npc->position.vx) - px;
        dz  = Ah_Q12f(npc->position.vz) - pz;
        lx  = dx * rx + dz * rz;
        ly  = dx * fx + dz * fz;
        len = sqrtf(lx * lx + ly * ly);
        if (len > AH_RADAR_RANGE)
        {
            lx *= AH_RADAR_RANGE / len;
            ly *= AH_RADAR_RANGE / len;
        }
        bx = cx + lx / AH_RADAR_RANGE * r;
        by = cy - ly / AH_RADAR_RANGE * r;
        if (bx < l + 3.0f || bx > l + w - 3.0f || by < t + 3.0f || by > t + h - 3.0f)
            continue;

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            memcpy(s_col, enemy, sizeof(s_col));
        Ah_Tri(bx, by - 3.5f, bx - 3.0f, by + 2.5f, bx + 3.0f, by + 2.5f);
    }
}

static int Ah_Kills(void)
{
    return (int)g_SavegamePtr->meleeKillCount + (int)g_SavegamePtr->rangedKillCount;
}

static void Ah_WeaponRow(char* name, char* value, size_t n)
{
    int w = g_SavegamePtr->equippedWeapon;

    snprintf(name, n, "%s", Ah_WeaponName(w));
    if (w == InvItemId_HyperBlaster)
        snprintf(value, n, "INF");
    else if (w >= InvItemId_Handgun && w <= InvItemId_Shotgun)
        snprintf(value, n, "%d/%d", g_SysWork.playerCombat.currentWeaponAmmo,
                 g_SysWork.playerCombat.totalWeaponAmmo);
    else
        snprintf(value, n, "---");
}

/* Green, yellow, orange, red as health drops: the damage colours of a
 * jet HUD's aircraft silhouette. Keeps its own colour under a lock, or the alert would
 * hide how hurt Harry is. */
static void Ah_HealthColor(float hp, float nowS)
{
    const float a = (float)g_PcConfig.flightHudOpacity / 100.0f;

    if (hp >= 75.0f)      Ah_Color(0.45f, 1.0f, 0.55f, 0.95f * a);
    else if (hp >= 50.0f) Ah_Color(1.0f, 0.92f, 0.25f, 0.95f * a);
    else if (hp >= 25.0f) Ah_Color(1.0f, 0.55f, 0.12f, 0.95f * a);
    else                  Ah_Color(1.0f, 0.18f, 0.12f, (fmodf(nowS, 0.6f) < 0.4f ? 0.95f : 0.45f) * a);
}

/* Harry in place of the aircraft: head, torso, arms, legs. (cx, top), h tall. */
static void Ah_Silhouette(float cx, float top, float h)
{
    const float k = h / 60.0f;
    const float limb = 4.5f * k, leg = 5.5f * k;
    int i;

    for (i = 0; i < 8; i++)
    {
        float a0 = i / 8.0f * 2.0f * AH_PI, a1 = (i + 1) / 8.0f * 2.0f * AH_PI;
        Ah_Tri(cx, top + 6.0f * k,
               cx + cosf(a0) * 5.5f * k, top + 6.0f * k + sinf(a0) * 5.5f * k,
               cx + cosf(a1) * 5.5f * k, top + 6.0f * k + sinf(a1) * 5.5f * k);
    }
    Ah_Quad(cx - 10.0f * k, top + 13.5f * k, cx + 10.0f * k, top + 13.5f * k,
            cx + 7.0f * k, top + 33.0f * k, cx - 7.0f * k, top + 33.0f * k);
    Ah_Line(cx - 11.0f * k, top + 15.5f * k, cx - 15.5f * k, top + 33.0f * k, limb);
    Ah_Line(cx + 11.0f * k, top + 15.5f * k, cx + 15.5f * k, top + 33.0f * k, limb);
    Ah_Line(cx - 3.8f * k, top + 34.0f * k, cx - 6.0f * k, top + 57.0f * k, leg);
    Ah_Line(cx + 3.8f * k, top + 34.0f * k, cx + 6.0f * k, top + 57.0f * k, leg);
}

static void Ah_BuildHud(void)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const int    touch  = Pc_Touch_IsDrivingInput();
    const float  nowS   = (float)SDL_GetTicks() / 1000.0f;
    const float  yawT   = Ah_Turns(pl->rotation.vy);
    const float  hp     = Ah_Q12f(pl->health);
    float        dmg    = 100.0f - hp;
    float        speedKmh = fabsf(Ah_Q12f(pl->moveSpeed)) * 3.6f;
    float        altFt    = -Ah_Q12f(pl->position.vy) * 3.28f;
    float        rx;
    int          tgt, shoot;
    char         buf[96], wName[24], wVal[24];

    if (dmg < 0.0f)   dmg = 0.0f;
    if (dmg > 100.0f) dmg = 100.0f;

    s_th = 1.4f;

    rx = s_w2 * 0.38f;
    if (rx > 150.0f) rx = 150.0f;
    snprintf(buf, sizeof(buf), "%d", (int)(speedKmh + 0.5f));
    Ah_Readout(-rx, "SPEED", buf, 1);
    snprintf(buf, sizeof(buf), "%d", (int)floorf(altFt + 0.5f));
    Ah_Readout(rx, "ALT", buf, -1);

    if (!(g_SysWork.playerCombat.isAiming && g_PcConfig.crosshair))
        Ah_GunReticle();

    tgt = Ah_Targets(nowS, &shoot);
    if (shoot)
    {
        Ah_UseMain();
        Ah_Box(-19.0f, 46.0f, 19.0f, 60.0f, s_th);
        Ah_Text("SHOOT", 0.0f, 49.5f, 7.0f, 1);
    }

    Ah_UseMain();
    {
        const float x = -s_w2 + 18.0f;
        Uint32 t = (Uint32)(g_SavegamePtr->gameplayTimer >> 12);
        snprintf(buf, sizeof(buf), "TIME: %02u:%02u:%02u", t / 3600u, (t / 60u) % 60u, t % 60u);
        Ah_Text(buf, x, -224.0f, 8.0f, 0);
        snprintf(buf, sizeof(buf), "SCORE: %d", Ah_Kills() * 1000);
        Ah_Text(buf, x, -210.0f, 8.0f, 0);
        if (tgt >= 0)
        {
            snprintf(buf, sizeof(buf), "TARGET: %s +1000", Ah_EnemyName(g_SysWork.npcs[tgt].model.charaId));
            Ah_Text(buf, x, -196.0f, 8.0f, 0);
        }
    }

    Ah_WeaponRow(wName, wVal, sizeof(wName));

    if (!touch)
    {
        const float colR = s_w2 - 20.0f;
        const float colL = colR - 120.0f;
        const float size = 8.0f;

        Ah_Radar(-s_w2 + 18.0f, 128.0f, 136.0f, 100.0f, yawT, nowS);

        Ah_UseMain();
        Ah_Text(">", colL - 9.0f, 96.0f, size, 0);
        Ah_Text(wName, colL, 96.0f, size, 0);
        Ah_Text(wVal, colR, 96.0f, size, 2);

        Ah_UseMain();
        if (s_flareMsgT > 0.0f)
            Ah_UseHi();
        Ah_Text("FLR", colL, 112.0f, size, 0);
        snprintf(buf, sizeof(buf), "%d", s_flareStock);
        Ah_Text(buf, colR, 112.0f, size, 2);
        if (s_flareStock < AH_FLARE_MAX)
        {
            Ah_UseDim();
            Ah_Rect(colL + 30.0f, 115.0f, colL + 30.0f + 50.0f * (s_rechargeT / AH_FLARE_RECHARGE), 117.0f);
        }

        Ah_UseMain();
        Ah_Text("DMG", colL, 128.0f, size, 0);
        snprintf(buf, sizeof(buf), "%d%%", (int)(dmg + 0.5f));
        Ah_Text(buf, colR, 128.0f, size, 2);

        Ah_HealthColor(hp, nowS);
        Ah_Silhouette(colR - 45.0f, 148.0f, 72.0f);
    }
    else
    {
        /* Touch puts buttons in both bottom corners, so the panels collapse to
         * one row in the free strip at the bottom centre. */
        float w;
        snprintf(buf, sizeof(buf), "DMG %d%%  %s %s  FLR %d", (int)(dmg + 0.5f), wName, wVal, s_flareStock);
        w = Ah_TextWidth(buf, 8.0f);
        Ah_UseMain();
        if (s_flareMsgT > 0.0f)
            Ah_UseHi();
        Ah_Text(buf, 10.0f, 214.0f, 8.0f, 1);
        Ah_HealthColor(hp, nowS);
        Ah_Silhouette(10.0f - w * 0.5f - 14.0f, 196.0f, 30.0f);
    }

    if (s_alert && pl->health > Q12(0.0f))
    {
        if (fmodf(nowS, 0.5f) < 0.32f)
        {
            const float w = Ah_TextWidth("MISSILE ALERT", 20.0f) * 0.5f + 12.0f;
            Ah_UseMain();
            Ah_Text("MISSILE ALERT", 0.0f, -90.0f, 20.0f, 1);
            Ah_Line(-w, -96.0f, w, -96.0f, s_th);
            Ah_Line(-w, -62.0f, w, -62.0f, s_th);
        }
    }
    else if (s_anyTrack)
    {
        Ah_UseMain();
        Ah_Text("WARNING", 0.0f, -82.0f, 11.0f, 1);
    }

    if (s_flareMsgT > 0.0f && fmodf(nowS, 0.2f) < 0.13f)
    {
        Ah_UseHi();
        Ah_Text("FLARE", 0.0f, 70.0f, 10.0f, 1);
    }
    else if (s_emptyMsgT > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO FLARES", 0.0f, 70.0f, 10.0f, 1);
    }
}

/* ------------------------------------------------------------------ */
/* Classic style (flight_hud = 2): tapes, boresight, round radar      */
/* ------------------------------------------------------------------ */

static void Ah_HeadingTape(float headingDeg)
{
    const float cy = -196.0f, half = 120.0f, ppd = 4.0f;
    char  buf[8];
    int   d;
    int   base = (int)floorf(headingDeg / 5.0f) * 5;

    for (d = base - 35; d <= base + 35; d += 5)
    {
        float x = (d - headingDeg) * ppd;
        int   dn = ((d % 360) + 360) % 360;
        if (x < -half || x > half)
            continue;

        Ah_UseDim();
        Ah_Line(x, cy, x, cy + ((dn % 10) == 0 ? 8.0f : 4.0f), s_th);
        if ((dn % 30) == 0)
        {
            const char* lbl = buf;
            switch (dn)
            {
                case 0:   lbl = "N"; break;
                case 90:  lbl = "E"; break;
                case 180: lbl = "S"; break;
                case 270: lbl = "W"; break;
                default:  snprintf(buf, sizeof(buf), "%02d", dn / 10); break;
            }
            Ah_UseMain();
            Ah_Text(lbl, x, cy + 11.0f, 8.0f, 1);
        }
    }

    Ah_UseMain();
    snprintf(buf, sizeof(buf), "%03d", ((int)(headingDeg + 0.5f)) % 360);
    Ah_Box(-17.0f, cy - 22.0f, 17.0f, cy - 6.0f, s_th);
    Ah_Text(buf, 0.0f, cy - 18.5f, 9.0f, 1);
    Ah_Line(0.0f, cy - 6.0f, -4.0f, cy - 1.0f, s_th);
    Ah_Line(0.0f, cy - 6.0f,  4.0f, cy - 1.0f, s_th);
}

/* Vertical tape with a value box. side: -1 left, +1 right; ticks and labels
 * face outward, the box sits on the inner edge. */
static void Ah_Tape(float x, float value, float unitsPerTick, int labelEvery,
                    const char* title, const char* boxText, int side)
{
    const float half = 80.0f, pp = 7.0f;
    char  buf[12];
    int   base = (int)floorf(value / unitsPerTick);
    int   k;

    for (k = base - 13; k <= base + 13; k++)
    {
        float v = k * unitsPerTick;
        float y = -(v - value) / unitsPerTick * pp;
        int   major = (labelEvery > 0) && (k % labelEvery) == 0;
        if (y < -half || y > half)
            continue;

        Ah_UseDim();
        Ah_Line(x, y, x + side * (major ? 9.0f : 5.0f), y, s_th);
        if (major && fabsf(y) > 12.0f)
        {
            snprintf(buf, sizeof(buf), "%d", (int)v);
            Ah_Text(buf, x + side * 12.0f, y - 3.5f, 7.0f, side < 0 ? 2 : 0);
        }
    }

    Ah_UseDim();
    Ah_Line(x, -half, x, half, s_th);

    Ah_UseMain();
    {
        float w = Ah_TextWidth(boxText, 10.0f) + 10.0f;
        float l = (side < 0) ? x + 4.0f : x - 4.0f - w;
        Ah_Box(l, -9.0f, l + w, 9.0f, s_th);
        Ah_Text(boxText, l + w * 0.5f, -5.0f, 10.0f, 1);
    }
    Ah_Text(title, x, -half - 14.0f, 8.0f, 1);
}

static void Ah_Boresight(void)
{
    Ah_UseMain();
    Ah_Line(-16.0f, 0.0f, -8.0f, 0.0f, s_th);
    Ah_Line(-8.0f, 0.0f, -4.0f, 6.0f, s_th);
    Ah_Line(-4.0f, 6.0f, 0.0f, 0.0f, s_th);
    Ah_Line(0.0f, 0.0f, 4.0f, 6.0f, s_th);
    Ah_Line(4.0f, 6.0f, 8.0f, 0.0f, s_th);
    Ah_Line(8.0f, 0.0f, 16.0f, 0.0f, s_th);
}

static void Ah_TargetsClassic(float nowS)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    int i;

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float wx, wy, wz, hx, hy, depth, dist, dx, dz, half;
        char  buf[16];

        if (!Ah_NpcLive(npc))
            continue;

        wx = Ah_Q12f(npc->position.vx + npc->collision.shapeOffsets.box.vx);
        wz = Ah_Q12f(npc->position.vz + npc->collision.shapeOffsets.box.vz);
        wy = Ah_Q12f(npc->position.vy + npc->collision.box.offsetY);
        dx = wx - Ah_Q12f(pl->position.vx);
        dz = wz - Ah_Q12f(pl->position.vz);
        dist = sqrtf(dx * dx + dz * dz);
        if (dist > AH_TARGET_RANGE)
            continue;
        if (!Ah_Project(wx, wy, wz, &hx, &hy, &depth))
            continue;
        if (hx < -s_w2 - 40.0f || hx > s_w2 + 40.0f || hy < -280.0f || hy > 280.0f)
            continue;

        half = 260.0f / depth;
        if (half < 9.0f)  half = 9.0f;
        if (half > 30.0f) half = 30.0f;

        if (s_lockState[i] == 1 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();

        Ah_Box(hx - half, hy - half, hx + half, hy + half, s_th);

        if (s_lockState[i] == 2)
        {
            float d = half + 7.0f;
            Ah_Line(hx, hy - d, hx + d, hy, s_th);
            Ah_Line(hx + d, hy, hx, hy + d, s_th);
            Ah_Line(hx, hy + d, hx - d, hy, s_th);
            Ah_Line(hx - d, hy, hx, hy - d, s_th);
            half = d;
        }

        Ah_Text(Ah_EnemyName(npc->model.charaId), hx, hy - half - 10.0f, 6.5f, 1);
        snprintf(buf, sizeof(buf), "%d", (int)(dist + 0.5f));
        Ah_Text(buf, hx, hy + half + 4.0f, 6.5f, 1);
    }
}

static void Ah_RadarClassic(float cx, float cy, float r, float yawTurns, float nowS)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    float yaw = yawTurns * 2.0f * AH_PI;
    float fx = sinf(yaw), fz = cosf(yaw);
    float rx = cosf(yaw), rz = -sinf(yaw);
    float px = Ah_Q12f(pl->position.vx), pz = Ah_Q12f(pl->position.vz);
    int   i;

    Ah_UseDim();
    Ah_Circle(cx, cy, r, s_th, 40);
    Ah_Circle(cx, cy, r * 0.5f, s_th * 0.7f, 28);
    Ah_Line(cx, cy - r, cx, cy - r + 6.0f, s_th);

    Ah_UseMain();
    Ah_Tri(cx, cy - 6.0f, cx - 4.5f, cy + 4.0f, cx + 4.5f, cy + 4.0f);

    for (i = 0; i < NPC_COUNT_MAX; i++)
    {
        const s_SubCharacter* npc = &g_SysWork.npcs[i];
        float dx, dz, lx, ly, len, bx, by;
        if (!Ah_NpcLive(npc))
            continue;

        dx  = Ah_Q12f(npc->position.vx) - px;
        dz  = Ah_Q12f(npc->position.vz) - pz;
        lx  = dx * rx + dz * rz;
        ly  = dx * fx + dz * fz;
        len = sqrtf(lx * lx + ly * ly);
        if (len > AH_RADAR_RANGE)
        {
            lx *= AH_RADAR_RANGE / len;
            ly *= AH_RADAR_RANGE / len;
        }
        bx = cx + lx / AH_RADAR_RANGE * (r - 3.0f);
        by = cy - ly / AH_RADAR_RANGE * (r - 3.0f);

        if (s_lockState[i] != 0 && fmodf(nowS, 0.3f) < 0.15f)
            Ah_UseHi();
        else
            Ah_UseMain();
        Ah_Rect(bx - 2.5f, by - 2.5f, bx + 2.5f, by + 2.5f);
    }
}

static void Ah_WeaponLines(char* l1, char* l2, size_t n)
{
    int w = g_SavegamePtr->equippedWeapon;

    snprintf(l1, n, "%s", Ah_WeaponName(w));
    if (w == InvItemId_HyperBlaster)
        snprintf(l2, n, "AMMO INF");
    else if (w >= InvItemId_Handgun && w <= InvItemId_Shotgun)
        snprintf(l2, n, "AMMO %d/%d", g_SysWork.playerCombat.currentWeaponAmmo,
                 g_SysWork.playerCombat.totalWeaponAmmo);
    else if (w >= InvItemId_KitchenKnife && w <= InvItemId_Axe)
        snprintf(l2, n, "MELEE");
    else
        l2[0] = '\0';
}

static void Ah_FlareLine(float xRight, float y, float size)
{
    char buf[16];
    float w;
    snprintf(buf, sizeof(buf), "FLR %d", s_flareStock);
    if (s_flareMsgT > 0.0f)
        Ah_UseHi();
    else
        Ah_UseMain();
    Ah_Text(buf, xRight, y, size, 2);

    if (s_flareStock < AH_FLARE_MAX)
    {
        w = Ah_TextWidth(buf, size);
        Ah_UseDim();
        Ah_Rect(xRight - w, y + size + 3.0f,
                xRight - w + w * (s_rechargeT / AH_FLARE_RECHARGE), y + size + 5.5f);
    }
}

static void Ah_BuildHudClassic(float vpW, float vpH)
{
    const s_SubCharacter* pl = &g_SysWork.playerWork.player;
    const int    touch  = Pc_Touch_IsDrivingInput();
    const float  nowS   = (float)SDL_GetTicks() / 1000.0f;
    const float  yawT   = Ah_Turns(pl->rotation.vy);
    const float  hdg    = yawT * 360.0f;
    const float  hp     = Ah_Q12f(pl->health);
    float        dmg    = 100.0f - hp;
    float        speedKmh = fabsf(Ah_Q12f(pl->moveSpeed)) * 3.6f;
    float        altFt    = -Ah_Q12f(pl->position.vy) * 3.28f;
    float        tapeX;
    char         buf[48], l1[24], l2[24];

    (void)vpH;
    if (dmg < 0.0f)   dmg = 0.0f;
    if (dmg > 100.0f) dmg = 100.0f;

    s_th = 1.4f;

    Ah_HeadingTape(hdg);

    tapeX = s_w2 * 0.55f;
    if (tapeX > 190.0f) tapeX = 190.0f;
    snprintf(buf, sizeof(buf), "%d", (int)(speedKmh + 0.5f));
    Ah_Tape(-tapeX, speedKmh, 1.0f, 5, "SPEED", buf, -1);
    snprintf(l1, sizeof(l1), "%d", (int)floorf(altFt + 0.5f));
    Ah_Tape(tapeX, altFt, 2.0f, 5, "ALT", l1, 1);

    if (!(g_SysWork.playerCombat.isAiming && g_PcConfig.crosshair))
        Ah_Boresight();

    Ah_TargetsClassic(nowS);

    Ah_UseMain();
    {
        Uint32 t = (Uint32)(g_SavegamePtr->gameplayTimer >> 12);
        snprintf(buf, sizeof(buf), "TIME %02u:%02u:%02u", t / 3600u, (t / 60u) % 60u, t % 60u);
        Ah_Text(buf, s_w2 - 18.0f, -222.0f, 8.0f, 2);
        snprintf(buf, sizeof(buf), "SCORE %06d", Ah_Kills() * 1000);
        Ah_Text(buf, s_w2 - 18.0f, -208.0f, 8.0f, 2);
    }

    Ah_WeaponLines(l1, l2, sizeof(l1));

    if (!touch)
    {
        const float lx = -s_w2 + 20.0f;
        const float rr = 50.0f;
        const float rcx = s_w2 - 20.0f - rr, rcy = 170.0f;
        const float wx  = rcx - rr - 14.0f;

        Ah_UseMain();
        Ah_Text("DMG", lx, 140.0f, 8.0f, 0);
        snprintf(buf, sizeof(buf), "%d%%", (int)(dmg + 0.5f));
        Ah_Text(buf, lx, 154.0f, 16.0f, 0);
        Ah_UseDim();
        Ah_Box(lx, 178.0f, lx + 110.0f, 186.0f, s_th);
        Ah_UseMain();
        Ah_Rect(lx + 2.0f, 180.0f, lx + 2.0f + 106.0f * (hp < 0.0f ? 0.0f : (hp > 100.0f ? 1.0f : hp / 100.0f)), 184.0f);

        Ah_Text(l1, wx, 146.0f, 9.0f, 2);
        if (l2[0])
            Ah_Text(l2, wx, 162.0f, 9.0f, 2);
        Ah_FlareLine(wx, 178.0f, 9.0f);

        Ah_RadarClassic(rcx, rcy, rr, yawT, nowS);
    }
    else
    {
        /* Touch puts buttons in both bottom corners, so the panels collapse to
         * one row in the free strip at the bottom centre. */
        snprintf(buf, sizeof(buf), "DMG %d%%", (int)(dmg + 0.5f));
        Ah_UseMain();
        Ah_Text(buf, -12.0f, 214.0f, 8.0f, 2);
        snprintf(buf, sizeof(buf), "%s %s", l1, l2);
        Ah_Text(buf, 12.0f, 214.0f, 8.0f, 0);
        Ah_FlareLine(Ah_TextWidth("FLR 4", 8.0f) * 0.5f, 198.0f, 8.0f);
    }

    if (s_alert && pl->health > Q12(0.0f))
    {
        if (fmodf(nowS, 0.5f) < 0.32f)
        {
            const float w = Ah_TextWidth("MISSILE ALERT", 20.0f) * 0.5f + 12.0f;
            Ah_UseMain();
            Ah_Text("MISSILE ALERT", 0.0f, -80.0f, 20.0f, 1);
            Ah_Line(-w, -86.0f, w, -86.0f, s_th);
            Ah_Line(-w, -52.0f, w, -52.0f, s_th);
        }
    }
    else if (s_anyTrack)
    {
        Ah_UseMain();
        Ah_Text("WARNING", 0.0f, -72.0f, 11.0f, 1);
    }

    if (s_flareMsgT > 0.0f && fmodf(nowS, 0.2f) < 0.13f)
    {
        Ah_UseHi();
        Ah_Text("FLARE", 0.0f, 40.0f, 10.0f, 1);
    }
    else if (s_emptyMsgT > 0.0f)
    {
        Ah_UseMain();
        Ah_Text("NO FLARES", 0.0f, 40.0f, 10.0f, 1);
    }

    (void)vpW;
}

static void Ah_BuildFlares(void)
{
    static const float core[4] = { 1.0f, 1.0f, 0.95f, 1.0f };
    int i, k;

    for (i = 0; i < AH_PARTICLES_MAX; i++)
    {
        const s_AhFlare* f = &s_flares[i];
        float hx, hy, depth, rad, a;
        float inner[4], outer[4] = { 1.0f, 0.45f, 0.1f, 0.0f };
        float px, py, pd, qx, qy, qd;

        if (!f->alive || !Ah_Project(f->x, f->y, f->z, &hx, &hy, &depth))
            continue;

        a = f->life / 0.8f;
        if (a > 1.0f) a = 1.0f;
        a *= 0.8f + 0.2f * Ah_Rand();

        /* Burning trail, oldest segment faintest. */
        px = hx; py = hy; pd = depth;
        for (k = 0; k < f->trailN; k++)
        {
            float ta = a * (1.0f - (float)(k + 1) / (AH_TRAIL_N + 1)) * 0.7f;
            if (!Ah_Project(f->tx[k], f->ty[k], f->tz[k], &qx, &qy, &qd))
                break;
            Ah_Color(1.0f, 0.55f, 0.2f, ta);
            Ah_Line(px, py, qx, qy, 2.4f * 3.0f / (pd < 1.0f ? 1.0f : pd) + 0.8f);
            px = qx; py = qy; pd = qd;
        }

        rad = 0.14f * s_camH / depth * 2.0f;
        if (rad < 2.5f)  rad = 2.5f;
        if (rad > 28.0f) rad = 28.0f;

        inner[0] = 1.0f; inner[1] = 0.85f; inner[2] = 0.55f; inner[3] = a;
        Ah_Glow(hx, hy, rad, inner, outer);
        {
            float c[4], o[4];
            memcpy(c, core, sizeof(c));
            c[3] = a;
            o[0] = 1.0f; o[1] = 0.95f; o[2] = 0.8f; o[3] = 0.0f;
            Ah_Glow(hx, hy, rad * 0.35f, c, o);
        }
    }
}

/* ------------------------------------------------------------------ */
/* GL                                                                  */
/* ------------------------------------------------------------------ */

static GLuint s_prog, s_vao, s_vbo;
static GLint  s_locOfs, s_locShadow;
static int    s_glReady;

static GLuint Ah_Shader(GLenum type, const char* src)
{
    GLuint sh = glCreateShader(type);
    GLint  ok = 0;
    const char* parts[2] = { PsyX_Shader_Preamble(type == GL_FRAGMENT_SHADER, PSYX_GLSL_LEGACY), src };
    glShaderSource(sh, 2, parts, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok)
    {
        char log[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[FLIGHTHUD] shader failed: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static void Ah_GlInit(void)
{
    static const char* vs_src =
        "attribute vec2 a_pos;\n"
        "attribute vec4 a_col;\n"
        "uniform vec2 u_ofs;\n"
        "varying vec4 v_col;\n"
        "void main() {\n"
        "    v_col = a_col;\n"
        "    gl_Position = vec4(a_pos + u_ofs, 0.0, 1.0);\n"
        "}\n";
    static const char* fs_src =
        "#ifdef GL_ES\n"
        "precision mediump float;\n"
        "#endif\n"
        "varying vec4 v_col;\n"
        "uniform float u_shadow;\n"
        "void main() {\n"
        "    gl_FragColor = mix(v_col, vec4(0.0, 0.0, 0.0, v_col.a * 0.6), u_shadow);\n"
        "}\n";
    GLuint vs, fs;
    GLint  ok = 0, prevVao = 0, prevBuf = 0;

    s_glReady = -1;

    vs = Ah_Shader(GL_VERTEX_SHADER, vs_src);
    fs = Ah_Shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs)
        return;

    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glBindAttribLocation(s_prog, 0, "a_pos");
    glBindAttribLocation(s_prog, 1, "a_col");
    glLinkProgram(s_prog);
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok)
    {
        char log[512];
        glGetProgramInfoLog(s_prog, (GLsizei)sizeof(log), NULL, log);
        SH_DBG("[FLIGHTHUD] program link failed: %s", log);
        glDeleteProgram(s_prog);
        s_prog = 0;
        return;
    }
    s_locOfs    = glGetUniformLocation(s_prog, "u_ofs");
    s_locShadow = glGetUniformLocation(s_prog, "u_shadow");

    /* Runs before the draw's own state save, so hand back exactly what was bound
     * on entry: PsyCross caches its bindings and would otherwise keep drawing
     * through our buffer (see pc_ra_toast.c). */
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);

    s_glReady = 1;
}

static void Ah_Submit(const s_AhBatch* b)
{
    glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)b->n * AH_VERT_FLOATS * sizeof(float), b->v);
    glDrawArrays(GL_TRIANGLES, 0, b->n);
}

void Pc_FlightHud_Draw(void)
{
    GLint vp[4];
    float vpW, vpH, aspect;
    GLint prevProg = 0, prevVao = 0, prevBuf = 0;
    GLint prevSrcRgb = GL_ONE, prevDstRgb = GL_ZERO, prevSrcA = GL_ONE, prevDstA = GL_ZERO;
    GLint prevEqRgb = GL_FUNC_ADD, prevEqA = GL_FUNC_ADD;
    GLboolean prevBlend, prevDepth, prevCull;
    int   red;

    if (!Pc_FlightHud_Enabled() || !Ah_InGameplay())
        return;
    if (g_PcConsoleInputActive)
        return;

    glGetIntegerv(GL_VIEWPORT, vp);
    if (vp[2] <= 0 || vp[3] <= 0)
        return;
    vpW    = (float)vp[2];
    vpH    = (float)vp[3];
    aspect = vpW / vpH;

    if (!s_glReady)
        Ah_GlInit();
    if (s_glReady != 1)
        return;

    s_w2    = 240.0f * aspect;
    s_halfW = 120.0f * aspect;
    s_camH  = (float)ReadGeomScreen();
    vcGetNowCamPos(&s_cam);
    if (s_camH < 1.0f)
        s_camH = 1.0f;

    red = s_alert && g_SysWork.playerWork.player.health > Q12(0.0f);
    {
        const float o = (float)g_PcConfig.flightHudOpacity / 100.0f;
        if (red)
        {
            s_main[0] = 1.0f;  s_main[1] = 0.22f; s_main[2] = 0.16f; s_main[3] = 0.95f * o;
            s_hi[0]   = 1.0f;  s_hi[1]   = 0.85f; s_hi[2]   = 0.75f; s_hi[3]   = 1.0f * o;
        }
        else
        {
            s_main[0] = 0.45f; s_main[1] = 1.0f;  s_main[2] = 0.55f; s_main[3] = 0.9f * o;
            s_hi[0]   = 1.0f;  s_hi[1]   = 0.9f;  s_hi[2]   = 0.35f; s_hi[3]   = 1.0f * o;
        }
        memcpy(s_dim, s_main, sizeof(s_dim));
        s_dim[3] *= 0.55f;
    }

    s_hud.n  = 0;
    s_glow.n = 0;
    s_fill.n = 0;
    s_cur    = &s_glow;
    Ah_BuildFlares();
    s_cur = &s_hud;
    if (g_PcConfig.flightHud == 2)
        Ah_BuildHudClassic(vpW, vpH);
    else
        Ah_BuildHud();

    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prevBuf);
    glGetIntegerv(GL_BLEND_SRC_RGB, &prevSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &prevDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &prevSrcA);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &prevDstA);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &prevEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &prevEqA);
    prevBlend = glIsEnabled(GL_BLEND);
    prevDepth = glIsEnabled(GL_DEPTH_TEST);
    prevCull  = glIsEnabled(GL_CULL_FACE);

    glUseProgram(s_prog);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, AH_VERT_FLOATS * sizeof(float), (void*)(2 * sizeof(float)));
    glEnable(GL_BLEND);
    /* PsyCross can leave GL_FUNC_REVERSE_SUBTRACT behind after an ABR=2 prim. */
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);

    if (s_fill.n > 0)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        Ah_Submit(&s_fill);
    }

    if (s_glow.n > 0)
    {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        Ah_Submit(&s_glow);
    }

    if (s_hud.n > 0)
    {
        /* A dark copy one pixel down-right first: green strokes alone vanish
         * against the white fog. */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glBufferData(GL_ARRAY_BUFFER, sizeof(s_hudV), NULL, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)s_hud.n * AH_VERT_FLOATS * sizeof(float), s_hud.v);
        {
            const float px = (vpH / 480.0f < 1.0f) ? 1.0f : vpH / 480.0f;
            glUniform2f(s_locOfs, 2.0f * px / vpW, -2.0f * px / vpH);
        }
        glUniform1f(s_locShadow, 1.0f);
        glDrawArrays(GL_TRIANGLES, 0, s_hud.n);
        glUniform2f(s_locOfs, 0.0f, 0.0f);
        glUniform1f(s_locShadow, 0.0f);
        glDrawArrays(GL_TRIANGLES, 0, s_hud.n);
    }

    glBindVertexArray((GLuint)prevVao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint)prevBuf);
    glUseProgram((GLuint)prevProg);
    glBlendFuncSeparate((GLenum)prevSrcRgb, (GLenum)prevDstRgb, (GLenum)prevSrcA, (GLenum)prevDstA);
    glBlendEquationSeparate((GLenum)prevEqRgb, (GLenum)prevEqA);
    if (!prevBlend) glDisable(GL_BLEND);
    if (prevDepth)  glEnable(GL_DEPTH_TEST);
    if (prevCull)   glEnable(GL_CULL_FACE);
}
