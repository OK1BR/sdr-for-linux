# Poznámky z reálného nasazení — závod 2026-08-22 (YO DX HF)

Testovaná binárka: **sdr-for-linux 0.4.1**, `build/sdr-for-linux`
(dev build, ne release), git `1c9761c`. Rádio ANAN G2E na
`192.168.1.247`, protokol 2, RX 14 016,6 kHz @ 1536 kHz, GL renderer
(GTK 4.22). TCI server na portu 40001 obsloužil dva klienty —
skimmer-for-linux a log-for-linux. Ukončeno 21:33, čistě
(`p2: stopped (clean: run=0, FPGA rest, drained)`).

Sesterské dokumenty: `log-for-linux/docs/CONTEST-NOTES-2026-08-22.md`
(3 hlášené položky), `skimmer-for-linux/docs/CONTEST-NOTES-2026-08-22.md`.

**Richard k této aplikaci za dnešek nic nehlásil** — RX, TCI i panadapter
odjely bez připomínky. Níže jen nález z logu.

---

## Poznámky

### N1. GtkImage baseline warnings — 65× za běh
**Nález (log, ne hlášení):** 65 hlášek tvaru

```
Gtk-WARNING: GtkImage 0x… reported baselines of minimum -1 and natural -1,
but sizes of minimum N and natural N. Baselines must be inside the widget size.
```

pokaždé s jinou adresou objektu. Tvořily drtivou většinu všeho, co
aplikace za 2,5 h běhu do stderr napsala. Provozní log je jinak bez
ERROR i CRITICAL; jediná další anomálie je `p2: DUC sequence errors: 2
(+1)` na 26. řádku, tj. při rozjezdu streamu — za zbývajících 2,5 h se
už neopakovala (jediný výskyt v celém logu). Zaznamenáno pro pořádek,
nevypadá to na problém.

**Vyhodnocení:** závažnost **kosmetická**, vizuálně se nic nehlásilo.
Rozbor je společný se skimmerem, kde se objevuje týž warning (18×) —
**detailní zápis a návrh diagnostiky je v
`skimmer-for-linux/docs/CONTEST-NOTES-2026-08-22.md`, položka N2.**
Krátce: neověřeno, jestli je chyba naše nebo regrese v GTK 4.22;
doporučený první krok je backtrace přes `G_DEBUG=fatal-warnings` pod
`gdb`, ne slepá oprava.

→ **Uzavřeno, i s opravou čísel v tomto odstavci: viz „N1 — rozbor
23. 8. 2026" na konci dokumentu.**

### Poznámka k testované binárce
Jelo se z `build/` (dev build, 23,7 MB) — `build-release/` drží starší
verzi ze 17. 8. (20,5 MB). Pro dnešek to nevadilo a CLAUDE.md tuhle cestu
dokumentuje jako běžnou, ale u contest-nasazení stojí za zvážení jet
z release buildu; pokud se v tom bude pokračovat zítra, je to jeden
`meson compile -C build-release`.

---

# 2. den — 23. 8. 2026

Binárka **beze změny** (`build/sdr-for-linux`, dev build, git `1c9761c`).
Spuštěno 12:23, radio ANAN G2E `192.168.1.247` (P2, sw 110.5) nalezeno
hned při discovery, `p2: started dev=1020 ddc=0 @ 14016360 Hz, 1536000 Hz`.
PipeWire sink 192 kHz / 2 ch / quantum 1920, `renderer: GSK_RENDERER=gl
(GTK 4.22)`, TCI server na 40001 obsloužil oba klienty
(`tci: client 0 connected`, `client 1 connected`, `iq_start — 192000 Hz float32`).

**Richard k této appce ani 2. den nic nehlásil.**

**Log 2. dne** (`/var/tmp/contest-2026-08-23-logy/sdr.log`): 295 řádků,
z toho **257 provozních `tx:` řádků** (KEY CW / fwd-rev-SWR / UNKEY).
SWR se drží 1,05–1,21 při špičkách kolem 40–57 W, drive 32/255, ANT1 —
bez anomálie. Kromě toho jediný nálezový řádek, viz níže.

