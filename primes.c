/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

/* 计算1到n的所有素数
	100是默认值，n可以在命令行输入
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <task.h>

int quiet;
int goal;
int buffer;

void
primetask(void *arg)
{
	Channel *c, *nc;
	int p, i;
	c = arg;
//	return;
	printf(">>>>> primetask\n");
	p = chanrecvul(c);
	if(p > goal)
		taskexitall(0);
	if(!quiet)
		printf("%d\n", p);
	nc = chancreate(sizeof(unsigned long), buffer);
	taskcreate(primetask, nc, 32768);
	for(;;){
		i = chanrecvul(c);
		if(i%p)
			chansendul(nc, i);
	}
}

void
taskmain(int argc, char **argv)
{
	/* 主任务入口
	 */
	int i;
	Channel *c;

	if(argc>1)
		goal = atoi(argv[1]);
	else
		goal = 100;
	printf("goal=%d\n", goal);

	/* 创建Channel */
	c = chancreate(sizeof(unsigned long), buffer);
	taskcreate(primetask, c, 32768);
	for(i=2;; i++) {
		printf(">>>>>>>taskmain\n");
		chansendul(c, i);
//		break;
	}
}

void*
emalloc(unsigned long n)
{
	return calloc(n ,1);
}

long
lrand(void)
{
	return rand();
}
