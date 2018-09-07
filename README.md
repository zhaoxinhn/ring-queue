# ring-queue
unlock queue base on shared memory
usage:
1、void* pbuf = malloc(bufsize);//如果是进程通信这个地方需要使用shm
2、
rte_ring_enqueue / rte_ring_enqueue_bulk
rte_ring_dequeue / rte_ring_dequeue_bulk
