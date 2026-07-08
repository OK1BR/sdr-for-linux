/*
 * bandplan.c — see bandplan.h. Static, compiled-in band-plan data.
 *
 * Frequencies are transmitted frequencies in Hz. Region edges + national edge
 * overrides drive the display (and, later, the hard TX out-of-band guard). The
 * segment tables are R1 HF complete (display / soft info only). Data provenance
 * and the volatile-value caveats are in docs/BANDPLAN.md.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bandplan.h"

#include <stdio.h>
#include <string.h>

/* ---- Regions & countries ------------------------------------------------- */

static const struct { const char *key, *name; } g_regions[] = {
  { "R1", "Region 1 (EU / AF / ME)" },
  { "R2", "Region 2 (Americas)" },
  { "R3", "Region 3 (Asia-Pacific)" },
};
static const struct { const char *key, *name; } g_countries[] = {
  { "",   "— none (region plan) —" },
  { "CZ", "Czech Republic (OK)" },
  { "US", "United States" },
};

int         bp_region_count(void) { return (int)(sizeof g_regions / sizeof g_regions[0]); }
const char *bp_region_name(int r)  { return (r >= 0 && r < bp_region_count()) ? g_regions[r].name : ""; }
const char *bp_region_key(int r)   { return (r >= 0 && r < bp_region_count()) ? g_regions[r].key  : ""; }
int         bp_region_from_key(const char *k) {
  if (k) { for (int i = 0; i < bp_region_count(); i++) { if (!strcmp(k, g_regions[i].key)) { return i; } } }
  return -1;
}
int         bp_country_count(void) { return (int)(sizeof g_countries / sizeof g_countries[0]); }
const char *bp_country_name(int c) { return (c >= 0 && c < bp_country_count()) ? g_countries[c].name : ""; }
const char *bp_country_key(int c)  { return (c >= 0 && c < bp_country_count()) ? g_countries[c].key  : ""; }
int         bp_country_from_key(const char *k) {
  if (!k) { return 0; }
  for (int i = 0; i < bp_country_count(); i++) { if (!strcmp(k, g_countries[i].key)) { return i; } }
  return -1;
}

/* ---- Band edges per region (0/0 = not allocated in that region) ---------- */

typedef struct {
  const char *band;
  int64_t r[3][2];   /* [region][lo,hi] */
} bp_band_row_t;

static const bp_band_row_t g_bands[] = {
  /* band     R1 lo/hi                 R2 lo/hi                 R3 lo/hi              */
  { "2200m", {{135700,137800},        {135700,137800},        {135700,137800}} },
  { "630m",  {{472000,479000},        {472000,479000},        {472000,479000}} },
  { "160m",  {{1810000,2000000},      {1800000,2000000},      {1800000,2000000}} },
  { "80m",   {{3500000,3800000},      {3500000,4000000},      {3500000,3900000}} },
  { "60m",   {{5351500,5366500},      {5351500,5366500},      {5351500,5366500}} },
  { "40m",   {{7000000,7200000},      {7000000,7300000},      {7000000,7200000}} },
  { "30m",   {{10100000,10150000},    {10100000,10150000},    {10100000,10150000}} },
  { "20m",   {{14000000,14350000},    {14000000,14350000},    {14000000,14350000}} },
  { "17m",   {{18068000,18168000},    {18068000,18168000},    {18068000,18168000}} },
  { "15m",   {{21000000,21450000},    {21000000,21450000},    {21000000,21450000}} },
  { "12m",   {{24890000,24990000},    {24890000,24990000},    {24890000,24990000}} },
  { "10m",   {{28000000,29700000},    {28000000,29700000},    {28000000,29700000}} },
  { "6m",    {{50000000,52000000},    {50000000,54000000},    {50000000,54000000}} },
  { "4m",    {{70000000,70500000},    {0,0},                  {0,0}} },   /* R1 only */
  { "2m",    {{144000000,146000000},  {144000000,148000000},  {144000000,148000000}} },
  { "70cm",  {{430000000,440000000},  {430000000,450000000},  {430000000,440000000}} },
};
#define NBAND_ROWS ((int)(sizeof g_bands / sizeof g_bands[0]))

/* National edge overrides: replace the region edge for (country,band). Only the
 * deltas that differ from the region plan (see docs/BANDPLAN.md §4). */
typedef struct { const char *country, *band; int64_t lo, hi; } bp_override_t;
static const bp_override_t g_overrides[] = {
  { "CZ", "160m", 1800000,   2000000   },   /* OK runs 1800, not the 1810 R1-plan table */
  { "CZ", "4m",   70100000,  70300000  },   /* OK 4 m secondary, 10 W ERP, permit-gated  */
  { "US", "70cm", 420000000, 450000000 },   /* US 70 cm is 420–450, wider than R2 430–450 */
  /* (US 60 m is 5 discrete channels, US 40 m already = the R2 7000–7300 edge — no edge delta.) */
};

