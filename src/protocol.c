/*
 * protocol.c — see protocol.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "protocol.h"

#include <string.h>
#include <openssl/sha.h>

const char *cs_type_name(uint16_t data_type) {
  switch (data_type) {
    /* Server-pushed state seen in the startup burst / streams. */
    case INFO_RADIO:        return "INFO_RADIO";
    case INFO_ADC:          return "INFO_ADC";
    case INFO_RECEIVER:     return "INFO_RECEIVER";
    case INFO_TRANSMITTER:  return "INFO_TRANSMITTER";
    case INFO_VFO:          return "INFO_VFO";
    case INFO_BAND:         return "INFO_BAND";
    case INFO_BANDSTACK:    return "INFO_BANDSTACK";
    case INFO_MEMORY:       return "INFO_MEMORY";
    case INFO_DISPLAY:      return "INFO_DISPLAY";
    case INFO_PS:           return "INFO_PS";
    case INFO_RX_SPECTRUM:  return "INFO_RX_SPECTRUM";
    case INFO_TX_SPECTRUM:  return "INFO_TX_SPECTRUM";
    case INFO_RXAUDIO:      return "INFO_RXAUDIO";
    case INFO_RXAUDIO_OPUS: return "INFO_RXAUDIO_OPUS";
    /* Commands the server echoes / sends during setup. */
    case CMD_START_RADIO:   return "CMD_START_RADIO";
    case CMD_SAMPLE_RATE:   return "CMD_SAMPLE_RATE";
    case CMD_RECEIVERS:     return "CMD_RECEIVERS";
    case CMD_LOCK:          return "CMD_LOCK";
    case CMD_SAT:           return "CMD_SAT";
    case CMD_FILTER_VAR:    return "CMD_FILTER_VAR";
    case CMD_RX_FILTER_CUT: return "CMD_RX_FILTER_CUT";
    case CMD_TX_FILTER_CUT: return "CMD_TX_FILTER_CUT";
    case CMD_PAN:           return "CMD_PAN";
    case CMD_AGC:           return "CMD_AGC";
    case CMD_MOX:           return "CMD_MOX";
    case CMD_VOX:           return "CMD_VOX";
    case CMD_TUNE:          return "CMD_TUNE";
    case CMD_TWOTONE:       return "CMD_TWOTONE";
    case CMD_PONG:          return "CMD_PONG";
    default:                return "CMD/INFO?";
  }
}

int cs_tcp_payload_size(uint16_t data_type) {
  switch (data_type) {
    /* Packed structs the server sends over TCP (mirrors client_tcp_thread). */
    case INFO_MEMORY:      return (int)(sizeof(MEMORY_DATA)      - sizeof(HEADER));
    case INFO_BAND:        return (int)(sizeof(BAND_DATA)        - sizeof(HEADER));
    case INFO_BANDSTACK:   return (int)(sizeof(BANDSTACK_DATA)   - sizeof(HEADER));
    case INFO_RADIO:       return (int)(sizeof(RADIO_DATA)       - sizeof(HEADER));
    case INFO_ADC:         return (int)(sizeof(ADC_DATA)         - sizeof(HEADER));
    case INFO_RECEIVER:    return (int)(sizeof(RECEIVER_DATA)    - sizeof(HEADER));
    case INFO_TRANSMITTER: return (int)(sizeof(TRANSMITTER_DATA) - sizeof(HEADER));
    case INFO_VFO:         return (int)(sizeof(VFO_DATA)         - sizeof(HEADER));
    case CMD_SAMPLE_RATE:  return (int)(sizeof(U32_COMMAND)      - sizeof(HEADER));
    case CMD_AGC:          return (int)(sizeof(AGC_COMMAND)      - sizeof(HEADER));
    /* Everything else on TCP is header-only (piHPSDR's default reads nothing). */
    default:               return 0;
  }
}

void cs_pwd_hash(const uint8_t challenge[SHA512_DIGEST_LENGTH],
                 const char *pwd,
                 uint8_t out[SHA512_DIGEST_LENGTH]) {
  size_t pwdlen = strlen(pwd);
  if (pwdlen > SHA512_DIGEST_LENGTH) {
    pwdlen = SHA512_DIGEST_LENGTH;
  }

  /* Scratch buffer: [ 64-byte block ][ pwd ]. */
  uint8_t s[SHA512_DIGEST_LENGTH + SHA512_DIGEST_LENGTH];
  memcpy(s, challenge, SHA512_DIGEST_LENGTH);
  memcpy(s + SHA512_DIGEST_LENGTH, pwd, pwdlen);

  uint8_t hash[SHA512_DIGEST_LENGTH];
  SHA512(s, SHA512_DIGEST_LENGTH + pwdlen, hash);

  for (int i = 0; i < 99999; i++) {
    memcpy(s, hash, SHA512_DIGEST_LENGTH);
    memcpy(s + SHA512_DIGEST_LENGTH, pwd, pwdlen);
    SHA512(s, SHA512_DIGEST_LENGTH + pwdlen, hash);
  }

  memcpy(out, hash, SHA512_DIGEST_LENGTH);
}
