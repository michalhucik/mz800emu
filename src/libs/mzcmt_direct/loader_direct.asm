DEFC	destination=$d400
DEFC	relocated=$0155
DEFC	hdrsize=$80
        org relocated-hdrsize

INCLUDE "mzfhdr.asm"

.startmain
	ld a,$08
	out ($ce),a

.osizex	
	ld hl,$0000 
	ld ($1102),hl
	exx
	
.oexecx
	ld hl,$0000
	ld ($1106),hl

	out ($e0),a

;relocate code
include "reloc.asm"
	call relocs

.ofromx	
	ld hl,$0000
	exx

	out ($e4),a
	jp $e99a

.relocs
	ld de,$1200	
	exx
	ld bc,$06cf
	exx
.pilot   
        di
        ld hl,$e002
	ld bc,$10c2
	
.pii     
	xor a
	
.pi_low 
	inc a
        bit 5,(hl)
        jp z, pi_LOW
			; => 26t

.pi_high 
	add l		;4t
        bit 5,(hl)	;12r
        jp  nz, pi_HIGH	;10t
			; => 26t

.sync  

	cp  82		; 7t
        rl  b		; 8t
        cp  54		; 7t 
        jr  nc, pii	; 12/7t
			;==== 34/29t

        inc b		; 4t
        jr  nz , pii	; 12/7t
			;==== 45/40t
       
       	cp  34		; 7t
        
        ld  a,c    	; 4t

        jr  nc, inv	; 7/12t

.noinv
;===========================


	ld b, $20	; mask for e002
	xor a
	ex af,af'

.sy1			; synchro pulse
	bit 5,(hl)
.syz	jp z, sy1
;.sy2
;	bit 5,(hl)
;.synz	jp nz, sy2

	ld a,10
fsync:	dec a
	jp nz,fsync
	nop
	
	ld c,$00	
	nop
;============================
	jp enter	;10t	go to rutine!

.inv
			; before inv wait: 58t
.ps     bit 5,(hl)	; 12t
        jp  z, ps	; 10t

        xor 8		; 7t
;	ld  (pe1),a 	; 13t
;	ld  (synz),a 	; 13t
        xor 8        	; 7t
;        ld  (po1),a  	; 13t
        ld  (syz),a  	; 13t

        jp  noinv	; 10t

			; 91t for noinv
			; 84t in the uplevel signal for inv
	
			; compensation: 2 loops + 34t = 86t
			; noinv: ;lacking 5t
;========================================
.full
	ld a,c		; 4t
	ld (de),a	; 7t
	inc de		; 6t
	ex af,af'	; 4t
	xor c		; 4t
	ex af,af'	; 4t
	exx		; 4t
	dec hl		; 4t
	ld a,h		; 4t
	or l		; 4t
	exx		; 4t
	jp z, finish	; 10t
	ld c,$00	; 7t
			; 66t
			; ===
			; 112t
			; ====
.enter

;1st bit
	ld a,(hl)	; 7t

	and b		
	rlca		
	rlca		
	or c	
	ld c,a	
	nop
	nop
;	nop
;	nop
;	nop
			; 40t

;2nd bit
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
			; 32t
	ld a,(hl)

	and b		; 4t
	jp z, lz1	; 10t
.lo1
	bit 5,(hl)	; 12t
	jp nz, lo1	; 10t
	jp go1		; 10t

.lz1	
	bit 5,(hl)	; 12t
	jp z, lz1	; 10t
	jp go1		; 10t
.go1
	rlca		; 4t
	nop		; 4t
	nop		; 4t
	nop		; 4t
	or c		; 4t
	ld c,a		; 4t

	ld a,4		; 7t
;	ld a,3		; 7t

.wl1			; 56t
	dec a		; 
	jp nz,wl1	; 

	nop		; 4t
			; 80t + 31t = 111t
			;==================
;3rd bit
	ld a,(hl)

	and b
	nop
	nop
	nop
	or c
	ld c,a
	nop
;	nop
;	nop
;	nop
		; 40t

;4th bit
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
		; 32t

	ld a,(hl)

	and b
	jp z, lz2	; 10t
.lo2
	bit 5,(hl)	; 12t
	jp nz, lo2	; 10t
	jp go2		; 10t

.lz2	
	bit 5,(hl)	; 12t
	jp z, lz2	; 10t
	jp go2		; 10t
.go2
	rrca
	nop
	or c
	ld c,a

	ld a,5		; 7t
;	ld a,4		; 7t

.wl2			; 70t
	dec a		; 
	jp nz,wl2	; 
			; 113t
;5th bit
	ld a,(hl)
	and b
	rrca
	rrca
	or c
	ld c,a
	nop
	nop
;	nop
;	nop
;	nop
			; 40t
;6th bit
	nop
	nop
	nop
	nop
	nop
	nop
	nop
	nop
			; 32t

	ld a,(hl)	; 7t

	and b
	jp z, lz3	; 10t
.lo3
	bit 5,(hl)	; 12t
	jp nz, lo3	; 10t
	jp go3		; 10t

.lz3	
	bit 5,(hl)	; 12t
	jp z, lz3	; 10t
	jp go3		; 10t
.go3
	rrca
	rrca
	rrca
	or c
	ld c,a

;	ld a,3		; 7t
	ld a,4		; 7t

.wl3			; 56t
	dec a		; 
	jp nz,wl3	; 
	nop
	nop
			; 111t

;7th bit
	ld a,(hl)	; 7t

	and b
	rrca
	rrca
	rrca
	rrca
	or c
	ld c,a

	exx
	out (c),a
	exx
	nop
		; 40t
;8th bit
;	nop
;	nop
;	nop
	nop
	nop
	nop
	nop
	nop
			; 32t

	ld a,(hl)	; 7t

	and b
	jp z, lz4	; 10t
.lo4
	bit 5,(hl)	; 12t
	jp nz, lo4	; 10t
	jp go4		; 10t

.lz4	
	bit 5,(hl)	; 12t
	jp z, lz4	; 10t
	jp go4		; 10t
.go4
	rlca
	rlca
	rlca
	or c
	ld c,a

	jp full		
			; 50t	
.finish
	exx
	xor a
	out (c),a	; reset border
	exx
	ex af,af'
	scf
	ccf
.checksumx
	cp $00		; check the overall xor 
	jp z,fin
	scf
.fin
	ret
.delayx	defb $00
.delayo	defb $00
.reloc_end
DEFINE	IS_CHECKSUM
include	"ptrs.asm"

.endmain