### N1 — aktualizace: 2. den ani JEDEN GtkImage baseline warning
1. den 65 hlášek, 2. den **0**, při nezměněné binárce, nezměněném GTK
(`gtk4 1:4.22.4-1` z 7. 6., žádná pacman transakce 22.–23. 8.) a
prokazatelně stejném prostředí. Detailní rozbor a co z toho plyne pro
triage je v `skimmer-for-linux/docs/CONTEST-NOTES-2026-08-22.md`,
položka **N2 — aktualizace**. Krátce: warning není deterministický
napříč běhy → slepá oprava vyloučená, priorita klesá.

→ **Uzavřeno, viz „N1 — rozbor 23. 8. 2026" na konci dokumentu.**

### N2 (nové, drobné). `p2: DUC sequence errors: 2 (+1)` se zopakovalo
Přesně jako 1. den: jediný výskyt v celém logu, na **25. řádku**, tj. při
rozjezdu streamu; za zbývajících ~2,5 h ani jednou. Reprodukovatelné
napříč oběma dny, vždy jen při startu a vždy stejný počet.

**Vyhodnocení:** závažnost **nízká** (provozně se neprojevilo), ale
**reprodukovatelnost ze dvou nezávislých běhů** ho posouvá z „šum při
rozjezdu" do „drobný, ale skutečný jev při startu DUC". Za samostatnou
opravu to nestojí; hodí se to vzít při nejbližším zásahu do P2 TX cesty
a ověřit, jestli nechybí pár prvních sekvencí při náběhu.

### Poznámka ke spuštění 2. dne
Appky 2. den startoval agent (`setsid nohup ./build/sdr-for-linux`), ne
Richardův obvyklý launcher — proměnné prostředí byly ověřeně shodné
(viz skimmer/N2), ale sám způsob spuštění se lišil. Zaznamenáno pro
případ, že by se rozdíl mezi dny (GtkImage warningy) později ukázal jako
s tím související.

---

## N1 — rozbor 23. 8. 2026 (uzavřeno)

Dohledání příčiny těch baseline warningů, včetně pokusů diagnózu
**vyvrátit**. Sesterský zápis ve `skimmer-for-linux` je jiný repozitář
a tímhle se nemění.

**Závěr:** je to **chování GTK/Panga při selhání font‑metrik**, ne naše
chyba. Kosmetické (GTK si hodnotu samo opraví na −1 a kreslí dál), na
povel nereprodukovatelné, **žádná změna kódu**.

### Oprava čísel z původního zápisu

Původní zápis nahoře mluví o 65 hláškách a baseline −1; obojí je vedle —
znovu ověřeno grepem nad `/var/tmp/contest-2026-08-22-logy/sdr.log`:

| tvrzení nahoře | skutečnost |
|---|---|
| 65 hlášek | **64** (`grep -c "Gtk-WARNING"` = 64, všechny GtkImage; 65. řádek se do počtu nejspíš připletl z `p2: DUC sequence errors` — 64 + 1 = 65 sedí, ale jak ten původní počet vznikl, je rekonstrukce, ne ověřený fakt) |
| „baselines of minimum −1" | **−2147483648**, tedy `INT_MIN`; velikost je vždy 16/16 |
| „pokaždé s jinou adresou" | **32 různých adres, každá právě 2×** |
| skimmer 18× | **16** hlášek = 8 obrázků × 2, po čtyřech ve dvou dávkách (14:08:46 = jeho start, 14:26:06) |

**Tvar dávky (ověřeno):** všech 64 hlášek leží v jediném okně
**14:15:23.453–14:15:23.523** (70 ms) a chodí ve dvou vlnách — každá
adresa jednou v .453–.494 a podruhé v .521–.523. Bezprostředně před nimi
jsou v logu normální `tx: KEY CW` / `UNKEY`, takže je to **uprostřed
běhu**, ne při startu (sdr má PID 643039; skimmer 643441 a jeho první
řádek je 14:08:46). Za zbývajících ~2,5 h se to už neopakovalo, 2. den
nepadla ani jedna (ověřeno: `grep -c GtkImage` = 0 v obou logách z
`/var/tmp/contest-2026-08-23-logy/`).

### Řetěz příčiny — ověřeno ve zdrojích GTK 4.22.4

