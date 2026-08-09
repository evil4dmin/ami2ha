	idnt	"src/net/socket.c"
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	section	"CODE",code
	public	_net_lib_open
	cnop	0,4
_net_lib_open
	movem.l	l13,-(a7)
	tst.l	_SocketBase
	beq	l5
	moveq	#0,d0
	bra	l1
l5
	moveq	#0,d2
	bra	l7
l6
	move.l	d2,d0
	lsl.l	#2,d0
	lea	l3,a0
	add.l	d0,a0
	move.l	(a0),d0
	lea	l10,a1
	move.l	_SysBase,a6
	jsr	-552(a6)
	move.l	d0,a0
	move.l	a0,_SocketBase
	beq	l12
	move.l	_SocketBase,a0
	moveq	#0,d0
	move.w	(20,a0),d0
	move.l	d0,_net_socket_api_version
	moveq	#0,d0
	bra	l1
l12
l9
	addq.l	#1,d2
l7
	moveq	#5,d0
	cmp.l	d2,d0
	bhi	l6
l8
	moveq	#-1,d0
l1
l13	reg	a6/d2
	movem.l	(a7)+,a6/d2
l15	equ	8
	rts
	cnop	0,4
l10
	dc.b	98
	dc.b	115
	dc.b	100
	dc.b	115
	dc.b	111
	dc.b	99
	dc.b	107
	dc.b	101
	dc.b	116
	dc.b	46
	dc.b	108
	dc.b	105
	dc.b	98
	dc.b	114
	dc.b	97
	dc.b	114
	dc.b	121
	dc.b	0
	cnop	0,4
l3
	dc.l	4
	dc.l	3
	dc.l	2
	dc.l	1
	dc.l	0
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_lib_close
	cnop	0,4
_net_lib_close
	movem.l	l20,-(a7)
	tst.l	_SocketBase
	beq	l19
	move.l	_SocketBase,a1
	move.l	_SysBase,a6
	jsr	-414(a6)
	move.l	#0,_SocketBase
l19
l16
l20	reg	a6
	movem.l	(a7)+,a6
l22	equ	4
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_error_text
	cnop	0,4
_net_error_text
	movem.l	l37,-(a7)
	move.l	(4+l39,a7),d1
	move.l	d1,d0
	sub.l	#-4,d0
	beq	l34
	subq.l	#1,d0
	beq	l32
	subq.l	#1,d0
	beq	l30
	subq.l	#1,d0
	beq	l28
	subq.l	#1,d0
	beq	l26
	bra	l25
l26
	move.l	#l27,d0
	bra	l23
l28
	move.l	#l29,d0
	bra	l23
l30
	move.l	#l31,d0
	bra	l23
l32
	move.l	#l33,d0
	bra	l23
l34
	move.l	#l35,d0
	bra	l23
l25
	move.l	#l36,d0
l23
l37	reg
l39	equ	0
	rts
; stacksize=0
	cnop	0,4
l27
	dc.b	111
	dc.b	107
	dc.b	0
	cnop	0,4
l29
	dc.b	110
	dc.b	111
	dc.b	32
	dc.b	84
	dc.b	67
	dc.b	80
	dc.b	47
	dc.b	73
	dc.b	80
	dc.b	32
	dc.b	115
	dc.b	116
	dc.b	97
	dc.b	99
	dc.b	107
	dc.b	32
	dc.b	114
	dc.b	117
	dc.b	110
	dc.b	110
	dc.b	105
	dc.b	110
	dc.b	103
	dc.b	32
	dc.b	40
	dc.b	98
	dc.b	115
	dc.b	100
	dc.b	115
	dc.b	111
	dc.b	99
	dc.b	107
	dc.b	101
	dc.b	116
	dc.b	46
	dc.b	108
	dc.b	105
	dc.b	98
	dc.b	114
	dc.b	97
	dc.b	114
	dc.b	121
	dc.b	32
	dc.b	110
	dc.b	111
	dc.b	116
	dc.b	32
	dc.b	102
	dc.b	111
	dc.b	117
	dc.b	110
	dc.b	100
	dc.b	41
	dc.b	0
	cnop	0,4
