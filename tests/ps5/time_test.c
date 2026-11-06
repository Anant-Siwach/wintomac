/* PS5 compat: time (clock_gettime, gettimeofday, time, nanosleep/usleep, localtime/gmtime/strftime). */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>

static double now(clockid_t c) { struct timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(void) {
    double a = now(CLOCK_MONOTONIC); usleep(50000); double b = now(CLOCK_MONOTONIC);
    printf("monotonic advanced 50ms: %d (<500ms: %d)\n", b - a >= 0.049, b - a < 0.5);
    struct timespec ts = {0, 20 * 1000000}; a = now(CLOCK_MONOTONIC); nanosleep(&ts, 0); b = now(CLOCK_MONOTONIC);
    printf("nanosleep 20ms: %d\n", b - a >= 0.019);
    struct timeval tv; gettimeofday(&tv, 0); time_t t = time(0);
    printf("time sane (>2020): %d, gtod==time: %d\n", t > 1577836800, tv.tv_sec - t <= 1 && t - tv.tv_sec <= 1);
    printf("realtime sane: %d\n", now(CLOCK_REALTIME) > 1577836800.0);
    time_t fixed = 1700000000; struct tm *g = gmtime(&fixed); char s[64];
    strftime(s, sizeof s, "%Y-%m-%d %H:%M:%S", g); printf("gmtime=%s wday=%d yday=%d\n", s, g->tm_wday, g->tm_yday);
    struct tm tm2 = *g; tm2.tm_mday += 40; printf("timegm +40d=%lld\n", (long long)(timegm(&tm2)));
    printf("clock() sane: %d\n", clock() >= 0);
    return 0;
}
