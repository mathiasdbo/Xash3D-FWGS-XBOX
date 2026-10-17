/*
xbox_snd_al_queue.h - the mixer's ring, played through queued OpenAL buffers
Copyright (C) 2026 Resonance3D contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

/*
Resonance3D (docs/r3d/divergences.md #178). With r3d_snd_backend al the
mixer still paints snd.buffer, a ring of `ring` stereo frames that frame
time t occupies at t mod ring (S_TransferPaintBuffer, s_mix.c). Nothing
reads the ring by itself as SDL's audio thread does: the main thread copies
what has been painted into OpenAL buffers queued on one streaming source
(xbox_snd_al.c), and the mixer's DMA position, snd.samplepos, advances by
each buffer's length once the source reports it processed. So the clock
moves only in whole buffers, as SDL's moves in whole callbacks, and stands
still while the source has nothing to play.

The bookkeeping, kept free of OpenAL and engine types so
tools/host-tests/xbox_snd_al_queue_host_test.c can drive it:
  queued   frame time up to which the ring has been queued (paintedtime's
           domain: queued <= paintedtime)
  played   ring position, in frames, up to which the source has played
  lens[]   the frames of each buffer in flight, oldest first
The ring's frames in flight are always (played, queued mod ring], so
played + sum(lens) == queued (mod ring).
*/
#ifndef XBOX_SND_AL_QUEUE_H
#define XBOX_SND_AL_QUEUE_H

#define XBOX_SND_AL_MAX_BUFFERS 32

typedef struct xbox_snd_al_queue_s
{
	int ring;   /* frames in the mixer's ring (snd.samples / 2) */
	int queued; /* frame time queued up to */
	int played; /* ring position played up to, 0 .. ring-1 */
	int head;   /* lens[head] is the oldest buffer in flight */
	int count;  /* buffers in flight */
	int lens[XBOX_SND_AL_MAX_BUFFERS];
} xbox_snd_al_queue_t;

static inline void Xbox_SndALQ_Init( xbox_snd_al_queue_t *q, int ring, int time )
{
	q->ring = ring;
	q->queued = time;
	q->played = time % ring;
	q->head = 0;
	q->count = 0;
}

/* The oldest buffer in flight has been played: the clock moves past it.
   Returns its frames, 0 when none is in flight. */
static inline int Xbox_SndALQ_Retire( xbox_snd_al_queue_t *q )
{
	int len;

	if( q->count <= 0 )
		return 0;
	len = q->lens[q->head];
	q->head = ( q->head + 1 ) % XBOX_SND_AL_MAX_BUFFERS;
	q->count--;
	q->played = ( q->played + len ) % q->ring;
	return len;
}

/* The next buffer to queue: the painted, unqueued frames from `queued` on,
   up to `paintedtime`, no more than max_frames and not across the ring's
   end. Returns its frames and its ring position in *pos, or 0 when there
   is less than min_frames to queue or no buffer is free.

   The painted time only moves back, or more than a ring ahead, when the
   mixer restarts its clock (S_GetSoundtime's chop after 2^30 frames, a
   rebase); the queue then follows it, and the play position moves by the
   same amount, so the frames in flight keep their meaning. */
static inline int Xbox_SndALQ_Next( xbox_snd_al_queue_t *q, int paintedtime, int min_frames, int max_frames, int *pos )
{
	int n, p;

	n = paintedtime - q->queued;
	if( n < 0 || n > q->ring )
	{
		const int moved = ( paintedtime - q->queued ) % q->ring;

		q->played = (( q->played + moved ) % q->ring + q->ring ) % q->ring;
		q->queued = paintedtime;
		return 0;
	}
	if( q->count >= XBOX_SND_AL_MAX_BUFFERS )
		return 0;

	p = q->queued % q->ring;
	if( n > q->ring - p )
		n = q->ring - p; /* the rest is queued from position 0 next */
	if( n > max_frames )
		n = max_frames;
	if( n < min_frames && n < q->ring - p )
		return 0; /* too little yet; a piece up to the ring's end goes as it is */
	if( n <= 0 )
		return 0;
	*pos = p;
	return n;
}

/* The buffer Next returned has been queued */
static inline void Xbox_SndALQ_Commit( xbox_snd_al_queue_t *q, int frames )
{
	q->lens[( q->head + q->count ) % XBOX_SND_AL_MAX_BUFFERS] = frames;
	q->count++;
	q->queued += frames;
}

/* Queues the painted, unqueued frames as Next splits them: submit(user,
   pos, frames) uploads and queues one buffer and returns nonzero only when
   both succeeded; only then is it committed. A failure commits nothing
   and stops the fill: those frames are tried again at the next update.
   Returns the buffers queued. */
static inline int Xbox_SndALQ_Fill( xbox_snd_al_queue_t *q, int paintedtime, int min_frames, int max_frames,
	int ( *submit )( void *user, int pos, int frames ), void *user )
{
	int n, pos, queued = 0;

	while(( n = Xbox_SndALQ_Next( q, paintedtime, min_frames, max_frames, &pos )) > 0 )
	{
		if( !submit( user, pos, n ))
			break;
		Xbox_SndALQ_Commit( q, n );
		queued++;
	}
	return queued;
}

/* Every buffer in flight is dropped (S_ClearBuffer: the source has been
   stopped and its queue emptied). Those frames will never play, so the
   play position moves to the queued one at once; left behind, the clock
   would trail the painted time forever and the mixer would paint less and
   less ahead. Returns the frames dropped. */
static inline int Xbox_SndALQ_Flush( xbox_snd_al_queue_t *q )
{
	int i, dropped = 0;

	for( i = 0; i < q->count; i++ )
		dropped += q->lens[( q->head + i ) % XBOX_SND_AL_MAX_BUFFERS];
	q->head = 0;
	q->count = 0;
	q->played = q->queued % q->ring;
	return dropped;
}

/* Frames queued but not yet played */
static inline int Xbox_SndALQ_InFlight( const xbox_snd_al_queue_t *q )
{
	int i, sum = 0;

	for( i = 0; i < q->count; i++ )
		sum += q->lens[( q->head + i ) % XBOX_SND_AL_MAX_BUFFERS];
	return sum;
}

#endif /* XBOX_SND_AL_QUEUE_H */
