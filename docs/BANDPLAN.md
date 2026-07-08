# Amateur band-plan reference (for the region-selectable band display)

**Status:** reference data for a *planned* feature (region-selectable band edges +
segment overlays on the spectrum; later, an out-of-band TX interlock). Not yet
implemented.

**Provenance:** compiled from the official IARU Region 1/2/3 band-plan PDFs, the
ARRL US allocation chart, and Czech (ČTÚ / Český radioklub) sources. Frequencies
are *transmitted* frequencies (not suppressed-carrier dial). See the Sources list
at the end.

> ⚠ **Before any of this drives a TX interlock**, re-verify the volatile values
> (esp. OK 4 m / 60 m edges + power/permit conditions, and the 2020 Novi Sad R1
> amendments) against the live regulator notice. The segment table is for
> **display only**; a hard TX refusal must come from the band-edge + national
> override tables in integer Hz.

---

## 1. IARU Regions

| Region | Coverage |
|---|---|
| **Region 1** | Europe, Africa, Middle East, Northern Asia (former-USSR north). *(Czech Republic / OK1BR.)* |
| **Region 2** | The Americas + Greenland + Pacific east of the dateline. |
| **Region 3** | Asia south/east of R1, Oceania, Pacific west of the dateline. |

---

## 2. Band edges per region (⚠ = region difference a TX guard must respect)

| Band | R1 | R2 | R3 | Notes |
|---|---|---|---|---|
| 2200 m | 135.7–137.8 kHz | same | same | secondary |
| 630 m | 472–479 kHz | same | same | secondary |
| 160 m ⚠ | 1810–2000 kHz (plan); many nat'l 1800 | 1800–2000 | 1800–2000 | R1 plan starts 1810; OK runs 1800 |
| 80 m ⚠ | 3500–3800 | 3500–**4000** | 3500–**3900** | top edge differs |
| 60 m | 5351.5–5366.5 kHz | same | same | WRC-15 secondary 15 W EIRP; often channelized |
| 40 m ⚠ | 7000–7200 | 7000–**7300** | 7000–7200 | R2 extends to 7300 |
| 30 m | 10100–10150 | same | same | secondary |
| 20 m | 14000–14350 | same | same | |
| 17 m | 18068–18168 | same | same | |
| 15 m | 21000–21450 | same | same | |
| 12 m | 24890–24990 | same | same | |
| 10 m | 28000–29700 | same | same | |
| 6 m ⚠ | 50–**52** MHz | 50–**54** | 50–**54** | R1 baseline 50–52 (OK = 50–52) |
| 4 m ⚠ | 70.0–70.5 MHz | — none — | — none — | **Region 1 only** |
| 2 m ⚠ | 144–**146** | 144–**148** | 144–**148** | R1 top 146 |
| 70 cm ⚠ | 430–**440** | 430–**450** | 430–440 | R2 to 450 (US 420–450) |

Sideband convention (R1 HF): below 10 MHz **LSB**, above **USB**; 60 m = **USB**.

---

## 3. IARU Region 1 HF segments (complete) — Hz

`max_bw` = max occupied bandwidth (Hz). Usage: CW / NB (narrow ≤500) / DIGI /
ALL (all modes) / BEACON / SAT / FM / ACDS (unattended data). CoA = centre of
activity.

