# Poznámky z reálného nasazení — závod 2026-07-11

První ostré contest-nasazení alfy 0.1.0-alpha (release build 310dbbd,
instalace ~/.local). Režim: Richard hlásí poznámky za provozu, žádné
zásahy do kódu během závodu — jen zápis + vyhodnocení. Z tohoto seznamu
se po závodě udělá triage do oprav pro příští verzi.

Formát: každá položka = co se stalo (Richardova slova) → vyhodnocení
(závažnost, pravděpodobná příčina, kde v kódu hledat).

---

## Poznámky

### 1. Pravý klik (toggle select/C-tune režimu) nespolehlivý
**Hlášení:** „výběr c-tun pravým klikem myši ne vždy funguje spolehlivě,
někdy musím kliknout vícekrát, aby výběrový filtr na změnu frekvence se
objevil."

**Vyhodnocení:** závažnost **vadí** (contest = časté QSY přes click-tune);
příčina s vysokou jistotou nalezena čtením kódu. Na pravém tlačítku visí
dvě gesta: `rdrag` (zoom) a `rclick` (toggle select mode). Toggle v
`on_right_released` (gui.c:1997) se přeskočí, když `rdrag_zoomed=1` —
jenže `on_rdrag_update` (gui.c:1957) nastavuje `rdrag_zoomed=1` při
JAKÉMKOLI pohybu: GtkGestureDrag nemá práh, drag-update přijde i při
pohybu o 1 px mezi press a release. Rychlé kliknutí reálnou myší se
skoro vždy o pixel hne → toggle tiše spolknut → „musím klikat vícekrát"
(projde jen dokonale nehybný klik).

**Návrh opravy (příští verze):** mrtvá zóna v `on_rdrag_update` — drag
se počítá (a `rdrag_zoomed` nastaví) až od prahu ~6 px (`hypot(off_x,
off_y)`, příp. GTK setting `gtk-dnd-drag-threshold`, default 8 px).
Zoom UX to nepoznamená (octave snap je ~90 px/oktávu, dB zoom má
spojitost od prahu). Jednořádková změna + živé ověření klikáním.

### 2. Spoty na okrajích kolidují s VFO výpisem a S-metrem
**Hlášení:** „spoty na okrajích se překrývají jak nalevo se zobrazením
frekvence, tak napravo s S-metrem."

**Vyhodnocení:** závažnost **kosmetické→vadí** (při zapnutých spotech
v závodě čitelnost obou HUD prvků trpí). Příčina: `draw_spots()`
(gui.c:587) skládá callsign labely do ≤3 řad pod horní hranou spektra
podle frekvence→x, ale nebere ohled na dva HUD obdélníky kreslené
později přes ně: velký VFO výpis vlevo nahoře a `draw_s_meter()`
(gui.c:655) vpravo nahoře (kreslí se až na :1087, tj. NAD spoty —
kolize je oboustranně nečitelná).

**Návrh opravy (příští verze):** spočítat bounding boxy VFO bloku a
S-metru (už se kreslí, rozměry jsou známé v draw_cb) a v `draw_spots()`
label, jehož rect protíná některý HUD rect, přesunout o řadu níž
(row-bump, stejné pravidlo jako řešení kolizí mezi spoty), případně
až za max řady zahodit. Alternativa (jednodušší, horší): spoty pod
HUD + poloprůhledné pozadí HUD. Preferuju row-bump — spot zůstane
čitelný i klikatelný.

### 3. CW náběh přes TCI působí pomalu — změřit latenci
**Hlášení:** „náběh při cw se mi zdá trošku pomalý, zkontrolovat latenci
při odeslání cw sekvence přes tci."

**Vyhodnocení:** závažnost **vadí** (contest CW přes SDC = rytmus výměn).
Zatím hypotézy, chce to MĚŘENÍ, ne dojem. Řetězec: SDC `cw_macros:` →
tci_server.c:509 `s_ops.cw_send()` → tx_gate (MOX) → tx_run → cw_gen
obálka → P2. Kandidáti na fixní zpoždění:
- TX_CHRONO 4-blokový prime exciteru ≈ **42,7 ms** (512 vz. @ 48 k ×4)
  — pokud jím prochází i CW klíčování, je to největší známá položka;
- lws wakeup + fronta mezi TCI threadem a klíčovacím slotem;
- `s_cw_delay_ms` (default 10, „stored" — ověřit, jestli se neaplikuje
  i na start sekvence, ne jen mezi makry);
