	idnt	"bug.c"
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	section	"CODE",code
	public	_test
	cnop	0,4
_test
	sub.w	#12,a7
	movem.l	l3,-(a7)
	move.l	_DOSBase,(0+l5,a7)
	move.l	(0+l5,a7),(4+l5,a7)
	jsr	-60(a6)
	move.l	d0,(8+l5,a7)
	move.l	(8+l5,a7),d0
l3	reg
l5	equ	0
	add.w	#12,a7
	rts
	public	_DOSBase
