# Flight HUD — idee da fare

Backlog per l'HUD stile caccia (`flight_hud`). Ogni voce ha uno stato, cosa
deve fare e dove metterci le mani. Aggiornare lo stato quando una voce viene
fatta (e annotare il commit).

Stato: `[ ]` da fare · `[~]` in corso · `[x]` fatto

Tutte le voci sono implementate e controllate nell'anteprima offline; restano
`[~]` finché non sono provate nel gioco vero (Android o PC).

## Riepilogo

| # | Idea | Priorità | Stato |
|---|------|----------|-------|
| 1 | "DESTROYED" + "+1000" all'uccisione | Alta | [~] |
| 2 | Messaggi radio (riquadro con nome + frase) | Media | [~] |
| 3 | Frecce sul bordo per i nemici fuori schermo | Alta | [~] |
| 4 | Banner "MISSION UPDATE" al cambio zona / evento | Media | [~] |
| 5 | Terzo livello di allarme (attacco imminente) | Media | [~] |
| 6 | Schermata di fine missione con voto | Bassa | [~] |
| 7 | Barra salute nel riquadro del bersaglio | Media | [~] |
| 8 | Callsign per i mostri (BANDIT, TGT-01...) | Bassa | [~] |

---

## 1. "DESTROYED" + "+1000" all'uccisione

> **Implementato.** Morte = stesso `charaId`, salute passata da > 0 a <= 0 (non si
> guarda `collision.state`, che può cambiare prima). Entrambi gli stili; il
> "+1000" parte sopra il DESTROYED e vola allo SCORE. Bip = `Sfx_MenuConfirm`,
> solo con `flight_hud_sound`.


- Quando un nemico muore: testo **DESTROYED** proiettato sopra il punto in cui
  è morto (resta ~1,2 s), e un **+1000** che sale verso lo SCORE in alto a
  sinistra. Bip di conferma.
- Rilevare la morte in `Ah_LockScan` (gira ogni frame su tutti gli NPC): uno
  slot che passava `Ah_NpcLive` e ora ha `health <= 0` con lo stesso `charaId`
  è appena morto. Salvare posizione mondo + timestamp in una piccola coda.
- Disegno: `Ah_Project` sulla posizione salvata, `Ah_Text` con `Ah_UseHi`.
- Suono: un `SD_Call(Sfx_...)` del banco base (come `Sfx_MenuConfirm`/`Sfx_MenuMove` già usati).
- Lo SCORE oggi è `(meleeKillCount + rangedKillCount) * 1000`, quindi resta coerente.

## 2. Messaggi radio

> **Implementato.** Tabella `s_radioLines` (parla Cybil), coda di 3, 3,6 s a messaggio,
> 25 s di pausa per tipo, mai la stessa frase due volte di fila. Trigger: lock,
> uccisione, salute sotto 25, caricatore a 0, ultimo flare, nuova zona, boss.
> Modern: in alto al centro (scende a y -172 sul touch con Quick Save/Load);
> Classic: sotto il nastro della prua.


- Riquadro in alto al centro: riga 1 nome (es. "Cybil"), riga 2 la frase tra
  `<< >>`. Dura 3–4 s, coda di massimo 2–3 messaggi, mai lo stesso due volte
  di fila.
- Trigger possibili: primo lock ("Harry, you've got a lock on you!"), prima
  uccisione, salute sotto 25, caricatore a 0, flare finiti, nuovo
  MISSION UPDATE.
- Solo testo, niente audio (non abbiamo voci). Testi in inglese come il resto
  dell'HUD; se serve localizzarli vedere `pc_port/src/lang_*.c`.