```
band   seg_lo     seg_hi     max_bw  usage          label
160m   1810000    1838000    200     CW             1836 CW-QRP CoA
160m   1838000    1840000    500     NB
160m   1840000    1843000    2700    ALL            digimodes
160m   1843000    2000000    2700    ALL
60m    5351500    5354000    200     CW|NB
60m    5354000    5366000    2700    ALL            USB voice
60m    5366000    5366500    20      NB             weak-signal
80m    3500000    3510000    200     CW             DX/contest
80m    3510000    3560000    200     CW             3555 QRS CoA
80m    3560000    3570000    200     CW             3560 QRP CoA
80m    3570000    3580000    200     NB|DIGI
80m    3580000    3590000    500     NB|DIGI
80m    3590000    3600000    500     NB|DIGI|ACDS
80m    3600000    3650000    2700    ALL            3630 DV CoA; ACDS 3600-3620
80m    3650000    3700000    2700    ALL            3690 SSB-QRP CoA
80m    3700000    3775000    2700    ALL            3735 Image; 3760 R1-Emerg
80m    3775000    3800000    2700    ALL            DX priority
40m    7000000    7040000    200     CW             7030 CW-QRP CoA
40m    7040000    7047000    500     NB|DIGI
40m    7047000    7050000    500     NB|DIGI|ACDS
40m    7050000    7053000    2700    ALL            digimodes/ACDS
40m    7053000    7060000    2700    ALL
40m    7060000    7100000    2700    ALL            7070 DV; 7090 SSB-QRP
40m    7100000    7130000    2700    ALL            7110 R1-Emergency CoA
40m    7130000    7175000    2700    ALL            SSB contest
40m    7175000    7200000    2700    ALL            7165 Image; DX priority
30m    10100000   10130000   200     CW             10116 QRP CoA
30m    10130000   10150000   500     NB|DIGI
20m    14000000   14060000   200     CW             14055 QRS CoA
20m    14060000   14070000   200     CW             14060 QRP CoA
20m    14070000   14089000   500     NB|DIGI
20m    14089000   14099000   500     NB|DIGI|ACDS
20m    14099000   14101000   0       BEACON         IBP exclusive
20m    14101000   14112000   2700    ALL|ACDS
20m    14112000   14125000   2700    ALL
20m    14125000   14300000   2700    ALL            14130 DV;14195 DX;14230 Img;14285 QRP
20m    14300000   14350000   2700    ALL            14300 Global-Emergency CoA
17m    18068000   18095000   200     CW             18086 QRP
17m    18095000   18105000   500     NB|DIGI
17m    18105000   18109000   500     NB|DIGI|ACDS
17m    18109000   18111000   0       BEACON         IBP exclusive
17m    18111000   18120000   2700    ALL|ACDS
17m    18120000   18168000   2700    ALL            18130 QRP;18150 DV;18160 Emerg
15m    21000000   21070000   200     CW             21055 QRS;21060 QRP
15m    21070000   21090000   500     NB|DIGI
15m    21090000   21110000   500     NB|DIGI|ACDS
15m    21110000   21120000   2700    ALL|ACDS       not SSB
15m    21120000   21149000   500     NB
15m    21149000   21151000   0       BEACON         IBP exclusive
15m    21151000   21450000   2700    ALL            21180 DV;21285 QRP;21340 Img;21360 Emerg
12m    24890000   24915000   200     CW             24906 QRP
12m    24915000   24925000   500     NB|DIGI
12m    24925000   24929000   500     NB|DIGI|ACDS
12m    24929000   24931000   0       BEACON         IBP exclusive
12m    24931000   24940000   2700    ALL|ACDS
12m    24940000   24990000   2700    ALL            24950 QRP;24960 DV
10m    28000000   28070000   200     CW             28055 QRS;28060 QRP
10m    28070000   28120000   500     NB|DIGI
10m    28120000   28150000   500     NB|DIGI|ACDS
10m    28150000   28190000   500     NB
10m    28190000   28225000   0       BEACON         IBP exclusive (regional/worldwide/continuous)
10m    28225000   28300000   2700    ALL|BEACON
10m    28300000   28320000   2700    ALL|ACDS
10m    28320000   29000000   2700    ALL            28330 DV;28360 QRP;28680 Img
10m    29000000   29100000   6000    ALL
10m    29100000   29200000   6000    ALL|FM         FM simplex 10k ch
10m    29200000   29300000   6000    ALL|ACDS
10m    29300000   29510000   6000    SAT            satellite links
10m    29510000   29520000   0       -              guard channel
10m    29520000   29590000   6000    FM             repeater input RH1-8
10m    29590000   29620000   6000    FM             29600 FM calling
10m    29620000   29700000   6000    FM             repeater output RH1-8
```
2200 m / 630 m single-segment: `2200m 135700 137800 200 CW`, `630m 472000 479000 500 CW|DIGI`.

### R2/R3 material HF deltas
- **160 m:** R2 adds 1800–1810 digi, CW DX 1830–1840, SSB DX 1840–1850. R3 coarser.
- **80 m:** R2 to 4000 (3885 AM CoA, 3985 Emerg); R3 to 3900 (3795 DX-phone, 3845 Image).
- **40 m:** R2 extra 7200–7300 all-modes (7285 SSB-QRP, 7290 AM, 7240/7275 Emerg).
- 30/20/17/15/12/10 m harmonized apart from CoA labels.

---

## 4. IARU R1 VHF/UHF segment highlights (calling freqs + repeater/sat sub-bands)

**6 m (50–52 R1):** 50.000–50.100 CW/beacons (50.090 CW CoA); 50.100–50.130 SSB DX
(**50.110 DX call**); 50.150 **SSB CoA**; 50.400–50.500 beacons excl; 50.510 SSTV;
**51.510 FM call**; 51.210–51.390 rptr in, 51.810–51.990 rptr out.

**4 m (70.0–70.5, R1 only):** 70.000–70.090 CW/beacons; **70.200 CW/SSB call**;
70.260 AM/FM call; **70.450 FM call**.

**2 m (144–146):** 144.000–144.025 sat DL; 144.050 CW CoA; **144.300 SSB call**;
144.400–144.490 beacons excl; **144.800 APRS**; 144.975–145.194 rptr in; **145.500
FM call**; 145.5625–145.7935 rptr out; 145.806–146.000 **sat excl**.

