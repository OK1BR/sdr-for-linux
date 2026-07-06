/*
 * protocol.h — helpers over the vendored piHPSDR wire contract.
 *
 *   - human-readable message-type names (for logging the startup burst)
 *   - the TCP payload-size table (how many bytes follow the 12-byte HEADER for a
 *     given message type) — the wire has NO length field, so this is essential
 *   - the password KDF (SHA-512 iterated 100000x), matching generate_pwd_hash()
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef PIHPSDR_CLIENT_PROTOCOL_H
#define PIHPSDR_CLIENT_PROTOCOL_H

#include <stdint.h>
#include <openssl/sha.h>     /* SHA512_DIGEST_LENGTH, used by the wire contract */

#include "client_server.h"   /* vendored, verbatim */

/* Name of a message type for logging, e.g. "INFO_RADIO". Never NULL. */
const char *cs_type_name(uint16_t data_type);

/*
 * Number of payload bytes that follow the HEADER for a TCP message of this
 * type (i.e. sizeof(struct) - sizeof(HEADER)), or 0 for header-only messages.
 *
 * This mirrors piHPSDR's own client_tcp_thread() dispatch exactly: types that
 * it reads as a packed struct return that struct's trailing size; every other
 * type is header-only (piHPSDR's default case reads no payload). Talking to the
 * same server, this is complete by construction.
 */
int cs_tcp_payload_size(uint16_t data_type);

/*
 * Compute the 64-byte challenge/response hash.
 *   response = SHA512^100000(challenge[64] || pwd), see generate_pwd_hash().
 * `pwd` may be "" (empty) but must not be NULL; it is truncated to 64 bytes.
 */
void cs_pwd_hash(const uint8_t challenge[SHA512_DIGEST_LENGTH],
                 const char *pwd,
                 uint8_t out[SHA512_DIGEST_LENGTH]);

#endif /* PIHPSDR_CLIENT_PROTOCOL_H */
