#include <stdlib.h>
#include <stdio.h>
#include "threadpool.h"

threadpool* create_threadpool(int num_threads_in_pool, int max_queue_size) {
    if (num_threads_in_pool > MAXT_IN_POOL || num_threads_in_pool <= 0)
        return nullptr;
    if (max_queue_size > MAXW_IN_QUEUE || max_queue_size <= 0)
        return nullptr;
    threadpool *pThreadpoolSt = (threadpool *) malloc(sizeof(threadpool));
    if (pThreadpoolSt == NULL) {
        perror("malloc");
        return nullptr;
    }
    pThreadpoolSt->num_threads = num_threads_in_pool;
    pThreadpoolSt->max_qsize = max_queue_size;
    pThreadpoolSt->qsize = 0;
    pThreadpoolSt->threads = (pthread_t *) malloc(sizeof(pthread_t) * num_threads_in_pool);
    if (pThreadpoolSt->threads == NULL) {
        perror("malloc");
        // free pThreadpoolSt
        return nullptr;
    }
    pThreadpoolSt->qhead = pThreadpoolSt->qtail = nullptr;
    pthread_mutex_init(&pThreadpoolSt->qlock, nullptr);
    pthread_cond_init(&pThreadpoolSt->q_not_empty, nullptr);
    pthread_cond_init(&pThreadpoolSt->q_not_full, nullptr);
    pThreadpoolSt->shutdown = pThreadpoolSt->dont_accept = 0;
    for (int i = 0; i < num_threads_in_pool; ++i) {
        if (pthread_create(&pThreadpoolSt->threads[i], nullptr, do_work, pThreadpoolSt) == 0) {
            perror("create thread");
            //free
            return nullptr;
        }
    }
}

void* do_work(void* p) {
    threadpool* thread_pool = (threadpool*) p;
}


void destroy_threadpool(threadpool* destroyme) {

}
