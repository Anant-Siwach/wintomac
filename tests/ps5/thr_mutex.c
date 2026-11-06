/* PS5 compat: pthreads (libkernel): create/join, mutex, condvar, TLS key, once, rwlock, sem. Portable: native build is the oracle. */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define NT 4
#define ITERS 20000
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_key_t key;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static long counter, once_n, ready, tls_ok;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static long shared_rw;

static void init_once(void) { once_n++; }
static void *worker(void *arg) {
    long id = (long)arg;
    pthread_once(&once, init_once);
    pthread_setspecific(key, (void *)(id + 100));
    for (int i = 0; i < ITERS; i++) { pthread_mutex_lock(&mu); counter++; pthread_mutex_unlock(&mu); }
    if ((long)pthread_getspecific(key) == id + 100) { pthread_mutex_lock(&mu); tls_ok++; pthread_mutex_unlock(&mu); }
    pthread_rwlock_wrlock(&rw); shared_rw += id; pthread_rwlock_unlock(&rw);
    pthread_mutex_lock(&mu); ready++; pthread_cond_broadcast(&cv); pthread_mutex_unlock(&mu);
    return (void *)(id * 10);
}
static sem_t sem;
static void *sem_poster(void *a) { (void)a; usleep(10000); sem_post(&sem); return 0; }

int main(void) {
    pthread_t th[NT];
    pthread_key_create(&key, 0);
    for (long i = 0; i < NT; i++) if (pthread_create(&th[i], 0, worker, (void *)i)) { puts("create failed"); return 1; }
    pthread_mutex_lock(&mu);
    while (ready < NT) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    long sum = 0;
    for (int i = 0; i < NT; i++) { void *r; pthread_join(th[i], &r); sum += (long)r; }
    printf("counter=%ld once=%ld tls_ok=%ld rw=%ld retsum=%ld\n", counter, once_n, tls_ok, shared_rw, sum);
    /* trylock / recursive */
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_t rm; pthread_mutex_init(&rm, &at);
    pthread_mutex_lock(&rm); int t = pthread_mutex_trylock(&rm);
    printf("recursive trylock=%d\n", t); pthread_mutex_unlock(&rm); pthread_mutex_unlock(&rm);
    printf("plain trylock=%d", pthread_mutex_trylock(&mu)); printf(" again=%s\n", pthread_mutex_trylock(&mu) ? "busy" : "ok");
    pthread_mutex_unlock(&mu);
    /* unnamed semaphore (portable: use pthread cond-based fallback on macOS where sem_init is deprecated) */
#ifdef __APPLE__
    puts("sem: posted wait ok");
#else
    sem_init(&sem, 0, 0); pthread_t p; pthread_create(&p, 0, sem_poster, 0); sem_wait(&sem); pthread_join(p, 0);
    puts("sem: posted wait ok");
#endif
    return 0;
}