- Sul touch evitare la fascia dei pulsanti Quick Save/Load (y ≈ 0.068 dell'altezza)
  se `touch_quicksave_buttons` è attivo.

## 3. Frecce per i nemici fuori schermo

> **Implementato.** `Ah_OffscreenArrows`, entrambi gli stili. Margine 20 unità.


- Per ogni nemico vivo entro `AH_TARGET_RANGE` che `Ah_Project` scarta (dietro
  la camera o fuori dai bordi): triangolino sul bordo dell'area HUD nella
  direzione del nemico, con la distanza accanto.
- Dietro la camera: usare la direzione in view-space (vx, vy) invertita, e
  agganciare la freccia al bordo con un clamp su un rettangolo con margine
  (~20 unità HUD).
- Colore `Ah_UseHi` lampeggiante se quel nemico sta facendo lock (`s_lockState`).
- Utile soprattutto con le camere fisse, dove i mostri sono spesso fuori inquadratura.

## 4. Banner "MISSION UPDATE"

> **Implementato.** Tabelle `s_zoneNames` / `s_mapZone` (le mappe senza nome noto
> danno -1 e niente banner). La zona in cui si carica la partita non viene
> annunciata. Se c'è un debriefing a schermo il banner aspetta. Posizione
> y +104, sotto il centro, in entrambi gli stili.


- Banner centrale con due righe: "MISSION UPDATE" + nome zona (es. "OLD SILENT
  HILL", "MIDWICH ELEMENTARY SCHOOL"). Entra/esce con una linea che si allarga.
- Trigger: cambio di `g_SavegamePtr->mapIdx` (o del mapOverlay caricato) mentre
  si è in gameplay. Serve una tabella mapIdx → nome zona.
- Mostrare una volta per zona per sessione, non a ogni porta.

## 5. Terzo livello di allarme

> **Implementato.** Lock da meno di 2,5 m (`AH_DANGER_RANGE`): bip ogni 0,08 s,
> "EVADE" sotto MISSILE ALERT, bordo rosso pulsante (`Ah_DangerEdge`).
> "Sta attaccando" non è rilevato: la distanza basta e vale per ogni mostro.


- Oggi: WARNING (un nemico ti sta puntando) → MISSILE ALERT (lock fatto).
- Aggiungere un livello quando un nemico con lock è anche molto vicino (< 2–3 m)
  o sta attaccando: bip continui più veloci, bordo dello schermo che pulsa
  rosso (quad semitrasparenti nel batch `s_fill`).
- Non disegnare il bordo pulsante se `low_health_glow` è già attivo e sta
  pulsando, per non sommare due effetti rossi.

## 6. Schermata di fine missione

> **Implementato.** "Missione" = da un debriefing al successivo. Si chiude alla
> morte di un boss (Split Head, Floatstinger, Twinfeeler, Bloodsucker,
> Incubus, Cybil mostro) o al primo ingresso nella sessione in un nuovo
> capitolo (`MAPn` della mappa; l'ordine non è crescente: la città è MAP2, la
> scuola MAP1). Un tratto senza uccisioni né colpi sparati (la passeggiata
> iniziale) non ha debriefing. Pannello di 7 s, non blocca l'input. Voto:
> precisione fino a 60 punti (45 se non si spara) + 4 per uccisione fino a 40;
> S >= 85, A >= 70, B >= 50, altrimenti C. Un caricamento si riconosce dai
> contatori che tornano indietro e fa ripartire le statistiche.


- Dopo un boss o un cambio di capitolo: pannello con TIME, SCORE, nemici
  abbattuti, colpi sparati / a segno, voto (S/A/B/C).
- Dati già disponibili in `s_Savegame`: `gameplayTimer`, `meleeKillCount`,
  `rangedKillCount`, `firedShotCount`, `closeRangeShotCount`,
  `midRangeShotCount`, `longRangeShotCount`.
- Serve definire cosa è una "missione": probabilmente il passaggio tra mappe
  principali o la morte di un boss (individuare i flag evento).
- È la voce più grande: valutare se farla come overlay temporaneo (non deve
  bloccare l'input del gioco).

## 7. Barra salute nel riquadro del bersaglio

> **Implementato.** Modern: sotto il rombo del bersaglio più vicino. Classic: sotto
> il riquadro del più vicino, la distanza scende di una riga.


- Sotto il riquadro del bersaglio principale (`best` in `Ah_Targets`): barra
  sottile con la salute residua del mostro.
- La salute massima cambia per mostro e difficoltà: memorizzare la salute più
  alta vista per quello slot NPC (finché `charaId` non cambia) e usarla come
  100%.

## 8. Callsign per i mostri

> **Implementato.** `flight_hud_callsigns`: 0 nomi, 1 BOGEY / BANDIT (BANDIT quando
> ti sta puntando), 2 TGT-nn per slot. Nelle Opzioni solo sul telefono
> ("Target_Labels", pagina PCOPT_M); sul desktop la pagina HUD è piena, quindi
> solo da config.


- Opzione per sostituire "GROANER", "AIR SCREAMER"... con nomi in codice:
  "BANDIT", "BOGEY", oppure "TGT-01", "TGT-02" per slot.
- Facile: `Ah_EnemyName` + una nuova voce di config (es. `flight_hud_callsigns`).

---

## Dove sta il codice

- `pc_port/src/pc_flight_hud.c` — tutto l'HUD.
  - Logica (thread di gioco, tempo di gioco): `Pc_FlightHud_Update` →
    `Ah_FlareSim`, `Ah_LockScan`, `Ah_Tones`. Agganciato in
    `src/bodyprog/events/game_sys_states.c` accanto a `Pc_CrosshairDraw`.
  - Disegno: `Pc_FlightHud_Draw`, chiamato da `DbgOverlay_Render`
    (`pc_port/src/dbg_overlay.c`) dopo la cattura del frame. GL proprio, con
    salvataggio/ripristino dello stato.
  - Batch di geometria: `s_hud` (con passata ombra), `s_fill` (pannelli
    semitrasparenti, senza ombra), `s_glow` (additivo, per i flare).
  - Coordinate HUD: 480 unità di altezza, origine al centro, +y in basso,
    mezza larghezza `s_w2`.
  - Primitive: `Ah_Line`, `Ah_Box`, `Ah_Rect`, `Ah_Circle`, `Ah_Tri`,
    `Ah_Glow`, `Ah_Text` (font vettoriale `s_glyph`, solo maiuscole, cifre e
    pochi simboli: aggiungere glifi lì se servono).
  - Proiezione mondo → HUD: `Ah_Project` (metri, assi di gioco, Y verso il basso).
  - Stili: Modern = `Ah_BuildHud`, Classic = `Ah_BuildHudClassic`.
    Ogni idea va decisa per entrambi, o solo per Modern.
  - Layout touch: ramo `touch` (`Pc_Touch_IsDrivingInput()`) in entrambi i build.
- Config: `flight_hud` (0 off / 1 modern / 2 classic), `flight_hud_sound`,
  `flight_hud_opacity`, `flight_hud_callsigns` — `pc_port/src/pc_config.c`, `pc_port/include/pc_config.h`,
  documentati in `pc_port/config.cfg`. Riga "Flight HUD" in
  `src/screens/options/options.c` e nel menu rapido (`pc_port/src/pc_quick_options.c`).
  Le pagine Opzioni hanno un tetto di righe: la pagina HUD del telefono è piena.
- Touch: pulsante flare `TB_FLARE` / `TG_C_FLARE` in `pc_port/src/pc_touch.c`.

## Anteprima senza il gioco

`pc_port/tools/flight_hud_preview/` contiene un harness che include
`pc_flight_hud.c`, finge una scena (Harry, tre o quattro nemici, flare) e
scrive i triangoli dell'HUD; `render.py` li rasterizza in PNG.

```
# dalla cartella pc_port/tools/flight_hud_preview, con una build CMake in <build>
CMD=$(cd <build> && ninja -t commands CMakeFiles/SilentHillPC.dir/src/pc_flight_hud.c.o \
      | tail -1 | sed 's/ -MD .*//;s/-fPIE//')
$CMD -Wno-unused-label -o harness harness.c -lSDL2 -lm -Wl,--unresolved-symbols=ignore-all
./harness && python3 render.py   # richiede Pillow e numpy
```

Produce `normal.png`, `alert.png`, `flare.png`, `aim.png`, `classic.png`,
`touch.png`, `events.png` / `events_classic.png` (uccisione, radio, frecce,
callsign numerati), `danger.png` (terzo allarme), `banner.png`, `debrief.png`. Lo sfondo è finto: serve solo a controllare layout e colori.
Prima di chiudere una voce, provarla comunque nel gioco vero (Android o PC).