**70 cm (430–440):** 432.000–432.100 CW/EME (432.050 CoA); **432.200 SSB call**;
432.400–432.490 beacons excl; **433.500 FM call**; 435.000–438.000 **satellite**;
438.000–440.000 ATV/FM/rptr out.

---

## 5. National overrides (deltas from region baseline)

### USA (Region 2, class-gated: Extra/Advanced/General/Technician)
- 80 m: Tech CW 3.525–3.600; Gen phone 3.800–4.000; Extra low edge 3.500.
- 40 m: **phone to 7.300** (R2 edge); Extra CW from 7.000; Gen phone 7.175–7.300.
- 60 m: **5 fixed channels** 5330.5 / 5346.5 / 5357.0 / 5371.5 / 5403.5 kHz,
  USB/CW/data, 100 W ERP, carrier 1.5 kHz below centre.
- 30 m: data-only, 200 W. 10 m: Tech phone from 28.300. **70 cm = 420–450 MHz.**
- Source: ARRL frequency-allocations chart.

### Czech Republic (OK — ČTÚ / Český radioklub), Region 1
| Band | OK | Notes |
|---|---|---|
| 160 m | **1800–2000 kHz** | wider low edge than the 1810 R1 table |
| 80 m | 3500–3800 | R1 standard |
| 60 m | 5351.5–5366.5 kHz | secondary, **15 W EIRP**, individual permit, USB voice |
| 40 m | 7000–7200 | R1 standard |
| 6 m | 50–52 MHz | R1 baseline |
| 4 m | **~70.1–70.3 MHz** | secondary, **10 W ERP**, individual permit (annual). ⚠ most volatile — re-check live ČTÚ notice |
| 2 m | 144–146 | R1 |
| 70 cm | 430–440 | R1 |

---

## 6. Data-model recommendation (C app)

Keep everything in **Hz (int64)** so integer comparisons drive the guard with no
float error; format kHz/MHz only in the UI.

```c
typedef enum { R1, R2, R3 } iaru_region_t;

typedef struct { const char *band; int64_t f_lo, f_hi; } band_edge_t;      // per (region, band)

typedef struct {                       // per segment within a band, ordered by seg_lo
    const char *band;
    int64_t seg_lo, seg_hi;            // Hz
    int32_t max_bw;                    // Hz, 0 = unspecified
    uint16_t usage;                    // CW|NB|DIGI|PHONE|BEACON|SAT|IMAGE|FM|DV|ACDS|EMERGENCY
    const char *label;                 // human tag / CoA note
} band_segment_t;

typedef struct {                       // national override, wins over region edges
    const char *country;               // "US","CZ"
    const char *lic_class;             // NULL = all; else "E","A","G","T"
    const char *band;
    int64_t f_lo, f_hi;
    uint16_t usage;
    const char *note;
} nat_override_t;
```

Usage bits: `CW=1<<0, NB=1<<1, DIGI=1<<2, PHONE=1<<3, BEACON=1<<4, SAT=1<<5,
IMAGE=1<<6, FM=1<<7, DV=1<<8, ACDS=1<<9, EMERGENCY=1<<10`.

**TX-guard logic (future):** `f` is TX-legal iff a band row for the active region
(or a matching national override, which wins) has `f_lo ≤ f ≤ f_hi`, and — where
class-gated overrides exist — `f` falls in an override row whose mode matches the
emission and `lic_class` matches the operator. Segment table = display/soft-warning
only, never the hard interlock.

**Storage:** ship the band-plan as **static compiled-in C tables** (or a bundled
read-only JSON) — always present, no parser, matches the vendoring ethos. Store
only the *selection* (`region`, `country`) in the app's mutable `config.ini`.

---

## Sources
- IARU R1 HF band plan (PDF, eff. 2016): https://www.iaru-r1.org/wp-content/uploads/2019/08/hf_r1_bandplan.pdf
- IARU R1 band-plans hub: https://www.iaru-r1.org/on-the-air/band-plans/
- IARU R2 band plan (PDF, 2016): https://www.iaru.org/wp-content/uploads/2020/01/R2-Band-Plan-2016.pdf
- IARU R3 interim band plan (PDF, 2019): https://www.iaru.org/wp-content/uploads/2020/01/R3-004-IARU-Region-3-Bandplan-rev.2.pdf
- IARU R1 VHF/UHF band plan (IRTS mirror): https://www.irts.ie/dnloads/IARU-R1-Bandplan-VHF-UHF.pdf
- ARRL US frequency allocations: http://www.arrl.org/frequency-allocations
- ČTÚ 70 MHz notice: https://ctu.gov.cz/cesky-telekomunikacni-urad-oznamuje-ze-na-zaklade-zadosti-obcanskeho-sdruzeni-cesky-radioklub
- Český radioklub — Radioamatérská pásma: https://ceskyradioklub.cz/prakticke/radioamaterska-pasma