- radio-side TR sekvence G1 (fyzika, ~ms, neovlivníme);
- druhý výklad „náběhu": tvar/šířka raised-cosine rampy elementu —
  kdyby šlo o attack každé značky, ne o start sekvence.

**Plán měření (po závodě, jednorázová instrumentace):** timestampy
t0=parse cw_macros → t1=MOX intent v gate → t2=první nenulový blok
obálky → t3=první TX paket s RF → t4=první nenulové fwd W; plus
srovnání se sidetonem a s piHPSDR+SDC na témže setupu. Pak rozhodnout,
co je odstranitelné (prime pro CW obejít/zkrátit?).

### 4. Odpadnutí TX→RX po CW: přeslechnutý začátek odpovědi (2–3 znaky)
**Hlášení:** „to samé chce zkontrolovat i odpadnutí, stává se mi, že
přeslechnu začátek navazující konverzace, klidně i o 2–3 znaky."

**Vyhodnocení:** závažnost **vadí hodně** (contest: přeslechnutý začátek
exchange = žádosti o opakování). 2–3 znaky @ ~30 WPM ≈ **0,5–1 s** ticha
po odklíčování — to je řádově víc než jednotlivé známé položky, takže se
nejspíš SČÍTÁ víc věcí:
- break-in **hang** (Prefs → CW; persisted [tx] cw_*) — kolik má
  Richard nastaveno? (workaround: zkrátit),
- 30 ms RF hold za poslední značkou (by-design),
- RX audio **unmute / anti-pump rampa** po TX (F6a mute logika) —
  jak dlouho trvá návrat demodu do slyšitelna,
- AGC recovery po TX (RX vstup byl při TX za 31 dB atenuátory; po
  přepnutí AGC dojíždí),
- případné znovu-rozjetí RX řetězce po odklíčování (zero-feed přechod).

**Plán měření (společně s #3, stejná instrumentace, opačný směr):**
t0=konec poslední obálky → t1=MOX off v gate → t2=TR zpět (RX pakety
tečou) → t3=demod unmute dokončen → t4=RX audio reálně slyšitelné
(RMS na sinku). Rozpočítat, kdo kolik ms žere; cíl pro CW break-in
řádově ≤150 ms celkem. Zkontrolovat, jestli anti-pump rampa není
zbytečně dlouhá pro CW (vs SSB, kde má smysl).

**Dodatek (Richard později během závodu):** měl při CW omylem **AGC
Slow** (zapomněl přepnout na Fast) → pomalé AGC recovery po vlastním
silném TX je pravděpodobný VÝZNAMNÝ přispěvatel k hluchotě (Slow decay
po odklíčování sedí na 0,5–1 s). Bere jako jeden element, ne celou
příčinu. → Měření #4 provést s AGC Fast i Slow, ať se příspěvek AGC
oddělí od systémové latence. Prevence samotné chyby = #8 (per-mode
AGC paměť).

### 5. ★ TÉMA: celková latence v CW a USB — contest-kritické
**Hlášení (direktiva):** „musíme se zaměřit na celkovou latenci v cw a
usb, to je v závodech kritické."

**Vyhodnocení:** povyšuje #3 + #4 z jednotlivých bugů na **prioritní
téma příští verze: latenční audit celého řetězce** pro CW i SSB(USB).
Ne jednotlivé záplaty, ale změřit → rozpočet → cíle → opravit → ověřit.

Matice měření (každé s timestampovou instrumentací + reálné ověření):
| Cesta | CW | USB |
|---|---|---|
| Klíč/PTT → RF na anténě | #3 (TCI makro→RF; + pedál→RF) | pedál/MOX→RF (mic chain + prime) |
| TX → RX turnaround | #4 (konec obálky→slyšitelné RX) | unkey→slyšitelné RX (anti-pump) |
| Mic → monitor (self-latence) | sidetone vs RF zarovnání | monitor zpoždění |
| RX řetězec (DDC→sluchátka) | ~15 ms sink + WDSP bloky — změřit reálně | totéž |

Známé stavební kameny: TX_CHRONO prime 4×512@48k ≈ 42,7 ms; PW sink
~15 ms; mic-ring drift (otevřené, memory); 30 ms RF hold; break-in
hang (nastavitelný); anti-pump rampy; AGC recovery. Reference cíl:
minimálně parita s piHPSDR na témže HW (změřit i piHPSDR!), ideál
CW turnaround ≤150 ms, SSB PTT→RF ≤50 ms.

