#include "rte_ring.h"
#include <stdio.h>

int main()
{
	int i=0;
	int bufsize = 1024*6;
	void* pbuf = malloc(bufsize);
	int in[2000];
	int out[2000];
	for(i=0;i<2000;i++)
		in[i] = i;
	struct rte_ring* r = rte_ring_create(pbuf,bufsize,sizeof(int));
	int ret = 0;
	printf("%p\n",pbuf);
	printf("queue size:%d\n",r->size);
	printf("queue available:%d\n",r->capacity);
	for(i=0;i<1500;i++)
	{
		if(i==1400)
			printf("a");
		ret = rte_ring_enqueue(r, &in[i]);
		if(i%10==0) ring_info(r);
		//printf("%d enqueue:%d, prod_head:%d, prod_tail:%d, cons_head:%d, cons_tail:%d\n",i, ret, *r->prod_head,*r->prod_tail,*r->cons_head,*r->cons_tail);
	}
	for(i=0;i<1500;i++)
	{
		rte_ring_dequeue_bulk(r, out + i,1);
		printf("queue available:%d\n",(r->capacity - *r->prod_head + *r->cons_tail)%r->capacity);
	}
	printf("prod_head:%d, prod_tail:%d, cons_head:%d, cons_tail:%d\n",*r->prod_head,*r->prod_tail,*r->cons_head,*r->cons_tail);

	return 0;
}