l31
	dc.b	104
	dc.b	111
	dc.b	115
	dc.b	116
	dc.b	32
	dc.b	110
	dc.b	111
	dc.b	116
	dc.b	32
	dc.b	102
	dc.b	111
	dc.b	117
	dc.b	110
	dc.b	100
	dc.b	0
	cnop	0,4
l33
	dc.b	99
	dc.b	111
	dc.b	117
	dc.b	108
	dc.b	100
	dc.b	32
	dc.b	110
	dc.b	111
	dc.b	116
	dc.b	32
	dc.b	99
	dc.b	114
	dc.b	101
	dc.b	97
	dc.b	116
	dc.b	101
	dc.b	32
	dc.b	115
	dc.b	111
	dc.b	99
	dc.b	107
	dc.b	101
	dc.b	116
	dc.b	0
	cnop	0,4
l35
	dc.b	99
	dc.b	111
	dc.b	117
	dc.b	108
	dc.b	100
	dc.b	32
	dc.b	110
	dc.b	111
	dc.b	116
	dc.b	32
	dc.b	99
	dc.b	111
	dc.b	110
	dc.b	110
	dc.b	101
	dc.b	99
	dc.b	116
	dc.b	0
	cnop	0,4
l36
	dc.b	117
	dc.b	110
	dc.b	107
	dc.b	110
	dc.b	111
	dc.b	119
	dc.b	110
	dc.b	32
	dc.b	101
	dc.b	114
	dc.b	114
	dc.b	111
	dc.b	114
	dc.b	0
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_socket_init
	cnop	0,4
_net_socket_init
	movem.l	l42,-(a7)
	move.l	(4+l44,a7),a1
	moveq	#-1,d0
	move.l	d0,(a1)
	move.l	#0,(4,a1)
l40
l42	reg
l44	equ	0
	rts
; stacksize=0
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_socket_is_open
	cnop	0,4
_net_socket_is_open
	movem.l	l50,-(a7)
	move.l	(4+l52,a7),a0
	tst.l	(a0)
	bge	l47
l49
	moveq	#0,d0
	bra	l48
l47
	moveq	#1,d0
l48
l45
l50	reg
l52	equ	0
	rts
; stacksize=0
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	cnop	0,4
l53
	subq.w	#4,a7
	movem.l	l59,-(a7)
	move.l	(8+l61,a7),d3
	moveq	#1,d0
	move.l	d0,(0+l61,a7)
	lea	(0+l61,a7),a0
	move.l	#2147772030,d1
	move.l	d3,d0
	move.l	_SocketBase,a6
	jsr	-114(a6)
	move.l	d0,d2
	beq	l56
l58
	moveq	#0,d0
	bra	l57
l56
	moveq	#1,d0
l57
l54
l59	reg	a6/d2/d3
	movem.l	(a7)+,a6/d2/d3
l61	equ	12
	addq.w	#4,a7
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_connect
	cnop	0,4
_net_connect
	sub.w	#28,a7
	movem.l	l82,-(a7)
	move.l	(40+l84,a7),d6
	move.l	(36+l84,a7),a4
	move.l	(32+l84,a7),a2
	move.l	a2,-(a7)
	jsr	_net_socket_init
	addq.w	#4,a7
	tst.l	_SocketBase
	bne	l65
l64
	moveq	#-1,d0
	bra	l62
