#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static long counter, ready;

static void *work(void *arg)
{
    long id = (long)arg;
    for (int i = 0; i < 20000; i++) { pthread_mutex_lock(&m); counter++; pthread_mutex_unlock(&m); }
    pthread_mutex_lock(&m); ready++; pthread_cond_signal(&cv); pthread_mutex_unlock(&m);
    return (void *)(id * 10);
}

int main(void)
{
    pthread_t t[4]; void *ret[4];
    for (long i = 0; i < 4; i++) pthread_create(&t[i], 0, work, (void *)i);
    pthread_mutex_lock(&m); while (ready < 4) pthread_cond_wait(&cv, &m); pthread_mutex_unlock(&m);
    for (int i = 0; i < 4; i++) pthread_join(t[i], &ret[i]);
    printf("4 threads, counter=%ld (expected 80000), returns: %ld %ld %ld %ld\n", counter, (long)ret[0], (long)ret[1], (long)ret[2], (long)ret[3]);
    return counter == 80000 ? 0 : 1;
}
