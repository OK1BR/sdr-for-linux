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
