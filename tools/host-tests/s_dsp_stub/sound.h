/* s_dsp_stub/sound.h - what s_dsp.c reads from engine/client/sound.h, for tools/host-tests/s_dsp_host_test.c */
#ifndef S_DSP_STUB_SOUND_H
#define S_DSP_STUB_SOUND_H

#define SOUND_11k 11025
#define SOUND_22k 22050
#define SOUND_44k 44100
#define SOUND_48k 48000
/* the Xbox's mix rate is a variable (divergence #173): here the test's */
extern int s_dsp_test_dma_speed;
#define SOUND_DMA_SPEED s_dsp_test_dma_speed
#define CLIP16( x ) bound( SHRT_MIN + 8, x, SHRT_MAX - 8 )

typedef struct { int left, right; } portable_samplepair_t;

void SX_Init( void );
void SX_Free( void );
void SX_RoomFX( portable_samplepair_t *paint, int num_samples );
void SX_ClearState( void );
void SX_R3D_SetRate( void );

#endif
