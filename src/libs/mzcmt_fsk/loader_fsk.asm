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

relocs:
	ld de,$1200
	exx
	ld bc,$06cf	; for border
	exx
	ex af,af'
	xor a		; for checksum
	ex af,af'
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
;========= debug =========
;       ld (de),a
;       inc de
;       jp pii
;=========================

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
.sy1
        bit 5,(hl)
.syz
        jp z, sy1
.sy2
        bit 5,(hl)
.synz
        jp nz, sy2

;===========================
; optimized code because time minimizing.
; Semantics: 
; ld a,$00	; loop counter
; ld b,$fe	; byte storage
; ld c,$fe	; initial value for byte storage 
; scf
;
	xor a	; 4t
	sub l	; 4t
	ld b,a	; 4t
	ld c,a	; 4t
	ld a,h	; 4t
	
			; 20t

;============================
	jp enter	;10t

.inv
			; before inv wait: 58t
.ps     bit 5,(hl)	; 12t
        jp  z, ps	; 10t

        xor 8		; 7t

	ld  (pe1),a 	; 13t
	ld  (synz),a 	; 13t
        xor 8 ;       	; 7t
        ld  (po1),a  	; 13t
        ld  (syz),a  	; 13t

        jp  noinv	; 10t

			; 91t for noinv
			; 84t in the uplevel signal for inv
	
			; compensation: 2 loops + 34t = 86t
			; noinv: ;lacking 5t

;=============================================
; byte full -> store it.
; 37t, but 37t saved from the undone part
; of inter-pulse routine -> timing fits
;=============================================
.full   
	ex af,af'	; 4b
	xor b		; 4b
	ex af,af'	; 4b
 	ex de,hl	; 4t
	ld (hl),b	; 7t store it
	ex de,hl	; 4t
	inc de		; 6t inc destination address for next byte
        ld  b,c		; 4t b reg: byte store
			; First zero rolled out of byte
			; identifies byte filled

;=====================================
; waiting for down edge, 26t per cycle
;=====================================           
.next_bit 
.high   dec a
        bit 5,(hl)
.pe1    jp  nz, high	; jp nz / jp z, modified byt polarity detecting code

;==================================================================
; decision if short or long pulse + preparation for a next bit, 19t
;==================================================================
.delayx    
			; 
	cp  $00		; 7t 	decision; value to be overwritten by loader code
			;
			;		  44khz, 3s/b: $e6: <=6 loops short, >=7 loops long
			;                 =================================================
			;	timing: (27t+15t) + n*26t gives 217t for 6 loops, 243t for 7 loops
			;	short pulse: 159t, long pulse: 317t; 238t is in the middle as an optimal decision point
			;
			;                 48khz, 3s/b: $e6: <=6 loops short, >=7 loops long
			; 		  =================================================
			;	timing: (27t+34t) + n*26t gives 217t for 6 loops, 243t for 7 loops
			;	short pulse: 146t, long pulse: 292t; 219t is in the middle as an optimal decision point
			;	real decision point rather towards longer pulse 
			; 	
	ld a,h		; 4t	prepare for next bit
        rl b         	; 8t
.enter	; entry point for data read
        
;===================================
; waiting for up edge, 26t per cycle
;===================================           
.low    dec a
        bit 5,(hl)	
.po1    jp  z, low	; jp z/nz, modified by polarity detecting code	

;====================================
; decision whether byte is complete
; and check for end of stream, 47t
;====================================
        jp  nc, FULL	;10t 
	exx		; 4t
	out (c),a	; 4t
	nop		; 4t
	nop		; 4t
	exx		; 4t

; end of stream identified by a looooong pulse
        cp  $c0		; 7t
        jp  nc, next_bit  ; 10t  

	ei
.run
	exx
	xor a
	out (c),a
	ex af,af'
.checksumx
	cp $00		; checksum value to be overwritten by loader code
	jr z, finish	; everything ok!
	scf		; set carry for error detection 
finish:
	ret

.reloc_end
DEFINE	IS_CHECKSUM
include	"ptrs.asm"

.endmain


