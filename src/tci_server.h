/*
 * TCI server (F6d-2a) — ExpertSDR-compatible Transceiver Control Interface.
 *
 * Implements the control + CW slice of the official TCI protocol (Expert
 * Electronics "TCI Protocol.pdf" v2.0; see docs/TCI-SCOPE.md): a WebSocket
 * server (libwebsockets, default port 40001) speaking text commands
 * `name:arg1,arg2;`. Binary streams (audio/IQ) are later phases and are
 * ignored here.
 *
 * Threading: the LWS service loop runs on its own thread; every received
 * command is dispatched to the GTK main loop (g_idle_add) and executed
 * there through the TciOps table, so the GUI wires plain main-thread
 * functions. Outgoing messages are queued per client and flushed by the
 * LWS thread. A 500 ms main-loop reporter broadcasts state changes to all
 * clients (the TCI server is the synchronizer — clients never poll).
 *
 * ⛔ TX safety: set_trx/set_tune/cw_send must route through the SAME GUI
 * paths as the MOX/TUNE buttons — everything lands in tx_gate. TCI never
 * gets a shortcut around the gate.
 */
#ifndef SDRFL_TCI_SERVER_H
#define SDRFL_TCI_SERVER_H

/* All callbacks run on the GTK main thread. Getters must be cheap (the
 * 500 ms reporter polls them). Watts and TCI drive share the 0-100 scale. */
typedef struct {
  long long (*get_freq)(void);            /* tuned frequency, Hz            */
  void      (*set_freq)(long long hz);
  const char *(*get_mode)(void);          /* TCI mode string ("usb", "cwu") */
  int       (*set_mode)(const char *m);   /* 0 = ok, -1 = unsupported       */
  void      (*get_filter)(int *lo, int *hi); /* RX passband, Hz             */
  void      (*set_filter)(int lo, int hi);
  double    (*get_drive)(void);           /* MOX power request, 0-100       */
  void      (*set_drive)(double v);
  double    (*get_tune_drive)(void);      /* TUNE power request, 0-100      */
  void      (*set_tune_drive)(double v);
  int       (*get_trx)(void);             /* 1 = MOX requested              */
  int       (*set_trx)(int on);           /* 0 = accepted, -1 = refused     */
  int       (*get_tune)(void);
  int       (*set_tune)(int on);
  double    (*get_volume)(void);          /* AF volume, dB (-60..0)         */
  void      (*set_volume)(double db);
  int       (*get_mute)(void);
  void      (*set_mute)(int on);
  int       (*get_cw_speed)(void);        /* WPM                            */
  void      (*set_cw_speed)(int wpm);
  void      (*cw_send)(const char *text); /* queue Morse (break-in keys it) */
  void      (*cw_stop)(void);             /* abort the CW queue             */
  int       (*get_tx_enable)(void);       /* 1 = TX possible (PA on, ready) */
  int       (*get_rate)(void);            /* IQ rate, Hz → IF_LIMITS        */
  double    (*get_smeter)(void);          /* RX signal in the passband, dBm */
  void      (*get_tx_meters)(double *mic_db, double *rms_w, double *pep_w, double *swr);
  /* TX audio over TCI (F6d-2c). set_tx_source_tci(1) switches the exciter
   * input from the mic to the TCI ring (0 = back to mic); tx_audio_push MAY
   * be called from the TCI service thread (must be lock-free/thread-safe). */
  int       (*set_tx_source_tci)(int on); /* 0 = ok, -1 = not possible      */
  void      (*tx_audio_push)(const float *mono48k, int n);
} TciOps;

/* Start/stop the server. start returns 0 on success (port bound). *ops is
 * copied. Safe to call stop when not running. */
#define TCI_SERVER_MAX_CLIENTS 8

int  tci_server_start(int port, const TciOps *ops);
void tci_server_stop(void);
int  tci_server_running(void);
int  tci_server_clients(void);   /* connected client count (for the GUI)    */

/* Describe connected client slot i (0..TCI_SERVER_MAX_CLIENTS-1) as
 * "ip:port · user-agent [· audio]". Returns 1 if that slot is connected. */
int  tci_server_client_info(int i, char *buf, int len);

/* RX audio input (F6d-2b): push volume-independent mono 48 kHz samples from
 * the demod tap (demod_set_audio_tap → this). Lock-free SPSC ring, cheap
 * no-op while no client has audio_start'ed. Callable from any thread. */
void tci_server_audio_push(const float *mono48k, int n);

/* Raw DDC IQ input (F6d-2d): push interleaved complex pairs at the engine
 * rate from the P2 feed callback. The server decimates per client to the
 * requested iq_samplerate (48-384 k, WDSP resampler) and streams type-0
 * blocks. Lock-free SPSC ring, cheap no-op while no client has iq_start'ed.
 * Callable from the radio thread. */
void tci_server_iq_push(const double *iq, int n_pairs, int rate);

/* TX pacing clock (F6d-2c): emit a TX_CHRONO frame asking the TX-owner client
 * for nsamples of TX audio. Wire to tx_run_set_ext_notify; called from the TX
 * feed thread. */
void tci_server_tx_chrono(int nsamples);

#endif