l65
	moveq	#16,d2
	moveq	#0,d0
	lea	(0+l84,a7),a0
	inline
	move.l	a0,a1
	subq.l	#8,d2
	bcs	.l3
	move.l	d0,d1
	lsl.l	#8,d0
	move.b	d1,d0
	addq.l	#4,d2
	move.l	d0,d1
	swap	d0
	move.w	d1,d0
	move.l	a1,d1
	lsr.l	#1,d1
	bcc	.l1
	move.b	d0,(a0)+
	addq.l	#1,d1
	subq.l	#1,d2
.l1
	lsr.l	#1,d1
	bcc	.l2
	move.w	d0,(a0)+
	subq.l	#2,d2
.l2
	move.l	d0,(a0)+
	subq.l	#4,d2
	bcc	.l2
	addq.l	#3,d2
	bcs	.l4
	bra	.l5
.l3
	addq.l	#7,d2
	bcc	.l5
.l4
	move.b	d0,(a0)+
	dbf	d2,.l4
.l5
	move.l	a1,d0
	einline
	move.l	d0,a1
	move.b	#2,(1+l84,a7)
	move.w	d6,(2+l84,a7)
	move.l	a4,a0
	move.l	_SocketBase,a6
	jsr	-180(a6)
	move.l	d0,d4
	cmp.l	#4294967295,d4
	beq	l67
l66
	move.l	d4,(4+l84,a7)
	bra	l68
l67
	move.l	a4,a0
	move.l	_SocketBase,a6
	jsr	-210(a6)
	move.l	d0,a3
	tst.l	a3
	beq	l69
l72
	tst.l	(16,a3)
	beq	l69
l71
	move.l	(16,a3),a0
	tst.l	(a0)
	bne	l70
l69
	moveq	#-2,d0
	bra	l62
l70
	move.l	(12,a3),d2
	move.l	(16,a3),a0
	move.l	(a0),a1
	lea	(4+l84,a7),a0
	inline
	move.l	a0,d0
	subq.l	#4,d2
	bcs	.l3
	move.l	d0,d1
	lsr.l	#1,d1
	bcc	.l1
	move.b	(a1)+,(a0)+
	subq.l	#1,d2
	bcs	.l3
	addq.l	#1,d1
.l1
	lsr.l	#1,d1
	bcc	.l2
	move.w	(a1)+,(a0)+
	subq.l	#2,d2
	bcs	.l3
.l2
	move.l	(a1)+,(a0)+
	subq.l	#4,d2
	bcc	.l2
.l3
	addq.l	#3,d2
	bcc	.l5
.l4
	move.b	(a1)+,(a0)+
	dbf	d2,.l4
.l5
	einline
l68
	moveq	#0,d2
	moveq	#1,d1
	moveq	#2,d0
	move.l	_SocketBase,a6
	jsr	-30(a6)
	move.l	d0,d3
	move.l	d3,(a2)
	bge	l74
l73
	moveq	#-3,d0
	bra	l62
l74
	move.l	(a2),-(a7)
	jsr	l53
	addq.w	#4,a7
	tst.l	d0
	bne	l76
l75
	move.l	a2,-(a7)
	jsr	_net_disconnect
	moveq	#-3,d0
	addq.w	#4,a7
	bra	l62
l76
	moveq	#16,d1
	lea	(0+l84,a7),a0
	move.l	(a2),d0
	move.l	_SocketBase,a6
	jsr	-54(a6)
	move.l	d0,d2
	move.l	d2,d5
	bne	l78
l77
	moveq	#0,d0
	bra	l62
l78
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#36,d1
	cmp.l	d0,d1
	beq	l79
l81
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#35,d1
	cmp.l	d0,d1
	bne	l80
l79
	moveq	#1,d0
	move.l	d0,(4,a2)
	moveq	#1,d0
	bra	l62
l80
	move.l	a2,-(a7)
	jsr	_net_disconnect
	moveq	#-4,d0
	addq.w	#4,a7
l62
l82	reg	a2/a3/a4/a6/d2/d3/d4/d5/d6
	movem.l	(a7)+,a2/a3/a4/a6/d2/d3/d4/d5/d6
