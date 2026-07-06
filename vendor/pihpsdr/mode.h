/*
 * Minimal shim for pihpsdr's mode.h.
 *
 * The vendored client_server.h includes mode.h but does not use any of its
 * symbols in the wire structs / enum / inline helpers that pihpsdr-client
 * relies on. An empty shim is therefore sufficient. See VENDOR.md.
 */
#ifndef PIHPSDR_CLIENT_SHIM_MODE_H
#define PIHPSDR_CLIENT_SHIM_MODE_H

#endif
