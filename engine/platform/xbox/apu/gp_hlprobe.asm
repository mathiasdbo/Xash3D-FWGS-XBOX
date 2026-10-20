; Transport probe for the room kernel (docs/r3d/divergences.md #180): what a
; stereo source sending at 1.0 delivers to a GP kernel, measured from the
; final mix (xbox_snd_gpprobe.c). Runs inside nxdk-ex's GP frame loop
; (lib/openal/dsp/gp_loop.asm; the ABI in lib/openal/docs/GP_KERNELS.md).
;
; Parameter 0 (AL_XBOX_GP_PARAM0):
;   0  OUT = in: the input replaces the dry mix; the codec gets the top 16 of
;      each 24-bit word, so a 16-bit source must come back unchanged
;   1  OUT = (in & $FF) << 8: the low 8 bits the codec would drop, moved up
;      where it sees them; an exact path delivers only zeros
;
; Assemble: python3 3rdparty/nxdk-ex/lib/openal/tools/dsp56asm.py
;   engine/platform/xbox/apu/gp_hlprobe.asm -o engine/platform/xbox/apu/gp_hlprobe.h --image

K_SCRATCH_BYTES equ $1000       ; image header: none used (one page declared)
K_DESCRIPTORS   equ 0           ; image header: no DMA of its own

KIN     equ     $E0             ; ABI (gp_loop.asm)
KPARAM  equ     $E8
OUTL    equ     $20             ; OUT R follows at $40: one 64-word run

        org     p:$100
kernel  move    x:KIN,r1        ; left block, the right one 32 words on
        move    #OUTL,r3
        move    x:KPARAM,a
        tst     a
        jne     low

        do      #64,copy_end
        move    x:(r1)+,x0
        move    x0,x:(r3)+
copy_end
        rts

; low = w - ((w >> 8) << 8), shifts only (the host simulator has no 'and')
low     do      #64,low_end
        move    x:(r1)+,a       ; w in A1, A0 clear
        tfr     a,b
        rep     #8
        asr     b
        move    b1,b            ; w >> 8, the bits shifted into B0 dropped
        rep     #8
        asl     b
        sub     b,a             ; w & $FF
        rep     #8
        asl     a
        move    a1,x:(r3)+
low_end
        rts