l84	equ	36
	add.w	#28,a7
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_connect_done
	cnop	0,4
_net_connect_done
	subq.w	#8,a7
	movem.l	l98,-(a7)
	move.l	(12+l100,a7),a2
	move.l	#0,(0+l100,a7)
	moveq	#4,d0
	move.l	d0,(4+l100,a7)
	tst.l	(4,a2)
	bne	l88
l87
	move.l	a2,-(a7)
	jsr	_net_socket_is_open
	addq.w	#4,a7
	tst.l	d0
	beq	l90
l89
	moveq	#0,d0
	bra	l91
l90
	moveq	#-4,d0
l91
	bra	l85
l88
	lea	(4+l100,a7),a0
	move.l	a0,a1
	lea	(0+l100,a7),a0
	move.l	#4103,d2
	move.l	#65535,d1
	move.l	(a2),d0
	move.l	_SocketBase,a6
	jsr	-96(a6)
	move.l	d0,d3
	bge	l93
l92
	move.l	a2,-(a7)
	jsr	_net_disconnect
	moveq	#-4,d0
	addq.w	#4,a7
	bra	l85
l93
	moveq	#36,d0
	cmp.l	(0+l100,a7),d0
	bne	l95
l94
	moveq	#1,d0
	bra	l85
l95
	tst.l	(0+l100,a7)
	beq	l97
l96
	move.l	a2,-(a7)
	jsr	_net_disconnect
	moveq	#-4,d0
	addq.w	#4,a7
	bra	l85
l97
	move.l	#0,(4,a2)
	moveq	#0,d0
l85
l98	reg	a2/a6/d2/d3
	movem.l	(a7)+,a2/a6/d2/d3
l100	equ	16
	addq.w	#8,a7
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_disconnect
	cnop	0,4
_net_disconnect
	movem.l	l105,-(a7)
	move.l	(4+l107,a7),a2
	tst.l	(a2)
	blt	l104
l103
	move.l	(a2),d0
	move.l	_SocketBase,a6
	jsr	-120(a6)
	move.l	d0,d1
l104
	move.l	a2,-(a7)
	jsr	_net_socket_init
	addq.w	#4,a7
l101
l105	reg	a2/a6
	movem.l	(a7)+,a2/a6
l107	equ	8
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_send
	cnop	0,4
_net_send
	movem.l	l119,-(a7)
	move.l	(12+l121,a7),d5
	move.l	(8+l121,a7),a3
	move.l	(4+l121,a7),a2
	tst.l	(a2)
	bge	l111
l110
	moveq	#-1,d0
	bra	l108
l111
	tst.l	d5
	bne	l113
l112
	moveq	#0,d0
	bra	l108
l113
	moveq	#0,d2
	move.l	d5,d1
	move.l	a3,a0
	move.l	(a2),d0
	move.l	_SocketBase,a6
	jsr	-66(a6)
	move.l	d0,d3
	move.l	d3,d4
	blt	l115
l114
	move.l	d4,d0
	bra	l108
l115
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#35,d1
	cmp.l	d0,d1
	beq	l116
l118
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#35,d1
	cmp.l	d0,d1
	bne	l117
l116
	moveq	#0,d0
	bra	l108
l117
	moveq	#-2,d0
l108
l119	reg	a2/a3/a6/d2/d3/d4/d5
	movem.l	(a7)+,a2/a3/a6/d2/d3/d4/d5
l121	equ	28
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_recv
	cnop	0,4
_net_recv
	movem.l	l133,-(a7)
	move.l	(12+l135,a7),d5
	move.l	(8+l135,a7),a3
	move.l	(4+l135,a7),a2
	tst.l	(a2)
	bge	l125
l124
	moveq	#-1,d0
	bra	l122
l125
	moveq	#0,d2
	move.l	d5,d1
	move.l	a3,a0
	move.l	(a2),d0
	move.l	_SocketBase,a6
	jsr	-78(a6)
	move.l	d0,d3
	move.l	d3,d4
	ble	l127
