DEFC	relocated=$0000
DEFC	destination=$0000
        org $10f0
DEFINE	INHDR	
INCLUDE	"mzfhdr.asm"

.odata
.osizex	
	defb $00,$00

.ofromx
	defb $00,$00
.oexecx
	defb $00,$00, $00, $00

	;defm "[MZFT]"

.startmain
.borc
	ld a,$08
	out ($ce),a
	call $073e
	ld (hl),$01	; $E003
	sub a
	ld d,a
	ld e,a
	;call $0308
	call $02be
.cpyromlp
	out ($e2),a	
	ld a,(de)
	out ($e0),a
	ld (de),a
	inc de
	bit 4,d
	jr z, cpyromlp

	ld a,$c3
	ld ($061f),a
	ld hl,border
	ld ($0620),hl
	ld hl,$0512
	ld (hl),$01
	ld hl,$0a4b
.delayx
	ld (hl),$00
	ld hl,(osizex+1)
	ld ($1102),hl
	ld a,$c9	; ret
	ld ($069f),a	; disable motor handling
	ld ($0700),a	; in order to save time
	call $04f8
	ld bc,$06cf
	defb $ed,$71	; undoc out (c),0
	out ($e2),a
	jp c,$e9aa
	ld hl,osizex+1
	jp $ed08

border:
	push bc
	ld a,(borc)
	xor $0f
	ld (borc),a
	ld bc,$06cf
	out (c),a
	pop bc
	ret	

.endmain