### 6. ✅ ZACHOVAT: CW Skimmer dekóduje i naše odchozí vysílání
**Hlášení:** „co je naopak velmi praktický, CW skimmer bere i moje
odchozí zprávy, to musíme určitě zachovat, díky tomu se mi ve decode
window u skimmeru lépe orientuje, to jiný appky nedělali."

**Vyhodnocení:** NENÍ bug — **pozitivní emergentní chování, povýšit na
ZÁMĚR + hlídat regresi.** Mechanismus: při CW TX zůstává RX DDC běžet
(vstup za 31 dB atenuátory, ale vlastní TX je i tak silný) a náš TCI IQ
stream teče z nedotčené 1536k DDC nepřetržitě → skimmer v SDC vidí
vlastní odchozí značky inline s pileupem. (Při SSB+PS se DDC0 přepíná
na feedback a RX řetězce se zero-feedují — CW se to netýká, PS je pro
CW carrier vyloučen.)

**⚠ Regresní riziko:** latenční audit (#5) a jakékoli budoucí „mute RX
během TX" / pauzy IQ streamu tohle NESMÍ rozbít. Přidat do tripwires:
**IQ stream během CW TX musí dál nést vysílaný signál** (žádný zero-feed
IQ tapu při CW TX). Ideálně zakódovat do gate testu (tci IQ RMS > práh
při keyed CW), minimálně do živého checklistu před release.

### 7. TX okno: phone utility skrýt v CW, vymyslet CW-specifický HUD
**Hlášení:** „při klíčování CW nejsou potřeba v tx okně vidět utility
pro phone režim, stejně tam nefungují — ukazatel mikrofonního signálu,
alc, šumová brána atd., nechat pouze pro voice režimy… nevím ještě jak
u digitálů… ale pro CW jsou zbytečné, tam zkus vymyslet něco trochu
jiného, pro cw."

**Vyhodnocení:** závažnost **kosmetické→vadí** (clutter + zavádějící —
metry v CW ukazují nic/šum). TX HUD (gui.c, mic peak meter ~:727 +
Proc/gate prvky, footer Mic/Proc skupiny) je dnes mode-agnostický.

**Návrh (příští verze) — mode-aware TX HUD:**
- **Voice (USB/LSB/AM):** jako dnes — mic peak, ALC/komprese, gate,
  PROC; + PEP/SWR.
- **CW:** mic/ALC/gate PRYČ. Místo toho (k diskusi s Richardem):
  **odesílaný text s progresem** — cw_gen/TCI frontu známe, zvýraznit
  právě vysílaný znak + zbytek fronty (contest killer-feature: vidíš,
  co ještě odejde, spolu s možností cw_macros_stop), WPM, stav
  klíče/hang (break-in countdown), sidetone pitch; PEP/SWR zůstává.
- **Digi (DIGU/DIGL):** mic chain je stejně vynucen OFF (clean-chain
  pravidlo) → mic utility skrýt taky; místo nich level meter TCI TX
  audia od klienta + clip indikátor (odhalí přebuzení z Decodia).
  Rozhodnout až po diskusi (Richard: „nevím ještě jak u digitálů").

### 8. Per-mode paměť AGC (CW vs SSB si pamatují svoji hodnotu)
**Hlášení:** „chtělo by to nějak vymyslet, aby si CW a SSB pamatovali
při přepínání svoji hodnotu AGC."

**Vyhodnocení:** závažnost **vadí** (viz dodatek #4 — špatné AGC v CW
reálně stálo přeslechnuté znaky; setting per-mode chybu strukturálně
eliminuje). piHPSDR přesně tohle má (per-mode „modesettings": AGC,
filtr, NR…) — vzor k okopírování.

**Návrh (příští verze):** per-mode-group paměť nastavení: skupiny
{CWL/CWU}, {LSB/USB}, {AM/SAM}, {DIGU/DIGL}, FM až bude. Při přepnutí
módu uložit aktuální AGC (mód Slow/Med/Fast/Long/vyp) pro starou
skupinu a obnovit pro novou; persist do config.ini ([mode.cw] agc=…).
Rozšiřitelný vzor — kandidáti na stejné zacházení hned: AGC-T, NR/NB/
ANF, případně filtr (ten už per-mode presety má). Rozsah 1. kroku
držet malý: jen AGC mód (Richardova žádost), zbytek po diskusi.
Typické defaulty: CW=Fast, SSB=Slow/Med, digi=Fast či off.

### 9. ✅ ZACHOVAT: smazání volačky ve spotlistu SDC ji sundá i ze spektra
**Hlášení:** „pokud smažu volačku ze spotlistu v SDC, tak zmizí i ze
spektra, nevím jestli je to záměr ze specifikace, ale určitě to chci
zanechat, líbí se mi to a je to praktické, zpřehledňuje to spektrum
v tom závodním chaosu."

**Vyhodnocení:** JE to záměr — plná implementace TCI spot API:
tci_server.c:619 zpracovává `spot_delete:callsign` (a :621 i
`spot_clear`) → `s_ops.spot_delete` odstraní spot z overlay. SDC při
smazání ze spotlistu pošle spot_delete a my ho ctíme. Je to ze
specifikace (TCI spot rozhraní), ale ne každá app ho implementuje —
proto to Richard odjinud nezná.

**Regresní pojistka:** zachovat handling spot/spot_delete/spot_clear
1:1; už je krytý offline TCI gatem? → při triage ověřit, že
sdrfl-tci-test pokrývá spot_delete (pokud ne, přidat case). Spolu s #6
tvoří kategorii „emergentní/spec chování ověřené závodem = smluvní".

### 10. ✅ OPRAVENO: picker s dvěma rádii ukázal jen jedno (G1 „zmizela")
**Hlášení:** „výběr rádia nefunguje — zapnul jsem druhé rádio (ANAN 10E),
to správně je unsupported, ale G1 z výpisu úplně zmizela."

**Příčina (potvrzená z logu + kódu):** dedup ve fill_list používal
`dev_ip()` = `inet_ntoa()` = JEDEN statický buffer. `strcmp(ip,
dev_ip(&discovered[j]))` porovnával buffer sám se sebou → vždy shoda →
každé rádio kromě prvního (10E odpověděla dřív) spadlo jako „duplikát".
S jedním rádiem neviditelné — projeví se až druhým kusem v LAN.
**Fix:** kopie IP do lokálního bufferu před smyčkou (picker.c).
Discovery samotné bylo v pořádku (broadcast oba kusy přijal a uložil).

**Bonus zjištění:** 10E hlásí link-local IP **169.254.70.78** — nemá
DHCP lease. Firmware 10E: P2 (device=2 Hermes class, fw 10.3, protokol
P2 v3.1) — potvrzuje Richardovo „10E má P2" pro roadmapu.

### 10b. PROVĚŘIT: 10E má mít IP z routeru, ale hlásí 169.254.x.x
**Hlášení:** „u anan 10e se ukazuje špatná ip adresa… ip adresu by
mělo mít přidělenou z routeru… je to divné."

**Vyhodnocení (k prověření při bring-upu 10E):** 169.254.70.78 je
skutečná zdrojová adresa UDP odpovědi (recvfrom), ne chyba zobrazení —
rádio opravdu vysílá z APIPA adresy. Typická příčina u HPSDR boardů:
**DHCP timeout při bootu** (rádio zapnuto dřív, než naběhl link/DHCP —
Hermes firmware zkouší DHCP jen krátce po startu, pak spadne na
link-local a už se neptá znovu). Ověřit: (1) power-cycle 10E se
zapojeným kabelem a chvíli počkat → nové discovery; (2) DHCP lease
tabulka na routeru (MAC 40:84:32:B1:46:4E); (3) případně statická
rezervace. Náš kód IP jen zobrazuje — oprava pravděpodobně není u nás,
ale zaslouží ověření při bring-upu.

### 10c. ✅ OPRAVENO (kritické): engine startoval discovered[0], ne vybrané rádio
**Hlášení:** „nepřipojí se — radio not found on lan" (po výběru G1
v pickeru, s běžící 10E v LAN).

**Příčina (log potvrdil):** `start_radio` bral slepě
`discovered[selected_device]` (=0, piHPSDR-ism) — s víc rádii v poli
(broadcast kola pickeru se kumulují) to byl kus, který odpověděl
PRVNÍ = 10E. Whitelist ho odmítl → banner „No radio found on the LAN".
⚠ Kdyby druhé rádio bylo podporované, engine by se TIŠE připojil
k jinému kusu, než operátor vybral (a klíčoval by ho!) — whitelist
zafungoval jako druhá obranná linie přesně dle návrhu.
**Fix:** výběr podle zvolené IP (ipaddr_radio match přes discovered[]),
bez pinu první podporované rádio; „pin neodpověděl" = vlastní hláška.

## Po závodě — triage

**Výsledek testu: ~130 CW QSO, aplikace „vcelku použitelná" (Richard).**

Pořadí prací (dávky podle závislostí a rizika):
- **Dávka A — ✅ HOTOVO + ŽIVĚ OVĚŘENO (579a776, tentýž den):**
  A1 = #1 pravý klik (práh ~6 px) — „funguje mnohem lépe";
  A2 = #2 spoty × HUD — „krásně obtékají jak smetr tak i frekvenci";
  A3 = #8 per-mode AGC (defaulty CW/digi=Fast, SSB/AM=Slow; [rx]
  agc_ssb/cw/am/digi) — „taky dobře".
