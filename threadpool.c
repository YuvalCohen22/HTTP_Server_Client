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
        free(pThreadpoolSt);
        return nullptr;
    }
    pThreadpoolSt->qhead = pThreadpoolSt->qtail = nullptr;
    if (pthread_mutex_init(&pThreadpoolSt->qlock, nullptr) != 0) {
        perror("init mutex");
        free(pThreadpoolSt->threads);
        free(pThreadpoolSt);
        return nullptr;
    }
    if (pthread_cond_init(&pThreadpoolSt->q_not_empty, nullptr) != 0) {
        perror("init cond");
        free(pThreadpoolSt->threads);
        pthread_mutex_destroy(&pThreadpoolSt->qlock);
        free(pThreadpoolSt);
        return nullptr;
    }
    if (pthread_cond_init(&pThreadpoolSt->q_not_full, nullptr) != 0) {
        perror("init cond");
        free(pThreadpoolSt->threads);
        pthread_mutex_destroy(&pThreadpoolSt->qlock);
        pthread_cond_destroy(&pThreadpoolSt->q_not_empty);
        free(pThreadpoolSt);
        return nullptr;
    }
    pThreadpoolSt->shutdown = pThreadpoolSt->dont_accept = 0;
    for (int i = 0; i < num_threads_in_pool; ++i) {
        if (pthread_create(&pThreadpoolSt->threads[i], nullptr, do_work, pThreadpoolSt) != 0) {
            perror("create thread");
            pthread_mutex_destroy(&pThreadpoolSt->qlock);
            pthread_cond_destroy(&pThreadpoolSt->q_not_empty);
            pthread_cond_destroy(&pThreadpoolSt->q_not_full);
            for (int j = 0; i < j; j++) {
                pthread_join(pThreadpoolSt->threads[i], nullptr);
            }
            free(pThreadpoolSt->threads);
            free(pThreadpoolSt);
            return nullptr;
        }
    }
    return pThreadpoolSt;
}

void dispatch(threadpool* from_me, dispatch_fn dispatch_to_here, void *arg) {
    work_t *work = (work_t *) malloc(sizeof(work_t));
    if (work == NULL) {
        perror("malloc");
        return;
    }
    work->routine = dispatch_to_here;
    work->arg = arg;
    pthread_mutex_lock(&from_me->qlock);
    if (from_me->dont_accept) {
        free(work);
        return;
    }
    while (from_me->qsize == from_me->max_qsize) {
        pthread_cond_wait(&from_me->q_not_full, &from_me->qlock);
    }
    if (from_me->dont_accept) {
        free(work);
        return;
    }
    if (from_me->qsize == 0) {
        from_me->qhead = from_me->qtail = work;
    }
    else {
        from_me->qtail->next = work;
        from_me->qtail = from_me->qtail->next;
    }
    from_me->qsize++;
    pthread_cond_signal(&from_me->q_not_empty);
    pthread_mutex_unlock(&from_me->qlock);
}


void* do_work(void* p) {
    threadpool* thread_pool = (threadpool*) p;
    while (1) {
        pthread_mutex_lock(&thread_pool->qlock);
        while (thread_pool->qsize == 0 && !thread_pool->shutdown) {
            pthread_cond_wait(&thread_pool->q_not_empty, &thread_pool->qlock);
        }
        if (thread_pool->shutdown) {
            pthread_mutex_unlock(&thread_pool->qlock);
            break;
        }
        else {
            pthread_cond_signal(&thread_pool->q_not_full);
        }
        if (thread_pool->qsize == thread_pool->max_qsize) {
            pthread_cond_signal(&thread_pool->q_not_empty);
        }
        work_t* work = thread_pool->qhead;
        if (thread_pool->qsize) {
            thread_pool->qtail = nullptr;
        }
        thread_pool->qhead = thread_pool->qhead->next;
        thread_pool->qsize--;
        if (thread_pool->qsize == 0 && thread_pool->dont_accept) {
            pthread_cond_signal(&thread_pool->q_empty);
        }
        pthread_mutex_unlock(&thread_pool->qlock);
        work->routine(work->arg);
        free(work);
    }
    pthread_exit(NULL);
}


void destroy_threadpool(threadpool* destroyme) {
    pthread_mutex_lock(&destroyme->qlock);
    destroyme->dont_accept = 1;
    while (destroyme->qsize > 0) {
        pthread_cond_wait(&destroyme->q_empty, &destroyme->qlock);
    }
    destroyme->shutdown = 1;
    pthread_cond_broadcast(&destroyme->q_not_empty);
    for (int i = 0; i < destroyme->num_threads; ++i) {
        pthread_join(destroyme->threads[i], nullptr);
    }
    pthread_cond_destroy(&destroyme->q_not_empty);
    pthread_cond_destroy(&destroyme->q_not_full);
    pthread_cond_destroy(&destroyme->q_empty);
    pthread_mutex_destroy(&destroyme->qlock);
    free(destroyme->threads);
    free(destroyme);

}
