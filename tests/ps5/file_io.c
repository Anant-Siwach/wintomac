/* PS5 compat: file I/O. PS5: /data (writable) and /app0 (title dir); native build uses a temp dir. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#ifdef __APPLE__
#define BASE "/tmp/wintomac_fileio"
#else
#define BASE "/data/wintomac_fileio"
#endif

int main(void) {
    mkdir(BASE, 0777);
    char p[256]; snprintf(p, sizeof p, BASE "/a.txt");
    FILE *f = fopen(p, "w"); if (!f) { printf("fopen failed errno=%d\n", errno); return 1; }
    for (int i = 0; i < 100; i++) fprintf(f, "line %d\n", i);
    fclose(f);
    f = fopen(p, "r"); char l[64]; int n = 0; long bytes = 0;
    while (fgets(l, sizeof l, f)) { n++; bytes += strlen(l); }
    printf("lines=%d bytes=%ld\n", n, bytes);
    fseek(f, 5, SEEK_SET); printf("tell=%ld c=%c\n", ftell(f), fgetc(f)); fclose(f);
    struct stat st; stat(p, &st); printf("size=%lld isreg=%d\n", (long long)st.st_size, S_ISREG(st.st_mode));
    /* POSIX fd I/O */
    int fd = open(BASE "/b.bin", O_CREAT | O_RDWR | O_TRUNC, 0666);
    unsigned char buf[256]; for (int i = 0; i < 256; i++) buf[i] = i;
    printf("write=%zd\n", write(fd, buf, 256));
    printf("lseek=%lld\n", (long long)lseek(fd, 100, SEEK_SET));
    unsigned char r[4]; read(fd, r, 4); printf("read %d %d %d %d\n", r[0], r[1], r[2], r[3]);
    close(fd);
    rename(BASE "/b.bin", BASE "/c.bin");
    printf("b exists=%d c exists=%d\n", access(BASE "/b.bin", F_OK) == 0, access(BASE "/c.bin", F_OK) == 0);
    /* directory listing (sorted) */
    DIR *d = opendir(BASE); char names[16][64]; int nn = 0; struct dirent *e;
    while ((e = readdir(d))) if (e->d_name[0] != '.') strcpy(names[nn++], e->d_name);
    closedir(d);
    for (int i = 0; i < nn; i++) for (int j = i + 1; j < nn; j++) if (strcmp(names[i], names[j]) > 0) { char t[64]; strcpy(t, names[i]); strcpy(names[i], names[j]); strcpy(names[j], t); }
    for (int i = 0; i < nn; i++) printf("entry %s\n", names[i]);
    FILE *x = fopen(BASE "/nonexistent", "r"); printf("missing -> %s errno=%d\n", x ? "opened" : "NULL", errno == ENOENT);
    unlink(BASE "/a.txt"); unlink(BASE "/c.bin"); rmdir(BASE);
    printf("cleanup dir exists=%d\n", access(BASE, F_OK) == 0);
    return 0;
}
