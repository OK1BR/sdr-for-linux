/*
 * Minimal shim for pihpsdr's receiver.h.
 *
 * The vendored client_server.h (verbatim from piHPSDR) declares a handful of
 * server-side send_*() prototypes that take `const RECEIVER *`. pihpsdr-client
 * never calls those prototypes — it only needs the wire structs, the message
 * enum and the to_/from_ inline helpers. An opaque type is therefore enough to
 * let the vendored header compile standalone, without dragging the whole
 * piHPSDR receiver subsystem (and its transitive includes) into this project.
 *
 * See VENDOR.md.
 */
#ifndef PIHPSDR_CLIENT_SHIM_RECEIVER_H
#define PIHPSDR_CLIENT_SHIM_RECEIVER_H

typedef struct _receiver RECEIVER;

#endif
