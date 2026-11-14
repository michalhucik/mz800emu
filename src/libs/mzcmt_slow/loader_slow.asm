DEFC	destination=$d400
DEFC	relocated=$0155
DEFC	hdrsize=$80
DEFC	TBL=$0300
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

	ld a,$fc
	ld hl, TBL
	ld de, TBL+1
	ld (hl),a
	ld b,$00
	ld a,(delayo+destination-relocated)
	ld c,a
	ldir
	ld a,$fd
	ld (hl),a
	ld a,(delayo+destination-relocated)
	ld c,a
	ldir
	ld a,$fe
	ld (hl),a
	ld a,(delayo+destination-relocated)
	ld c,a
	ldir
	ld a,$ff
	ld (hl),a
	ld c,$20
	ldir
	
;relocate code
include "reloc.asm"
	call relocs

.ofromx	
	ld hl,$0000
	exx

	out ($e4),a
	jp $e99a

relocs:
	exx
	ld de,$1200	
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
	xor a		; 4t
	ld ixl, a	; 8t	for checkxor

.sy1			; synchro pulse
	bit 5,(hl)
.syz	jp z, sy1
.sy2
	bit 5,(hl)
.synz	jp nz, sy2
	
	ld de,TBL 	;10t	pointer to the table
	ld b,$fe	; 7t	byte storage & byte complete indication by 1st zero during left shift
	ld a,b		; 4t	a and c the same
	ld c,a		; 4t
	scf		; 4t	carry for init
	
;============================
	jp enter	;10t	go to rutine!

.inv
			; before inv wait: 58t
.ps     bit 5,(hl)	; 12t
        jp  z, ps	; 10t

        xor 8		; 7t
	ld  (pe1),a 	; 13t
	ld  (synz),a 	; 13t
        xor 8        	; 7t
        ld  (po1),a  	; 13t
        ld  (syz),a  	; 13t

        jp  noinv	; 10t

			; 91t for noinv
			; 84t in the uplevel signal for inv
	
			; compensation: 2 loops + 34t = 86t
			; noinv: ;lacking 5t
.full
	and c		; 4t	late write of 2 bits to acc
	exx		; 4t
	ld (de),a	; 7t	write byte
	xor ixl		; 8t	xor for check
	ld ixl, a	; 8t	store check back
	inc de		; 6t
	exx		; 4t
	ld b,$fe	; 7t	init b again
			
			; section 48t

.next_bits

.high
	inc e
	bit 5,(hl)
.pe1	jp nz, high

	ld a,(de)	; 7t get the value from the table
	ld c,a		; 4t get the incomplete byte to a for shift lefts
	ld e,$00	; 7t reset e - pointer to the table

	ld a,b		; 4t byte storage
	rlca		; 4t shift 2 times
	rla		; 4t for 2 bits per pulse

			; section 30t
.enter
.low
	inc e
	bit 5,(hl)
.po1	jp z,low
	
	jp nc, full     ; 10t carry from left shifts of acc
	exx		; 4t
	out (c),a	; 12t border
	exx		; 4t
	and c		; 4t mask the byte storage with table record
	ld b,a		; 4t and store it to b
	ld a,e		; 4t check the final long pulse
	cp $30		; 7t over a thershold?
	jp c, next_bits	; 10t in range -> go for next bit handling
			
			; section 59t
.finish
	exx
	xor a
	out (c),a	; reset border
	exx
	ld a,ixl	
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