l126
	move.l	d4,d0
	bra	l122
l127
	tst.l	d4
	bne	l129
l128
	moveq	#-1,d0
	bra	l122
l129
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#35,d1
	cmp.l	d0,d1
	beq	l130
l132
	move.l	_SocketBase,a6
	jsr	-162(a6)
	moveq	#35,d1
	cmp.l	d0,d1
	bne	l131
l130
	moveq	#0,d0
	bra	l122
l131
	moveq	#-2,d0
l122
l133	reg	a2/a3/a6/d2/d3/d4/d5
	movem.l	(a7)+,a2/a3/a6/d2/d3/d4/d5
l135	equ	28
	rts
	machine	68020
	opt o+,ol+,op+,oc+,ot+,oj+,ob+,om+
	public	_net_wait
	cnop	0,4
_net_wait
	sub.w	#84,a7
	movem.l	l174,-(a7)
	move.l	(96+l176,a7),d7
	move.l	(92+l176,a7),d4
	move.l	(100+l176,a7),d3
	move.l	(104+l176,a7),a5
	move.l	(88+l176,a7),a4
	move.l	d7,(72+l176,a7)
	moveq	#0,d5
	tst.l	a5
	beq	l139
	move.l	#0,(a5)
l139
	tst.l	(108+l176,a7)
	beq	l141
	move.l	(108+l176,a7),a0
	move.l	#0,(a0)
l141
	moveq	#32,d2
	moveq	#0,d0
	lea	(0+l176,a7),a0
	inline
	move.l	a0,a1
	subq.l	#8,d2
	bcs	.l3
	move.l	d0,d1
	lsl.l	#8,d0
	move.b	d1,d0
	addq.l	#4,d2
	move.l	d0,d1
	swap	d0
	move.w	d1,d0
	move.l	a1,d1
	lsr.l	#1,d1
	bcc	.l1
	move.b	d0,(a0)+
	addq.l	#1,d1
	subq.l	#1,d2
.l1
	lsr.l	#1,d1
	bcc	.l2
	move.w	d0,(a0)+
	subq.l	#2,d2
.l2
	move.l	d0,(a0)+
	subq.l	#4,d2
	bcc	.l2
	addq.l	#3,d2
	bcs	.l4
	bra	.l5
.l3
	addq.l	#7,d2
	bcc	.l5
.l4
	move.b	d0,(a0)+
	dbf	d2,.l4
.l5
	move.l	a1,d0
	einline
	move.l	d0,a1
	moveq	#32,d2
	moveq	#0,d0
	lea	(32+l176,a7),a0
	inline
	move.l	a0,a1
	subq.l	#8,d2
	bcs	.l3
	move.l	d0,d1
	lsl.l	#8,d0
	move.b	d1,d0
	addq.l	#4,d2
	move.l	d0,d1
	swap	d0
	move.w	d1,d0
	move.l	a1,d1
	lsr.l	#1,d1
	bcc	.l1
	move.b	d0,(a0)+
	addq.l	#1,d1
	subq.l	#1,d2
.l1
	lsr.l	#1,d1
	bcc	.l2
	move.w	d0,(a0)+
	subq.l	#2,d2
.l2
	move.l	d0,(a0)+
	subq.l	#4,d2
	bcc	.l2
	addq.l	#3,d2
	bcs	.l4
	bra	.l5
.l3
	addq.l	#7,d2
	bcc	.l5
.l4
	move.b	d0,(a0)+
	dbf	d2,.l4
.l5
	move.l	a1,d0
	einline
	move.l	d0,a1
	tst.l	a4
	beq	l143
l144
	tst.l	(a4)
	blt	l143
l142
	cmp.l	#256,(a4)
	bcc	l146
