; Half-Life's room effect (engine/client/sound/s_dsp.c) as a GP kernel
; (docs/r3d/divergences.md #180). Runs inside nxdk-ex's GP frame loop
; (lib/openal/dsp/gp_loop.asm; the ABI in lib/openal/docs/GP_KERNELS.md) on the
; room mix, which the mixer queues to a stereo source sending at 1.0 into this
; kernel's slot; the output goes on the dry mix, scaled by the master volume.
;
; s_dsp.c's SX_RoomFX, one 32-sample block a frame: the amplitude modulation
; (RVB_DoAMod), the reverb's two lines (RVB_DoReverb), the mono delay
; (DLY_DoDelay), the left channel's delay (DLY_DoStereoDelay), then the
; mixer's S_MixBufferWithGain and the output's CLIP16 (s_mix.c). The same
; integer arithmetic: samples are ints in A1, each >> and / of the C code
; rounds as it does (a multiply by a constant with the result's integer part
; taken is a floor; truncating divisions work on the magnitude), and CLIP16 is
; s_dsp.c's, -32760..32759.
;
; What the CPU keeps (SX_R3D_GpParams in s_dsp.c): the presets and cvars,
; turned into the parameters below. What the kernel keeps: the rest of the
; *_CheckNew*Val functions (a line set up fresh, the crossfades to a new
; length) and the delay lines. A delay line here is the distance between its
; write and read positions, as s_dsp.c's pointers keep it; a line that was
; just set up reads zeros where nothing has been written since (its age), as
; the calloc'd or cleared array does. Each line lives in scratch: the frame's
; delayed blocks are read before the kernel runs and its new block written
; after, so a read 31 samples back (the reverb's second line after a
; crossfade to 32) takes the sample this frame wrote.
;
; Timing: parameters that arrive in frame n are applied at the end of it, for
; frame n+1 (the delayed blocks of frame n+1 are read with the new lengths).
;
; Parameters (AL_XBOX_GP_PARAM0 + n, all ints; SXGP_* in engine/client/sound/s_dsp_gp.h):
;   0 RUN  1 GAIN  2 ALPHA  3 AMOD_LP  4 AMOD_MOD
;   5 RVB_GEN  6 RVB_S1  7 RVB_S2  8 RVB_LP  9 RVB_FB
;  10 DLY_GEN 11 DLY_S  12 DLY_LP  13 DLY_FB  14 STE_GEN  15 STE_S
;
; Assemble: python3 3rdparty/nxdk-ex/lib/openal/tools/dsp56asm.py
;   engine/platform/xbox/apu/gp_hldsp.asm -o engine/platform/xbox/apu/gp_hldsp.h --image

K_SCRATCH_BYTES equ $20F00      ; image header: the four lines below
K_DESCRIPTORS   equ 11          ; image header: 7 reads, 4 writes

KIN     equ     $E0             ; ABI (gp_loop.asm)
KSCR    equ     $E1
KPARAM  equ     $E8
K_RHEAD equ     $19
K_WHEAD equ     $1A
FIFO2   equ     $08
OUTL    equ     $20

EOL     equ     $004000
CTL_R   equ     $0049D0         ; memory -> DSP, circular scratch, 24-bit, step 1
CTL_W   equ     $0049D2         ; DSP -> memory, the same

; scratch: byte offsets and sizes, 4 bytes a sample; each size holds the longest
; distance (4800 and 19200 samples: 0.1 s and 0.4 s at 48 kHz, room_hires 2)
B_RV1   equ     $0000
B_RV2   equ     $4B80
B_ST    equ     $9700
B_MO    equ     $E280
SZ_RV   equ     $4B80           ; 4832 samples
SZ_ST   equ     $4B80
SZ_MO   equ     $12C80          ; 19232 samples

; a line's state (16 words)
F_ACT   equ     0               ; set up (s_dsp.c: lpdelayline != NULL)
F_DSAMP equ     1               ; delaysamples
F_DOUT  equ     2               ; input - output, in samples
F_DXF   equ     3               ; input - crossfade output
F_XFADE equ     4               ; crossfade samples left
F_AGE   equ     5               ; samples written since set up (saturating)
F_W     equ     6               ; this frame's write position, bytes
F_LP0   equ     7               ; lowpass state
F_LP1   equ     8
F_SIZE  equ     9               ; line size, bytes
F_RDD   equ     10              ; its read descriptor
F_XFD   equ     11              ; its crossfade read descriptor, 0 for none
F_WRD   equ     12              ; its write descriptor
F_RDB   equ     13              ; the delayed block
F_XFB   equ     14              ; the crossfade's delayed block
F_WBB   equ     15              ; the block written
F_N     equ     16

INIT    equ     $108
RV1     equ     $110            ; reverb lines (reverbdly[0], [1])
RV2     equ     $120
MON     equ     $130            ; mono delay (monodly)
STE     equ     $140            ; stereo delay (stereodly)
CUR     equ     $150            ; the line being worked on
S       equ     $160            ; the parameters in use (16)
TD      equ     $170            ; temporaries
TX      equ     $171
TS      equ     $172
TA      equ     $173
TSUM    equ     $174
FBK     equ     $175            ; the stage's feedback << 15
LPF     equ     $176            ; the reverb's lowpass flag
GRV     equ     $177            ; the generations applied
GDL     equ     $178
GST     equ     $179
CURP    equ     $17A            ; the line CUR was loaded from
LPS     equ     $180            ; rgsxlp[10]

D_R1O   equ     $190            ; descriptors, read chain
D_R1X   equ     $198
D_R2O   equ     $1A0
D_R2X   equ     $1A8
D_MO    equ     $1B0
D_SO    equ     $1B8
D_SX    equ     $1C0
D_W1    equ     $1C8            ; write chain
D_W2    equ     $1D0
D_WM    equ     $1D8
D_WS    equ     $1E0

RD1     equ     $200            ; 32-word blocks
XF1     equ     $220
RD2     equ     $240
XF2     equ     $260
RDM     equ     $280
RDS     equ     $2A0
XFS     equ     $2C0
WB1     equ     $2E0
WB2     equ     $300
WBM     equ     $320
WBS     equ     $340
BL      equ     $360            ; the room mix, ints; BR follows
BR      equ     $380
VLR     equ     $3A0            ; (L + R) >> 1
NZ      equ     $3C0            ; |L| + |R|: nonzero when either is
V1      equ     $3E0            ; each reverb line's output
V2      equ     $400

S_RUN   equ     S+0
S_GAIN  equ     S+1
S_ALPHA equ     S+2
S_AMLP  equ     S+3
S_AMMOD equ     S+4
S_RVGEN equ     S+5
S_RVS1  equ     S+6
S_RVS2  equ     S+7
S_RVLP  equ     S+8
S_RVFB  equ     S+9
S_DLGEN equ     S+10
S_DLS   equ     S+11
S_DLLP  equ     S+12
S_DLFB  equ     S+13
S_STGEN equ     S+14
S_STS   equ     S+15

CLIPHI  equ     32759           ; CLIP16: SHRT_MAX - 8
CLIPLO  equ     -32760          ; SHRT_MIN + 8
C3      equ     2796203         ; ceil(2^23 / 3): |n| / 3 exactly for |n| < 2^17
C6      equ     1398102         ; ceil(2^23 / 6)
K11     equ     1441792         ; 11 << 17: (11 v) >> 6
K255    equ     8355840         ; 255 << 15: (255 v) >> 8

; Loop bodies end in at least two plain instructions: no jump in the last
; three words of a DO loop. CLIP16's compares spell out CLIPHI and CLIPLO
; (the host simulator reads an ALU immediate in lower case).

        org     p:$100
kernel  move    x:INIT,a
        tst     a
        jne     ready
        jsr     setup
ready   jsr     input
        move    x:S_RUN,a
        tst     a
        jeq     mix
        jsr     amod
        jsr     reverb
        jsr     mono
        jsr     stereo
mix     jsr     output
        jsr     endframe
        rts

; ------------------------------------------------------------ the room mix
; BL/BR = input >> 8 (a 16-bit source arrives as s << 8)
input   move    x:KIN,r1
        move    #BL,r4
        move    #$8000,y1
        do      #64,in_end
        move    x:(r1)+,x0
        mpy     x0,y1,a
        move    a1,x:(r4)+
        nop
in_end  rts

; ------------------------------------------------------------ RVB_DoAMod
amod    move    x:S_AMLP,a
        move    x:S_AMMOD,b
        tst     a
        jne     am_go
        tst     b
        jne     am_go
        rts
am_go   tst     a
        jeq     am_cl
; the lowpass: res = (5 of rgsxlp + paint) >> 2, rgsxlp shifted as s_dsp.c does
        move    #BL,r4
        move    #BR,r5
        move    #BL,r6
        move    #BR,r7
        do      #32,lp_end
        move    x:(r4)+,x0
        move    x:(r5)+,x1
        move    x:LPS+0,a
        move    x:LPS+1,y0
        add     y0,a
        move    x:LPS+2,y0
        add     y0,a
        move    x:LPS+3,y0
        add     y0,a
        move    x:LPS+4,y0
        add     y0,a
        add     x0,a
        asr     a
        asr     a
        move    x:LPS+5,b
        move    x:LPS+6,y0
        add     y0,b
        move    x:LPS+7,y0
        add     y0,b
        move    x:LPS+8,y0
        add     y0,b
        move    x:LPS+9,y0
        add     y0,b
        add     x1,b
        asr     b
        asr     b
        move    a1,x:(r6)+
        move    b1,x:(r7)+
        move    x0,x:LPS+4
        move    x1,x:LPS+9
        move    x:LPS+1,y0
        move    y0,x:LPS+0
        move    x:LPS+2,y0
        move    y0,x:LPS+1
        move    x:LPS+3,y0
        move    y0,x:LPS+2
        move    x:LPS+4,y0
        move    y0,x:LPS+3
        move    x:LPS+5,y0
        move    y0,x:LPS+4
        move    x:LPS+6,y0
        move    y0,x:LPS+5
        move    x:LPS+7,y0
        move    y0,x:LPS+6
        move    x:LPS+8,y0
        move    y0,x:LPS+7
        move    x:LPS+9,y0
        move    y0,x:LPS+8
        nop
lp_end
; the modulation (sxamodl/r stay 255: their targets only move in dead code), then CLIP16
am_cl   move    #BL,r4
        move    #BL,r6
        move    #K255,y0
        move    x:S_AMMOD,b
        tst     b
        jeq     am_c0
        do      #64,amm_end
        move    x:(r4)+,x0
        mpy     x0,y0,a
        move    a1,a
        cmp     #32759,a
        jle     amm1
        move    #CLIPHI,a
amm1    cmp     #-32760,a
        jge     amm2
        move    #CLIPLO,a
amm2    move    a1,x:(r6)+
        nop
amm_end rts
am_c0   do      #64,am0_end
        move    x:(r4)+,a
        cmp     #32759,a
        jle     am01
        move    #CLIPHI,a
am01    cmp     #-32760,a
        jge     am02
        move    #CLIPLO,a
am02    move    a1,x:(r6)+
        nop
am0_end rts

; ------------------------------------------------------------ RVB_DoReverb
reverb  move    x:RV1+F_ACT,a
        tst     a
        jne     rv_go
        rts
rv_go   move    #BL,r4
        move    #BR,r5
        move    #VLR,r0
        move    #NZ,r1
        do      #32,pre_end
        move    x:(r4)+,x0
        move    x:(r5)+,x1
        tfr     x0,a
        add     x1,a
        asr     a
        move    a1,x:(r0)+
        tfr     x0,a
        abs     a
        tfr     x1,b
        abs     b
        add     b,a
        move    a1,x:(r1)+
        nop
pre_end
        move    x:S_RVFB,x0
        move    #$4000,y0
        mpy     x0,y0,a
        move    a0,x:FBK
        move    x:S_RVLP,x0
        move    x0,x:LPF
        move    #RV1,r0
        move    #V1,r3
        jsr     rv_run
        move    #RV2,r0
        move    #V2,r3
        jsr     rv_run
; voutm = v1 + v2, then / 6 (alpha) or (11 voutm) >> 6
        move    #V1,r0
        move    #V2,r1
        move    #V1,r2
        move    x:S_ALPHA,a
        tst     a
        jne     cb_al
        move    #K11,y0
        do      #32,cb_end
        move    x:(r0)+,a
        move    x:(r1)+,x0
        add     x0,a
        move    a1,x0
        mpy     x0,y0,a
        move    a1,x:(r2)+
        nop
cb_end  jmp     cb_add
cb_al   move    #C6,y0
        do      #32,cba_end
        move    x:(r0)+,a
        move    x:(r1)+,x0
        add     x0,a
        tfr     a,b
        abs     a
        move    a1,x0
        mpy     x0,y0,a
        move    a1,a
        tst     b
        jge     cba1
        neg     a
cba1    move    a1,x:(r2)+
        nop
cba_end
; paint = CLIP16(paint + voutm), both channels
cb_add  move    #V1,r0
        move    #BL,r4
        move    #BR,r5
        do      #32,ad_end
        move    x:(r0)+,x0
        move    x:(r4),a
        add     x0,a
        cmp     #32759,a
        jle     ad1
        move    #CLIPHI,a
ad1     cmp     #-32760,a
        jge     ad2
        move    #CLIPLO,a
ad2     move    a1,x:(r4)+
        move    x:(r5),a
        add     x0,a
        cmp     #32759,a
        jle     ad3
        move    #CLIPHI,a
ad3     cmp     #-32760,a
        jge     ad4
        move    #CLIPLO,a
ad4     move    a1,x:(r5)+
        nop
ad_end  rts

; one reverb line over the frame: r0 its line, r3 its output block
rv_run  jsr     ld_cur
        move    x:CUR+F_DOUT,a
        move    x:CUR+F_RDB,r2
        jsr     zpre
        move    x:CUR+F_XFADE,a
        tst     a
        jeq     rr_nx
        move    x:CUR+F_DXF,a
        move    x:CUR+F_XFB,r2
        jsr     zpre
rr_nx   move    x:CUR+F_RDB,r0
        move    x:CUR+F_XFB,r1
        move    x:CUR+F_WBB,r2
        move    #VLR,r4
        move    #NZ,r5
        move    x:CUR+F_DOUT,a
        cmp     #32,a
        jge     rr_32
; 31 back: the last sample reads what this frame's first wrote
        move    #1,n7
        jsr     rv_line
        move    x:CUR+F_WBB,r7
        move    x:CUR+F_RDB,a
        add     #31,a
        move    x:(r7),x0
        move    a1,r7
        nop
        move    x0,x:(r7)
        move    #31,n7
        jsr     rv_line
        jmp     rr_done
rr_32   move    #32,n7
        jsr     rv_line
rr_done jsr     sv_cur
        rts

; RVB_DoReverbForOneDly for n7 samples: r0 delayed, r1 crossfade delayed,
; r2 written, r3 output, r4 VLR, r5 NZ
rv_line do      n7,rl_end
        move    x:(r0)+,a               ; delay
        move    x:(r1)+,x1              ; the crossfade's delayed sample
        move    x:(r4)+,y1              ; vlr
        move    x:(r5)+,b
        tst     a
        jne     rl_go
        tst     b
        jne     rl_go
        move    x:CUR+F_XFADE,b
        tst     b
        jne     rl_go
        clr     a                       ; nothing in or on the line: write 0
        move    a,x:CUR+F_LP0
        jmp     rl_t
rl_go   move    x:CUR+F_XFADE,b
        tst     b
        jeq     rl_nx
; crossfade, m = xfade: delay = delay m / 32 + xf (32 - m) / 32, truncating
        move    a1,x:TD
        move    b1,x0
        move    #32,b
        sub     x0,b
        move    b1,x0
        move    #$20000,y0
        mpy     x0,y0,b
        move    b0,y0                   ; (32 - m) << 18
        tfr     x1,b
        abs     b
        move    b1,x0
        mpy     x0,y0,b                 ; |xf| (32 - m) / 32
        move    b1,b
        tfr     x1,a
        tst     a
        jge     rl_s1
        neg     b
rl_s1   move    b1,x:TS
        move    x:TD,a
        abs     a
        move    a1,x:TA
        neg     a
        move    a1,x0
        mpy     x0,y0,b                 ; floor(-|d| (32 - m) / 32)
        move    b1,b
        move    x:TA,x0
        add     x0,b                    ; |d| m / 32
        move    x:TD,a
        tst     a
        jge     rl_s2
        neg     b
rl_s2   move    x:TS,x0
        add     x0,b
        tfr     b,a
        move    x:CUR+F_XFADE,b
        sub     #1,b
        move    b1,x:CUR+F_XFADE
        jne     rl_nx
        move    x:CUR+F_DXF,b           ; done: output = crossfade output, then moved on
        sub     #1,b
        move    b1,x:CUR+F_DOUT
rl_nx   tst     a
        jeq     rl_nd
        move    a1,x0
        move    x:FBK,y0
        mpy     x0,y0,a                 ; (feedback delay) >> 8
        move    a1,a
        add     y1,a
        cmp     #32759,a
        jle     rl_c1
        move    #CLIPHI,a
rl_c1   cmp     #-32760,a
        jge     rl_c2
        move    #CLIPLO,a
rl_c2   jmp     rl_lp
rl_nd   tfr     y1,a
rl_lp   move    x:LPF,b
        tst     b
        jeq     rl_t
        move    x:CUR+F_LP0,b           ; valt = (lp0 + val) >> 1
        move    a1,x:CUR+F_LP0
        add     a,b
        asr     b
        move    b1,a
rl_t    move    a1,x:(r2)+
        move    a1,x:(r3)+
        nop
rl_end  rts

; ------------------------------------------------------------ DLY_DoDelay
mono    move    x:MON+F_ACT,a
        tst     a
        jne     mo_go
        rts
mo_go   move    #MON,r0
        jsr     ld_cur
        move    x:CUR+F_DOUT,a
        move    x:CUR+F_RDB,r2
        jsr     zpre
        move    x:S_DLFB,x0
        move    #$4000,y0
        mpy     x0,y0,a
        move    a0,x:FBK
        move    #RDM,r0
        move    #WBM,r2
        move    #BL,r4
        move    #BR,r5
        move    #BL,r6
        move    #BR,r7
        do      #32,mo_end
        move    x:(r0)+,a               ; delay
        move    x:(r4)+,x0              ; left
        move    x:(r5)+,x1              ; right
        tst     a
        jne     mo_g
        tfr     x0,b
        tst     b
        jne     mo_g
        tfr     x1,b
        tst     b
        jne     mo_g
        jmp     mo_s
mo_g    move    a1,y1
        tfr     x0,a
        add     x1,a
        asr     a
        move    a1,a                    ; (left + right) >> 1
        move    x:FBK,y0
        mpy     y1,y0,b
        move    b1,b                    ; (feedback delay) >> 8
        add     b,a
        cmp     #32759,a
        jle     mo_c1
        move    #CLIPHI,a
mo_c1   cmp     #-32760,a
        jge     mo_c2
        move    #CLIPLO,a
mo_c2   move    x:S_DLLP,b
        tst     b
        jeq     mo_nl
        move    x:CUR+F_LP0,b           ; val = (lp0 + lp1 + val) / 3
        move    x:CUR+F_LP1,y1
        add     y1,b
        add     a,b
        move    b1,x:TSUM
        abs     b
        move    b1,y1
        move    #C3,y0
        mpy     y1,y0,a
        move    a1,a
        move    x:TSUM,b
        tst     b
        jge     mo_p
        neg     a
mo_p    move    x:CUR+F_LP1,y1
        move    y1,x:CUR+F_LP0
        move    a1,x:CUR+F_LP1
mo_nl   move    a1,x:(r2)+              ; the line
        asr     a
        asr     a
        move    a1,a                    ; val >> 2
        tfr     x0,b
        add     a,b
        cmp     #32759,b
        jle     mo_c3
        move    #CLIPHI,b
mo_c3   cmp     #-32760,b
        jge     mo_c4
        move    #CLIPLO,b
mo_c4   move    b1,x0
        tfr     x1,b
        add     a,b
        cmp     #32759,b
        jle     mo_c5
        move    #CLIPHI,b
mo_c5   cmp     #-32760,b
        jge     mo_c6
        move    #CLIPLO,b
mo_c6   move    b1,x1
        jmp     mo_t
mo_s    clr     a                       ; nothing in or on the line
        move    a,x:CUR+F_LP0
        move    a,x:CUR+F_LP1
        move    a,x:(r2)+
mo_t    move    x0,x:(r6)+
        move    x1,x:(r7)+
        nop
mo_end  jsr     sv_cur
        rts

; ------------------------------------------------------------ DLY_DoStereoDelay
stereo  move    x:STE+F_ACT,a
        tst     a
        jne     st_go
        rts
st_go   move    #STE,r0
        jsr     ld_cur
        move    x:CUR+F_DOUT,a
        move    x:CUR+F_RDB,r2
        jsr     zpre
        move    x:CUR+F_XFADE,a
        tst     a
        jeq     st_nx
        move    x:CUR+F_DXF,a
        move    x:CUR+F_XFB,r2
        jsr     zpre
st_nx   move    #RDS,r0
        move    #XFS,r1
        move    #WBS,r2
        move    #BL,r4
        do      #32,st_end
        move    x:(r0)+,a               ; delay
        move    x:(r1)+,y1              ; the crossfade's delayed sample
        move    x:(r4),x0               ; left
        tst     a
        jne     st_g
        tfr     x0,b
        tst     b
        jne     st_g
        move    x:CUR+F_XFADE,b
        tst     b
        jne     st_g
        clr     b                       ; nothing in or on the line: write 0
        move    b,x:(r2)+
        jmp     st_t
st_g    move    x:CUR+F_XFADE,b
        tst     b
        jeq     st_nf
; crossfade, m = xfade: delay = (xf (128 - m)) >> 7 + (delay m) >> 7
        move    a1,x:TD
        move    b1,x1
        move    #128,b
        sub     x1,b
        move    b1,x1
        move    #$8000,y0
        mpy     x1,y0,b
        move    b0,y0                   ; (128 - m) << 16
        mpy     y1,y0,b
        move    b1,x:TS                 ; (xf (128 - m)) >> 7
        move    x:TD,a
        neg     a
        move    a1,x1
        mpy     x1,y0,b
        move    b1,b
        move    x:TD,x1
        add     x1,b                    ; (delay m) >> 7 = delay + floor(-delay (128 - m) / 128)
        move    x:TS,x1
        add     x1,b
        tfr     b,a
        move    x:CUR+F_XFADE,b
        sub     #1,b
        move    b1,x:CUR+F_XFADE
        jne     st_nf
        move    x:CUR+F_DXF,b           ; done: output = crossfade output, then moved on
        sub     #1,b
        move    b1,x:CUR+F_DOUT
st_nf   tfr     x0,b
        cmp     #32759,b
        jle     st_c1
        move    #CLIPHI,b
st_c1   cmp     #-32760,b
        jge     st_c2
        move    #CLIPLO,b
st_c2   move    b1,x:(r2)+              ; the line takes CLIP16(left)
        move    a1,x0                   ; left = delay
st_t    move    x0,x:(r4)+
        nop
        nop
st_end  jsr     sv_cur
        rts

; ------------------------------------------------------------ out = CLIP16(dry + (room gain) >> 8)
output  move    #BL,r4
        move    #OUTL,r3
        move    #$8000,y1
        move    #$80,x1
        move    x:S_GAIN,a
        cmp     #256,a
        jeq     out_u
        move    a1,x0
        move    #$4000,y0
        mpy     x0,y0,a
        move    a0,y0                   ; gain << 15
        do      #64,out_end
        move    x:(r4)+,x0
        mpy     x0,y0,b
        move    b1,b                    ; (room gain) >> 8
        move    x:(r3),x0
        mpy     x0,y1,a
        move    a1,a                    ; the dry mix, int
        add     b,a
        cmp     #32759,a
        jle     out1
        move    #CLIPHI,a
out1    cmp     #-32760,a
        jge     out2
        move    #CLIPLO,a
out2    move    a1,x0
        mpy     x0,x1,a
        move    a0,x:(r3)+              ; back as s << 8
        nop
out_end rts
out_u   do      #64,ou_end
        move    x:(r4)+,b
        move    x:(r3),x0
        mpy     x0,y1,a
        move    a1,a
        add     b,a
        cmp     #32759,a
        jle     ou1
        move    #CLIPHI,a
ou1     cmp     #-32760,a
        jge     ou2
        move    #CLIPLO,a
ou2     move    a1,x0
        mpy     x0,x1,a
        move    a0,x:(r3)+
        nop
ou_end  rts

; ------------------------------------------------------------ end of frame
; this frame's writes go where the lines are; lines that ran move on; the
; parameters that arrived are applied; the next frame's reads follow
endframe move   #RV1,r0
        jsr     ef_line
        move    #RV2,r0
        jsr     ef_line
        move    #MON,r0
        jsr     ef_line
        move    #STE,r0
        jsr     ef_line
        jsr     params
        move    #RV1,r0
        jsr     rd_line
        move    #RV2,r0
        jsr     rd_line
        move    #MON,r0
        jsr     rd_line
        move    #STE,r0
        jsr     rd_line
        rts

ef_line jsr     ld_cur
        move    x:CUR+F_WRD,r1
        move    #4,n1
        move    x:CUR+F_W,x0
        move    x0,x:(r1+n1)
        move    x:S_RUN,a
        tst     a
        jeq     ef_done
        move    x:CUR+F_ACT,a
        tst     a
        jeq     ef_done
        move    x:CUR+F_W,a
        add     #128,a
        move    x:CUR+F_SIZE,x0
        cmp     x0,a
        jlt     ef_w
        sub     x0,a
ef_w    move    a1,x:CUR+F_W
        move    x:CUR+F_AGE,a
        add     #32,a
        cmp     #$400000,a
        jlt     ef_a
        move    #$400000,a
ef_a    move    a1,x:CUR+F_AGE
ef_done jsr     sv_cur
        rts

; read offsets: (write - 4 distance) mod size
rd_line jsr     ld_cur
        move    x:CUR+F_DOUT,x0
        jsr     offset
        move    x:CUR+F_RDD,r1
        move    #4,n1
        nop
        move    a1,x:(r1+n1)
        move    x:CUR+F_XFD,a
        tst     a
        jeq     rd_done
        move    x:CUR+F_DXF,x0
        jsr     offset
        move    x:CUR+F_XFD,r1
        nop
        move    a1,x:(r1+n1)
rd_done rts
offset  tfr     x0,b
        asl     b
        asl     b
        move    x:CUR+F_W,a
        sub     b,a
        jge     off_ok
        move    x:CUR+F_SIZE,x0
        add     x0,a
off_ok  rts

; ------------------------------------------------------------ the parameters
; S = KPARAM, then what RVB_CheckNewReverbVal, DLY_CheckNewDelayVal and
; DLY_CheckNewStereoDelayVal do when their length was applied again
params  move    #KPARAM,r0
        move    #S,r1
        do      #16,pa_end
        move    x:(r0)+,x0
        move    x0,x:(r1)+
        nop
pa_end
        move    x:S_RVGEN,x0
        move    x:GRV,a
        cmp     x0,a
        jeq     pa_dly
        move    x0,x:GRV
        move    x:S_RVS1,a
        tst     a
        jne     pa_rv
        clr     a                       ; room_size 0: both lines freed
        move    a,x:RV1+F_ACT
        move    a,x:RV2+F_ACT
        jmp     pa_dly
pa_rv   move    #RV1,r0
        move    x:S_RVS1,y1
        move    #32,x1                  ; REVERB_XFADE
        jsr     xsetup
        move    #RV2,r0
        move    x:S_RVS2,y1
        move    #32,x1
        jsr     xsetup
pa_dly  move    x:S_DLGEN,x0
        move    x:GDL,a
        cmp     x0,a
        jeq     pa_ste
        move    x0,x:GDL
        move    x:S_DLS,a
        tst     a
        jne     pa_dl
        clr     a
        move    a,x:MON+F_ACT
        jmp     pa_ste
pa_dl   move    a1,x:MON+F_DSAMP        ; set up or cleared: empty, output delaysamples back
        move    a1,x:MON+F_DOUT
        move    #1,x0
        move    x0,x:MON+F_ACT
        clr     a
        move    a,x:MON+F_AGE
        move    a,x:MON+F_LP0
        move    a,x:MON+F_LP1
pa_ste  move    x:S_STGEN,x0
        move    x:GST,a
        cmp     x0,a
        jeq     pa_done
        move    x0,x:GST
        move    x:S_STS,a
        tst     a
        jne     pa_st
        clr     a
        move    a,x:STE+F_ACT
        jmp     pa_done
pa_st   move    #STE,r0
        move    x:S_STS,y1
        move    #128,x1
        jsr     xsetup
pa_done rts

; RVB_SetUpDly / DLY_CheckNewStereoDelayVal on line r0: length y1, crossfade x1
xsetup  jsr     ld_cur
        move    x:CUR+F_ACT,a
        tst     a
        jne     xs_act
        move    y1,x:CUR+F_DSAMP        ; DLY_Init
        move    y1,x:CUR+F_DOUT
        move    #1,x0
        move    x0,x:CUR+F_ACT
        clr     a
        move    a,x:CUR+F_AGE
        move    a,x:CUR+F_XFADE
        move    a,x:CUR+F_LP0
        move    a,x:CUR+F_LP1
xs_act  move    x:CUR+F_DSAMP,a         ; a new length: crossfade to it (delaysamples stays)
        cmp     y1,a
        jeq     xs_done
        move    y1,x:CUR+F_DXF
        move    x1,x:CUR+F_XFADE
xs_done jsr     sv_cur
        rts

; ------------------------------------------------------------ helpers
; CUR = line r0, and back
ld_cur  move    r0,x:CURP
        move    #CUR,r1
        do      #F_N,ld_end
        move    x:(r0)+,x0
        move    x0,x:(r1)+
        nop
ld_end  rts
sv_cur  move    x:CURP,r0
        move    #CUR,r1
        do      #F_N,sv_end
        move    x:(r1)+,x0
        move    x0,x:(r0)+
        nop
sv_end  rts

; zeros at the head of block r2 for a read a samples back: those written
; before the line was set up (a - age of them, at most 32)
zpre    move    x:CUR+F_AGE,x0
        sub     x0,a
        jle     zp_done
        cmp     #32,a
        jle     zp_n
        move    #32,a
zp_n    move    a1,n7
        clr     b
        rep     n7
        move    b,x:(r2)+
zp_done rts

; ------------------------------------------------------------ first call
setup   move    #SZ_RV,x0
        move    x0,x:RV1+F_SIZE
        move    x0,x:RV2+F_SIZE
        move    #SZ_ST,x0
        move    x0,x:STE+F_SIZE
        move    #SZ_MO,x0
        move    x0,x:MON+F_SIZE
        move    #D_R1O,x0
        move    x0,x:RV1+F_RDD
        move    #D_R1X,x0
        move    x0,x:RV1+F_XFD
        move    #D_W1,x0
        move    x0,x:RV1+F_WRD
        move    #RD1,x0
        move    x0,x:RV1+F_RDB
        move    #XF1,x0
        move    x0,x:RV1+F_XFB
        move    #WB1,x0
        move    x0,x:RV1+F_WBB
        move    #D_R2O,x0
        move    x0,x:RV2+F_RDD
        move    #D_R2X,x0
        move    x0,x:RV2+F_XFD
        move    #D_W2,x0
        move    x0,x:RV2+F_WRD
        move    #RD2,x0
        move    x0,x:RV2+F_RDB
        move    #XF2,x0
        move    x0,x:RV2+F_XFB
        move    #WB2,x0
        move    x0,x:RV2+F_WBB
        move    #D_MO,x0
        move    x0,x:MON+F_RDD
        move    #D_WM,x0
        move    x0,x:MON+F_WRD
        move    #RDM,x0
        move    x0,x:MON+F_RDB
        move    #WBM,x0
        move    x0,x:MON+F_WBB
        move    #D_SO,x0
        move    x0,x:STE+F_RDD
        move    #D_SX,x0
        move    x0,x:STE+F_XFD
        move    #D_WS,x0
        move    x0,x:STE+F_WRD
        move    #RDS,x0
        move    x0,x:STE+F_RDB
        move    #XFS,x0
        move    x0,x:STE+F_XFB
        move    #WBS,x0
        move    x0,x:STE+F_WBB

; descriptors: r0 it, x0 next, x1 control, y0 block, y1 line's scratch offset, b size - 1
        move    #CTL_R,x1
        move    #D_R1O,r0
        move    #D_R1X,x0
        move    #RD1,y0
        move    #B_RV1,y1
        move    #SZ_RV-1,b
        jsr     mkdesc
        move    #D_R1X,r0
        move    #D_R2O,x0
        move    #XF1,y0
        jsr     mkdesc
        move    #D_R2O,r0
        move    #D_R2X,x0
        move    #RD2,y0
        move    #B_RV2,y1
        jsr     mkdesc
        move    #D_R2X,r0
        move    #D_MO,x0
        move    #XF2,y0
        jsr     mkdesc
        move    #D_MO,r0
        move    #D_SO,x0
        move    #RDM,y0
        move    #B_MO,y1
        move    #SZ_MO-1,b
        jsr     mkdesc
        move    #D_SO,r0
        move    #D_SX,x0
        move    #RDS,y0
        move    #B_ST,y1
        move    #SZ_ST-1,b
        jsr     mkdesc
        move    #D_SX,r0
        move    #EOL,x0
        move    #XFS,y0
        jsr     mkdesc
        move    #CTL_W,x1
        move    #D_W1,r0
        move    #D_W2,x0
        move    #WB1,y0
        move    #B_RV1,y1
        move    #SZ_RV-1,b
        jsr     mkdesc
        move    #D_W2,r0
        move    #D_WM,x0
        move    #WB2,y0
        move    #B_RV2,y1
        jsr     mkdesc
        move    #D_WM,r0
        move    #D_WS,x0
        move    #WBM,y0
        move    #B_MO,y1
        move    #SZ_MO-1,b
        jsr     mkdesc
        move    #D_WS,r0
        move    #FIFO2,x0
        move    #WBS,y0
        move    #B_ST,y1
        move    #SZ_ST-1,b
        jsr     mkdesc
        move    #D_R1O,x0
        move    x0,x:K_RHEAD
        move    #D_W1,x0
        move    x0,x:K_WHEAD
; this frame runs on the parameters already there; the lines are all empty
        jsr     params
        move    #1,x0
        move    x0,x:INIT
        rts

mkdesc  move    x0,x:(r0)+
        move    x1,x:(r0)+
        move    #32,x0
        move    x0,x:(r0)+
        move    y0,x:(r0)+
        move    #0,x0
        move    x0,x:(r0)+
        move    x:KSCR,a
        add     y1,a
        move    a1,x:(r0)+
        move    b1,x:(r0)+
        rts
