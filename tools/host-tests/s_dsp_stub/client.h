/* s_dsp_stub/client.h - the client state s_dsp.c reads (the listener's water level), for tools/host-tests/s_dsp_host_test.c */
#ifndef S_DSP_STUB_CLIENT_H
#define S_DSP_STUB_CLIENT_H

typedef struct { struct { int waterlevel; } local; } client_t;
extern client_t cl;

#endif