- **Dávka B — ✅ ZMĚŘENO + OPRAVENO + ŽIVĚ OVĚŘENO (574e28b, tentýž
  den):** instrumentace SDRFL_LAT_DEBUG (zůstává v kódu) + 3 opravy:
  (1) cw_gen úvodní mezera z idle = −280 ms KAŽDÉHO overu (SDC makra
  nesou leading space → 7 ditů mrtvého ticha); (2) edge-triggered gate
  (−0..50 ms na obou koncích; SWR/metr kadence 50 ms zachována přes
  fresh_meter); (3) mute řízený enginem z IQ routeru (−110 ms na
  odpadnutí; GUI tick jen backstop, NESMÍ znovu nastavovat g_rx_silence).
  **Naměřeno (35 overů): náběh TCI→RF 320-360 → 32-42 ms** (z toho
  32 = PTT delay, záměr); release 445 → 407 ms. IC-705: první/poslední
  znak čistý; SSB klíčování OK.
  **Kolo 2 (9dd8e5a, tentýž večer): release 407 → 201 ms.** Atribuce
  (keyed_pub + rx_gap značky): těch ~106 ms NEBYL turnaround rádia
  (RX stream nemá jedinou pauzu), ale náš WDSP flush v unkey větvi
  blokující publikaci flagu — unkey se teď publikuje hned při
  p2_set_tx_state(NULL), flush běží dál souběžně. Settle per-mode:
  CW 100 ms (AGC Fast), hlas/TUNE 200 ms. Richard: „svižnější", AGC
  nepumpuje. Výsledek: 201 ms = hang 96 (jeho volba) + settle 100 +
  fade 5 → při 30 WPM &lt;1 znak. Čistě RX-audio timing, TX dráta ani
  ochrany nedotčeny (SWR audit + stale-reading fix viz b1fef7d).
  B3 (piHPSDR reference) neměřeno — není už k rozhodnutí potřeba.
  Bokem opraveno: SWR 2-consecutive filtr počítá jen čerstvá čtení
  (b1fef7d, +2 testy, Richardův bezpečnostní audit).