l145
	lea	(0+l176,a7),a0
	move.l	(a4),d0
	lsr.l	#5,d0
	lsl.l	#2,d0
	add.l	d0,a0
	moveq	#31,d0
	and.l	(a4),d0
	moveq	#1,d1
	lsl.l	d0,d1
	move.l	d1,d0
	or.l	d0,(a0)
	move.l	(a0),d0
	bra	l147
l146
	moveq	#0,d0
l147
	tst.l	d4
	beq	l149
	cmp.l	#256,(a4)
	bcc	l151
l150
	lea	(32+l176,a7),a0
	move.l	(a4),d0
	lsr.l	#5,d0
	lsl.l	#2,d0
	add.l	d0,a0
	moveq	#31,d0
	and.l	(a4),d0
	moveq	#1,d1
	lsl.l	d0,d1
	move.l	d1,d0
	or.l	d0,(a0)
	move.l	(a0),d0
	bra	l152
l151
	moveq	#0,d0
l152
l149
	moveq	#1,d5
	add.l	(a4),d5
l143
	tst.l	d3
	blt	l154
l153
	move.l	d3,d0
	divs.l	#1000,d0
	move.l	d0,(64+l176,a7)
	move.l	d3,d1
	divsl.l	#1000,d0:d1
	move.l	#1000,d1
	muls.l	d0,d1
	move.l	d1,(68+l176,a7)
l154
	lea	(72+l176,a7),a0
	move.l	a0,d1
	tst.l	d3
	blt	l156
l155
	lea	(64+l176,a7),a0
	bra	l157
l156
	move.l	#0,a0
l157
	move.l	a0,a3
	move.l	#0,a2
	tst.l	d4
	beq	l159
l158
	lea	(32+l176,a7),a0
	bra	l160
l159
	move.l	#0,a0
l160
	move.l	a0,a1
	lea	(0+l176,a7),a0
	move.l	d5,d0
	move.l	_SocketBase,a6
	jsr	-126(a6)
	move.l	d0,d2
	move.l	d2,d6
	ble	l162
l164
	tst.l	a4
	beq	l162
l163
	tst.l	(a4)
	blt	l162
l161
	tst.l	a5
	beq	l166
l167
	cmp.l	#256,(a4)
	bcc	l166
l168
	lea	(0+l176,a7),a0
	move.l	(a4),d0
	lsr.l	#5,d0
	lsl.l	#2,d0
	add.l	d0,a0
	moveq	#31,d0
	and.l	(a4),d0
	moveq	#1,d1
	lsl.l	d0,d1
	move.l	d1,d0
	and.l	(a0),d0
	beq	l166
l165
	moveq	#1,d0
	move.l	d0,(a5)
l166
	tst.l	d4
	beq	l170
l172
	tst.l	(108+l176,a7)
	beq	l170
l171
	cmp.l	#256,(a4)
	bcc	l170
l173
	lea	(32+l176,a7),a0
	move.l	(a4),d0
	lsr.l	#5,d0
	lsl.l	#2,d0
	add.l	d0,a0
	moveq	#31,d0
	and.l	(a4),d0
	moveq	#1,d1
	lsl.l	d0,d1
	move.l	d1,d0
	and.l	(a0),d0
	beq	l170
l169
	move.l	(108+l176,a7),a0
	moveq	#1,d0
	move.l	d0,(a0)
l170
l162
	move.l	(72+l176,a7),d0
l136
l174	reg	a2/a3/a4/a5/a6/d2/d3/d4/d5/d6/d7
	movem.l	(a7)+,a2/a3/a4/a5/a6/d2/d3/d4/d5/d6/d7
l176	equ	44
	add.w	#84,a7
	rts
	public	_SocketBase
	section	"DATA",data
	cnop	0,4
_SocketBase
	dc.l	0
	public	_net_socket_api_version
	cnop	0,4
_net_socket_api_version
	dc.l	0
	public	_SysBase