/* Apply any national override for (country,band) to lo/hi; return 1 if applied. */
static int apply_override(const char *country, const char *band, int64_t *lo, int64_t *hi) {
  if (!country || !*country) { return 0; }
  for (unsigned i = 0; i < sizeof g_overrides / sizeof g_overrides[0]; i++) {
    if (!strcmp(country, g_overrides[i].country) && !strcmp(band, g_overrides[i].band)) {
      *lo = g_overrides[i].lo; *hi = g_overrides[i].hi; return 1;
    }
  }
  return 0;
}

int bp_edges(bp_region_t region, const char *country_key, bp_edge_t *out, int max) {
  if (region < 0 || region > BP_R3) { return 0; }
  int n = 0;
  for (int i = 0; i < NBAND_ROWS && n < max; i++) {
    int64_t lo = g_bands[i].r[region][0], hi = g_bands[i].r[region][1];
    apply_override(country_key, g_bands[i].band, &lo, &hi);
    if (hi <= lo) { continue; }   /* not allocated in this region */
    out[n].band = g_bands[i].band; out[n].lo = lo; out[n].hi = hi; n++;
  }
  return n;
}

const char *bp_band_for_freq(bp_region_t region, const char *country_key,
                             int64_t f, int64_t *lo, int64_t *hi) {
  if (region < 0 || region > BP_R3) { return NULL; }
  for (int i = 0; i < NBAND_ROWS; i++) {
    int64_t l = g_bands[i].r[region][0], h = g_bands[i].r[region][1];
    apply_override(country_key, g_bands[i].band, &l, &h);
    if (h > l && f >= l && f <= h) {
      if (lo) { *lo = l; } if (hi) { *hi = h; }
      return g_bands[i].band;
    }
  }
  return NULL;
}

/* ---- IARU Region 1 segments (HF complete; docs/BANDPLAN.md §3) ------------ */

#define ALL   BP_ALL
static const bp_seg_t s_2200m[] = { {135700,137800,200,BP_CW,""} };
static const bp_seg_t s_630m[]  = { {472000,479000,500,BP_CW|BP_DIGI,""} };
static const bp_seg_t s_160m[]  = {
  {1810000,1838000,200,BP_CW,"1836 CW-QRP"}, {1838000,1840000,500,BP_NB,""},
  {1840000,1843000,2700,ALL,"digimodes"},    {1843000,2000000,2700,ALL,""},
};
static const bp_seg_t s_60m[] = {
  {5351500,5354000,200,BP_CW|BP_NB,""}, {5354000,5366000,2700,ALL,"USB voice"},
  {5366000,5366500,20,BP_NB,"weak-signal"},
};
static const bp_seg_t s_80m[] = {
  {3500000,3510000,200,BP_CW,"DX/contest"}, {3510000,3560000,200,BP_CW,"3555 QRS"},
  {3560000,3570000,200,BP_CW,"3560 QRP"},   {3570000,3580000,200,BP_NB|BP_DIGI,""},
  {3580000,3590000,500,BP_NB|BP_DIGI,""},   {3590000,3600000,500,BP_NB|BP_DIGI|BP_ACDS,""},
  {3600000,3650000,2700,ALL,"3630 DV"},     {3650000,3700000,2700,ALL,"3690 SSB-QRP"},
  {3700000,3775000,2700,ALL,"3760 Emerg"},  {3775000,3800000,2700,ALL,"DX priority"},
};
static const bp_seg_t s_40m[] = {
  {7000000,7040000,200,BP_CW,"7030 QRP"},   {7040000,7047000,500,BP_NB|BP_DIGI,""},
  {7047000,7050000,500,BP_NB|BP_DIGI|BP_ACDS,""}, {7050000,7053000,2700,ALL,"digi/ACDS"},
  {7053000,7060000,2700,ALL,""},            {7060000,7100000,2700,ALL,"7090 SSB-QRP"},
  {7100000,7130000,2700,ALL,"7110 Emergency"}, {7130000,7175000,2700,ALL,"SSB contest"},
  {7175000,7200000,2700,ALL,"7165 Image"},
};
static const bp_seg_t s_30m[] = {
  {10100000,10130000,200,BP_CW,"10116 QRP"}, {10130000,10150000,500,BP_NB|BP_DIGI,""},
};
static const bp_seg_t s_20m[] = {
  {14000000,14060000,200,BP_CW,"14055 QRS"}, {14060000,14070000,200,BP_CW,"14060 QRP"},
  {14070000,14089000,500,BP_NB|BP_DIGI,""},  {14089000,14099000,500,BP_NB|BP_DIGI|BP_ACDS,""},
  {14099000,14101000,0,BP_BEACON,"IBP excl"},{14101000,14112000,2700,ALL|BP_ACDS,""},
  {14112000,14125000,2700,ALL,""},           {14125000,14300000,2700,ALL,"14230 Image;14285 QRP"},
  {14300000,14350000,2700,ALL,"14300 Emergency"},
};
static const bp_seg_t s_17m[] = {
  {18068000,18095000,200,BP_CW,"18086 QRP"}, {18095000,18105000,500,BP_NB|BP_DIGI,""},
  {18105000,18109000,500,BP_NB|BP_DIGI|BP_ACDS,""}, {18109000,18111000,0,BP_BEACON,"IBP excl"},
  {18111000,18120000,2700,ALL|BP_ACDS,""},   {18120000,18168000,2700,ALL,"18150 DV;18160 Emerg"},
};
static const bp_seg_t s_15m[] = {
  {21000000,21070000,200,BP_CW,"21055 QRS"}, {21070000,21090000,500,BP_NB|BP_DIGI,""},
  {21090000,21110000,500,BP_NB|BP_DIGI|BP_ACDS,""}, {21110000,21120000,2700,ALL|BP_ACDS,"not SSB"},
  {21120000,21149000,500,BP_NB,""},          {21149000,21151000,0,BP_BEACON,"IBP excl"},
  {21151000,21450000,2700,ALL,"21285 QRP;21340 Image"},
};
static const bp_seg_t s_12m[] = {
  {24890000,24915000,200,BP_CW,"24906 QRP"}, {24915000,24925000,500,BP_NB|BP_DIGI,""},
  {24925000,24929000,500,BP_NB|BP_DIGI|BP_ACDS,""}, {24929000,24931000,0,BP_BEACON,"IBP excl"},
  {24931000,24940000,2700,ALL|BP_ACDS,""},   {24940000,24990000,2700,ALL,"24950 QRP;24960 DV"},
};
static const bp_seg_t s_10m[] = {
  {28000000,28070000,200,BP_CW,"28055 QRS"}, {28070000,28120000,500,BP_NB|BP_DIGI,""},
  {28120000,28150000,500,BP_NB|BP_DIGI|BP_ACDS,""}, {28150000,28190000,500,BP_NB,""},
  {28190000,28225000,0,BP_BEACON,"IBP excl"},{28225000,28300000,2700,ALL|BP_BEACON,""},
  {28300000,28320000,2700,ALL|BP_ACDS,""},   {28320000,29000000,2700,ALL,"28360 QRP;28680 Image"},
  {29000000,29100000,6000,ALL,""},           {29100000,29200000,6000,ALL|BP_FM,"FM simplex"},
  {29200000,29300000,6000,ALL|BP_ACDS,""},   {29300000,29510000,6000,BP_SAT,"satellite"},
  {29510000,29520000,0,0,"guard"},           {29520000,29590000,6000,BP_FM,"rptr in"},
  {29590000,29620000,6000,BP_FM,"29600 FM call"}, {29620000,29700000,6000,BP_FM,"rptr out"},
};
#undef ALL