- **Dávka C — design k diskusi:** #7 CW TX HUD (návrh: sent-text
  s progresem + WPM + hang stav; digi HUD otázka otevřená).
- **Pojistky:** #6 (IQ při CW TX nese signál) + #9 (spot_delete case
  v sdrfl-tci-test) — přidat do gate testů.

Roadmapa po dořešení: viz níže (10E → HL2 → Square SDR).

## Roadmapa po dořešení těchto připomínek (Richardova direktiva, 2026-07-11)

Do **dalšího vydání** přidat podporu rádií (v tomto pořadí):
1. **ANAN 10E** — dle Richarda běží **Protocol 2** (pozn.: dřívější
   rešerše říkala, že řada 10E jede jen P1 — jeho kus zjevně má P2
   firmware; ověřit při bring-upu discovery výpisem). P2 už máme →
   práce = správné Alex/HP bity pro Hermes-class board, PA tabulky,
   whitelist záznam, živé ověření RX i TX (TX-SAFETY checklist).
2. **Hermes Lite 2** — **Protocol 1** = celý nový milestone (P1
   discovery + link + odlišnosti HL2 gateware); zdaleka největší kus.
3. **Square SDR** — modifikace/derivát HL2 (stejný P1 základ, vlastní
   odchylky gateware — zmapovat).

Obě rádia (10E, HL2/Square) má Richard fyzicky u sebe → bring-up
na reálném HW možný. Drží whitelist filozofii: každé rádio až po
otestování, jinak blokované.