1. `gtkimage.c:991-1009`, `gtk_image_get_baseline_align()`:
   `baseline_align = (float)ascent / (ascent + descent)`, metriky jdou
   z `pango_context_get_metrics(gtk_widget_get_pango_context(image), NULL, NULL)`.
   Hodnota se cachuje, dokud není `0.0`; `gtk_image_css_changed()`
   (`gtkimage.c:1210-1221`) ji shazuje zpět na `0.0`.
2. `gtkimage.c:1188-1208`, `gtk_image_measure()` pro svislý směr:
   `*minimum_baseline = *minimum * baseline_align;` — tedy `int = int * float`.
3. Když Pango vrátí `ascent + descent == 0`, **dělení je ve floatu**, takže
   nespadne na dělení nulou, ale vyrobí `NaN` (0/0), případně `inf`.
   Přetypování na `int` (x86‑64, `cvttss2si`) dá **přesně `INT_MIN`**.
4. `gtksizerequest.c:379-388` má v sanity‑checku podmínku `min_baseline < 0`,
   takže `INT_MIN` zachytí, vypíše přesně ten text z logu, **baseline shodí
   na −1 a kreslí se dál** → proto jen kosmetika, vizuálně se nic nerozpadlo.

Aritmetika ověřena samostatným programem (`nanmath.c`, `gcc -O2`,
`volatile` aby se to opravdu spočítalo):

```
baseline_align NaN : (int)(16 * -nan) = -2147483648
baseline_align INF : (int)(16 * inf) = -2147483648
baseline_align  OK : (int)(16 * 0.789474) = 12
INT_MIN            = -2147483648
NaN == 0.0f ?      = 0  (0 => GtkImage si NaN zacachuje napořád)
```

Poslední řádek je důležitý: `NaN` projde testem „už je spočteno", takže
se **zacachuje trvale**; poctivá `0.0` by naopak vypadala jako
„nespočteno" a nikdy by nevarovala.

### Reprodukováno, ne jen odvozeno

Vlastní program proti **nainstalovanému** GTK 4.22.4 (`imgbase.c`,
broadway). Se zdravými fonty:

```
[rooted] pango font='Adwaita Sans 14.667px' ascent=14550 descent=3623 sum=18173 -> baseline_align=0,800638
[rooted] measure vertical: min=16 nat=16 min_baseline=12 nat_baseline=12
```

S **prázdným fontsetem** (`FONTCONFIG_FILE` na konfiguraci bez jediného
`<dir>`, `XDG_CACHE_HOME` do scratche — Richardovy fontcache se nesáhlo)
vypadne bajt po bajtu tatáž hláška jako ze závodu:

```
[orphan] pango font='Adwaita Sans 14.667px' ascent=0 descent=0 sum=0 -> baseline_align=-nan
Gtk-WARNING **: GtkImage 0x55c7e82b9730 reported baselines of minimum -2147483648 and
natural -2147483648, but sizes of minimum 16 and natural 16. Baselines must be inside the widget size.
```

Totéž **na naší reálné binárce**: `build/sdr-for-linux` puštěná headless
(broadway, izolované `XDG_CONFIG_HOME`, `SDRFL_RADIO_IP=192.0.2.1` — rádia
se to nedotklo) pod prázdným fontsetem hlásí 4 warningy hned při otevření
okna, About přidá 8 (7 nových obrázků) a Preferences 138 (87 nových).
**Kontrolní běh s normálními fonty, tytéž kroky včetně obou dialogů:
0 warningů.**

### Co se tím vyvrátilo (testováno, ne odhadnuto)

- **„Měří se neukotvený (unrooted) widget."** NE. Obrázek nikdy nevložený
  do okna měří stejně dobře jako obrázek v realizovaném okně — obojí
  `min_baseline=12`.
- **„Může za to naše CSS."** NE. `css_load()` (`src/gui.c:3190-3204`) se
  volá **jednou** při startu (`src/gui.c:4472`), globální písmo nenastavuje
  a jediné, čeho se z fontů dotýká, je `.span { font-family: "Adwaita Mono" }`
  na třech popiskách, ne na ikonách. Změřeno: ani **neexistující**
  font‑family metriky nerozbije (fontconfig substituuje — `ascent=10949
  descent=4070`, `baseline=11`, bez varování) a „Adwaita Mono" je
  nainstalovaná (4 řezy, `fc-list`). Jediná CSS cesta k nulovým metrikám je
  `font-size: 0px` — tu naše CSS nemá nikde (`.dim` má 12 px).
