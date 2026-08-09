	idnt	"bug.c"
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	section	"CODE",code
	public	_test
	cnop	0,4
_test
	movem.l	l3,-(a7)
	move.l	_DOSBase,a6
	jsr	-60(a6)
l1
l3	reg	a6
	movem.l	(a7)+,a6
l5	equ	4
	rts
	public	_DOSBase