static const struct { const char *band; const bp_seg_t *seg; int n; } g_r1_segtab[] = {
#define SEG(b, arr) { b, arr, (int)(sizeof arr / sizeof arr[0]) }
  SEG("2200m", s_2200m), SEG("630m", s_630m), SEG("160m", s_160m), SEG("60m", s_60m),
  SEG("80m", s_80m), SEG("40m", s_40m), SEG("30m", s_30m), SEG("20m", s_20m),
  SEG("17m", s_17m), SEG("15m", s_15m), SEG("12m", s_12m), SEG("10m", s_10m),
#undef SEG
};

static const bp_seg_t *find_segtab(const char *band, int *n) {
  for (unsigned i = 0; i < sizeof g_r1_segtab / sizeof g_r1_segtab[0]; i++) {
    if (!strcmp(band, g_r1_segtab[i].band)) { *n = g_r1_segtab[i].n; return g_r1_segtab[i].seg; }
  }
  *n = 0;
  return NULL;
}

int bp_segments(bp_region_t region, const char *band, bp_seg_t *out, int max) {
  (void)region;   /* R2/R3 fall back to the R1 table for now (docs/BANDPLAN.md) */
  int n = 0;
  const bp_seg_t *t = find_segtab(band, &n);
  if (!t) { return 0; }
  if (n > max) { n = max; }
  memcpy(out, t, (size_t)n * sizeof *out);
  return n;
}

/* Short mode word from a usage bitmask, for the readout. */
static const char *usage_word(uint16_t u) {
  if (u == 0)                   { return "guard"; }
  if (u == BP_BEACON)           { return "Beacons"; }
  if (u == BP_SAT)              { return "Satellite"; }
  if (u == BP_FM)               { return "FM"; }
  if (u & BP_PHONE)             { return "All modes"; }
  if (u & (BP_NB | BP_DIGI))    { return "Narrow/digi"; }
  if (u == BP_CW || (u & BP_CW)){ return "CW"; }
  return "All modes";
}

const char *bp_segment_at(bp_region_t region, const char *country_key, int64_t f) {
  static char buf[64];
  int64_t lo, hi;
  const char *band = bp_band_for_freq(region, country_key, f, &lo, &hi);
  if (!band) { return NULL; }
  int n = 0;
  const bp_seg_t *t = find_segtab(band, &n);
  for (int i = 0; i < n; i++) {
    if (f >= t[i].lo && f < t[i].hi) {
      if (t[i].label && t[i].label[0]) {
        snprintf(buf, sizeof buf, "%s · %s", usage_word(t[i].usage), t[i].label);
      } else {
        snprintf(buf, sizeof buf, "%s", usage_word(t[i].usage));
      }
      return buf;
    }
  }
  return NULL;
}