- **„Rozbil to špatný `gtk-xft-dpi`."** Neprokázáno. `gtk-xft-dpi = 0` GTK
  ignoruje; `= 1024` (1 pt/palec) srazí písmo na `0.152px`, ale Pango pořád
  vrací `ascent=151 descent=38` → `baseline=12`, bez varování.
- **„Je to naše ikona."** Naše zdroje nevytvářejí ani jeden `GtkImage`
  (`grep -rn "GtkImage\|gtk_image_" src/` = 0 výskytů); jediný náš dotek
  ikon je `gtk_menu_button_set_icon_name(…, "open-menu-symbolic")`
  (`src/gui.c:4508`) a `icon-name` u šesti stránek Preferences.

### Co zůstává domněnka

- **Co přesně 22. 8. ve 14:15:23 metriky na okamžik rozhodilo.** Z logu se
  to zjistit nedá. Okolní stav systému je ověřený a nic se nehýbalo:
  `gtk4 1:4.22.4-1`, `fontconfig 2:2.18.3-1`, pango 1.58.2;
  v `/var/log/pacman.log` **nula řádků z 21.–23. 8.**; fontcache uživatele
  naposledy z 5. 8., systémová z 11. 8. — obojí dávno před závodem.
- **Co ta dávka 32 obrázků byla.** Adresy chodí v párech ~2000 B od sebe
  (`0x…e688ad0`/`0x…e6892a0`, `0x…e7ffaf0`/`0x…e7ff320`, …), což sedí na
  widget se dvěma `GtkImage` (ikona + indikátor), typicky `GtkModelButton`
  v popover menu. Přiřadit to konkrétnímu prvku se ale nepovedlo: naměřeno
  primární menu ≈ 4 obrázky, About ≈ 7, Preferences ≈ 87 — **32 nesedí ani
  na jedno**. Že se to pak už neozvalo, jde vysvětlit dvojím způsobem
  (widgety zanikly se zavřením popoveru / nebo měření pohltila
  size‑request cache) a z logu se to nerozliší.

### Upstream

GNOME/gtk issue **#5926** „fontconfig files can cause weird measurements
and crash" (otevřeno 2023‑06‑27, zavřeno 2025‑02‑21; ověřeno přes GitLab
API) hlásí **doslova tutéž hlášku i tatáž čísla** (−2147483648 / 16 / 16)
z Fractalu a autor ji odstranil smazáním `~/.var/app/…/cache/fontconfig/`.
Sedí to i s tím, co jsme naměřili: náš pokus s prázdným fontsetem po
otevření Preferences skončil `Pango-CRITICAL: pango_font_get_hb_font:
assertion 'PANGO_IS_FONT (font)' failed` a SIGSEGV — tedy i to „and crash"
z titulku. (Ten pád je artefakt uměle **úplně** bezfontového systému, ne
toho, co se dělo v závodě — tam appka běžela 2,5 h bez problému.)

Kód v `gtkimage.c` **není opravený ani v hlavní větvi GTK** (main:
`gtk_image_get_baseline_align` na ř. 988,
`*minimum_baseline = *minimum * baseline_align;` na ř. 1199) — pořád žádná
kontrola `ascent + descent == 0`.

### Rozhodnutí a recept na znovuotevření

**Žádná změna kódu.** Není co opravit: warning pochází z GTK, my žádný
`GtkImage` nevytváříme a obranný kód v naší aplikaci by na cizím chování
nic nezměnil. Položka se zavírá jako **vysvětlená**, ne „zmizelo to samo".

Kdyby se to vrátilo, tohle chytí **první** výskyt i s backtracem (ne až
tisící):

```sh
G_DEBUG=fatal-warnings gdb -batch -ex run -ex bt --args ./build/sdr-for-linux
```

Když backtrace povede do `gtk_image_measure` → `gtk_image_get_baseline_align`
→ `pango_context_get_metrics` a metriky budou nulové, je to znovu tenhle
jev. První věc ke kontrole je pak **fontcache** (`fc-cache -rv`,
`~/.cache/fontconfig`), ne náš kód.
