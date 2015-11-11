/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"

/* 创建Channel 
 */
Channel*
chancreate(int elemsize, int bufsize)
{
	Channel *c;

	/*  malloc sizeof(Channel) + elemsize * bufsize
	 */
	c = malloc(sizeof *c+bufsize*elemsize);
	if(c == nil){
		fprint(2, "chancreate malloc: %r");
		exit(1);
	}
	memset(c, 0, sizeof *c);
	c->elemsize = elemsize;		// ?
	c->bufsize = bufsize;		// ?
	c->nbuf = 0;				// ?
	c->buf = (uchar*)(c+1);		// buf的其实地址
	return c;
}

/* bug - work out races */
void
chanfree(Channel *c)
{
	if(c == nil)
		return;
	free(c->name);
	free(c->arecv.a);
	free(c->asend.a);
	free(c);
}

/*  将alt加入Alt队列a里面
 */
static void
addarray(Altarray *a, Alt *alt)
{
	if(a->n == a->m){
		// 扩容 16 个单位
		a->m += 16;
		a->a = realloc(a->a, a->m*sizeof a->a[0]);
	}
	a->a[a->n++] = alt;
}

/*  删除下标为i的alt
 */
static void
delarray(Altarray *a, int i)
{
	--a->n;
	a->a[i] = a->a[a->n];
}

/*
 * doesn't really work for things other than CHANSND and CHANRCV
 * but is only used as arg to chanarray, which can handle it
 */
#define otherop(op)	(CHANSND+CHANRCV-(op))

/* 根据op获取Channel c的发送队列或者接收队列
 */
static Altarray*
chanarray(Channel *c, uint op)
{
	switch(op){
	default:
		return nil;
	case CHANSND:
		return &c->asend;
	case CHANRCV:
		return &c->arecv;
	}
}

/* 检查Alt a是否可以被执行
 */
static int
altcanexec(Alt *a)
{
	Altarray *ar;
	Channel *c;

	if(a->op == CHANNOP)
		return 0;
	c = a->c;
	if(c->bufsize == 0){	// 如果bufsize为0，则是通过发送接收队列共享数据的
		// 获取发送or接收队列
		ar = chanarray(c, otherop(a->op));
		return ar && ar->n;
	}else{
		switch(a->op){
		default:
			return 0;
		case CHANSND:
			// 如果是发送的情况，查看buffer还有没有空间了
			return c->nbuf < c->bufsize;
		case CHANRCV:
			// 如果是接收的情况，查看buffer还有没有数据
			return c->nbuf > 0;
		}
	}
}

/* 获取Alt a对应的队列
	发送队列 or 接收队列
   并把a加到队列中去
 */
static void
altqueue(Alt *a)
{
	Altarray *ar;

	ar = chanarray(a->c, a->op);
	addarray(ar, a);
}

/* 将Alt a从队列中删除
 */
static void
altdequeue(Alt *a)
{
	int i;
	Altarray *ar;

	ar = chanarray(a->c, a->op);
	if(ar == nil){
		fprint(2, "bad use of altdequeue op=%d\n", a->op);
		abort();
	}

	for(i=0; i<ar->n; i++)
		if(ar->a[i] == a){
			delarray(ar, i);
			return;
		}
	fprint(2, "cannot find self in altdq\n");
	abort();
}

/*  删除a指向的所有的Alt
 */
static void
altalldequeue(Alt *a)
{
	int i;

	for(i=0; a[i].op!=CHANEND && a[i].op!=CHANNOBLK; i++)
		if(a[i].op != CHANNOP)
			altdequeue(&a[i]);
}

/*  将src的内存数据移至dst
	src is nil，dst数据全为0
	n是数据大小
 */
static void
amove(void *dst, void *src, uint n)
{
	if(dst){
		if(src == nil)
			memset(dst, 0, n);
		else
			memmove(dst, src, n);
	}
}

/*
 * Actually move the data around.  There are up to three
 * players: the sender, the receiver, and the channel itself.
 * If the channel is unbuffered or the buffer is empty,
 * data goes from sender to receiver.  If the channel is full,
 * the receiver removes some from the channel and the sender
 * gets to put some in.
 */
 /*  数据拷贝
 	 1. 如果Channel的nbuf是0，将s->v拷贝到r->v
 	 2. 否则，如果r != nil，从Buffer中读取一个数据拷贝到r->v
 	 3. 如果s ！= nil, 将s->v写入到buffer中
  */
static void
altcopy(Alt *s, Alt *r)
{
	Alt *t;
	Channel *c;
	uchar *cp;

	/*
	 * Work out who is sender and who is receiver
	 */
	if(s == nil && r == nil)
		return;
	assert(s != nil);
	c = s->c;   // 使用的是s的channel
	if(s->op == CHANRCV){
		// 如果s其实是recieve，则s和r进行交换
		t = s;
		s = r;
		r = t;
	}
	assert(s==nil || s->op == CHANSND);
	assert(r==nil || r->op == CHANRCV);

	/*
	 * Channel is empty (or unbuffered) - copy directly.
	 */
	if(s && r && c->nbuf == 0){
		// 如果buffer的内部数量是0，只将s的v移至r的v
		amove(r->v, s->v, c->elemsize);
		return;
	}

	/*
	 * Otherwise it's always okay to receive and then send.
	 */
	if(r){
		// c->off 可以看成Buffer队列的头
		cp = c->buf + c->off*c->elemsize;
		// 将一个数据取出
		amove(r->v, cp, c->elemsize);
		--c->nbuf;
		if(++c->off == c->bufsize)
			c->off = 0;
	}
	if(s){
		// 将一个数据放到buffer内
		cp = c->buf + (c->off+c->nbuf)%c->bufsize*c->elemsize;
		amove(cp, s->v, c->elemsize);
		++c->nbuf;
	}
}

