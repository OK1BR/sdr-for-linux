/*
 * sdr-for-linux — minimal t_print/t_perror shim for vendored/adapted piHPSDR
 * engine code. piHPSDR ships a full message.c (timestamped, mutex-guarded); for
 * the engine's diagnostics we route to stderr so stdout stays clean for a
 * tool's structured output. Replace with a richer logger later if needed.
 */
#ifndef SDRFL_ENGINE_MESSAGE_H
#define SDRFL_ENGINE_MESSAGE_H

#include <stdio.h>

#define t_print(...) fprintf(stderr, __VA_ARGS__)
#define t_perror(s)  perror(s)

#endif /* SDRFL_ENGINE_MESSAGE_H */
