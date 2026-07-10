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
} TciOps;

/* Start/stop the server. start returns 0 on success (port bound). *ops is
 * copied. Safe to call stop when not running. */
int  tci_server_start(int port, const TciOps *ops);
void tci_server_stop(void);
int  tci_server_running(void);
int  tci_server_clients(void);   /* connected client count (for the GUI)    */

#endif