/*
	a->op is Recv: 从对应得Channel的发送队列随机取出一个Alt，
					并读取数据，Alt对应的Task可以进入就绪队列了
	a->op is Send: 将数据拷贝到Buffer中
 */
static void
altexec(Alt *a)
{
	int i;
	Altarray *ar;
	Alt *other;
	Channel *c;

	c = a->c;
	ar = chanarray(c, otherop(a->op));
	if(ar && ar->n){
		// 这里的逻辑目前看都是a->op == RECV才跑得到
		i = rand()%ar->n;
		other = ar->a[i];
		// a->v = other->v
		altcopy(a, other);
		// 将other从Channel的队列（接收队列）中删除
		altalldequeue(other->xalt);
		other->xalt[0].xalt = other;
		// Task重新进入就绪队列
		taskready(other->task);
	}else {
		// 将a->v的数据放到buffer里面
		altcopy(a, nil);
	}
}

#define dbgalt 0
/*
	遍历 Alt a 数组，如果数组中又可执行的Alt，
	随机选中一个执行，没有的话，最后会切换任务
 */
int
chanalt(Alt *a)
{
	int i, j, ncan, n, canblock;
	Channel *c;
	Task *t;

	needstack(512);
	for(i=0; a[i].op != CHANEND && a[i].op != CHANNOBLK; i++)
		;
	n = i;
	canblock = a[i].op == CHANEND;

	t = taskrunning;
	for(i=0; i<n; i++){
		a[i].task = t;
		a[i].xalt = a;
	}
if(dbgalt) print("alt ");
	ncan = 0;
	for(i=0; i<n; i++){
		c = a[i].c;
if(dbgalt) print(" %c:", "esrnb"[a[i].op]);
if(dbgalt) { if(c->name) print("%s", c->name); else print("%p", c); }
		if(altcanexec(&a[i])){
if(dbgalt) print("*");
			ncan++;
		}
	}
	if(ncan){
		j = rand()%ncan;
		for(i=0; i<n; i++){
			if(altcanexec(&a[i])){
				// 应该是随机选取一个执行
				if(j-- == 0){
if(dbgalt){
c = a[i].c;
print(" => %c:", "esrnb"[a[i].op]);
if(c->name) print("%s", c->name); else print("%p", c);
print("\n");
}
					altexec(&a[i]);
					return i;
				}
			}
		}
	}
if(dbgalt)print("\n");

	if(!canblock)
		return -1;

	for(i=0; i<n; i++){
		if(a[i].op != CHANNOP)
			altqueue(&a[i]);
	}

	taskswitch();   // 切换任务

	/*
	 * the guy who ran the op took care of dequeueing us
	 * and then set a[0].alt to the one that was executed.
	 */
	return a[0].xalt - a;
}

/*   对Channel进行操作
	 @param
	 	c : Channel
	 	op : 操作码
 */
static int
_chanop(Channel *c, int op, void *p, int canblock)
{
	Alt a[2];	// 用两个Alt，最后一个是标志

	a[0].c = c;
	a[0].op = op;
	a[0].v = p;
	a[1].op = canblock ? CHANEND : CHANNOBLK;
	if(chanalt(a) < 0)
		return -1;
	return 1;
}

// 发送一个数据，阻塞
int
chansend(Channel *c, void *v)
{
	return _chanop(c, CHANSND, v, 1);
}

// 发送一个数据，非阻塞
int
channbsend(Channel *c, void *v)
{
	return _chanop(c, CHANSND, v, 0);
}

// 接收一个数据，阻塞
int
chanrecv(Channel *c, void *v)
{
	return _chanop(c, CHANRCV, v, 1);
}

// 接收一个数据，非阻塞
int
channbrecv(Channel *c, void *v)
{
	return _chanop(c, CHANRCV, v, 0);
}

// 发送一个指针的指针，阻塞
int
chansendp(Channel *c, void *v)
{
	return _chanop(c, CHANSND, (void*)&v, 1);
}

// 接收一个数据，并返回，阻塞
void*
chanrecvp(Channel *c)
{
	void *v;

	_chanop(c, CHANRCV, (void*)&v, 1);
	return v;
}

// 发送一个指针的指针，非阻塞
int
channbsendp(Channel *c, void *v)
{
	return _chanop(c, CHANSND, (void*)&v, 0);
}

// 接收一个指针的指针，非阻塞
void*
channbrecvp(Channel *c)
{
	void *v;

	_chanop(c, CHANRCV, (void*)&v, 0);
	return v;
}

/* 发送unsigned long变量，阻塞 */
int
chansendul(Channel *c, ulong val)
{
	return _chanop(c, CHANSND, &val, 1);
}

// 接收一个unsigned long，阻塞
ulong
chanrecvul(Channel *c)
{
	ulong val;

	_chanop(c, CHANRCV, &val, 1);
	return val;
}

// 发送一个unsigned long， 非阻塞
int
channbsendul(Channel *c, ulong val)
{
	return _chanop(c, CHANSND, &val, 0);
}

// 接收一个unsigned long， 非阻塞
ulong
channbrecvul(Channel *c)
{
	ulong val;

	_chanop(c, CHANRCV, &val, 0);
	return val;
}

