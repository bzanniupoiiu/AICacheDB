#include "daily.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <math.h>
#include <limits.h>

int64_t now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void buf_add(Buffer *b, const void *p, size_t n) {
    if (b->error || n > MAX_OUTPUT || b->n > MAX_OUTPUT - n) { b->error = 1; return; }
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        void *q = realloc(b->p, cap);
        if (!q) { b->error = 1; return; }
        b->p = q; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n; b->p[b->n] = 0;
}
void buf_printf(Buffer *b, const char *fmt, ...) {
    char tmp[1024]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(tmp)) { b->error = 1; return; }
    buf_add(b, tmp, (size_t)n);
}
void bulk(Buffer *b, const void *p, size_t n) {
    buf_printf(b, "$%zu\r\n", n); buf_add(b, p, n); buf_add(b, "\r\n", 2);
}
void reply_error(Buffer *b, const char *s) { buf_printf(b, "-%s\r\n", s); }
void buf_free(Buffer *b) { free(b->p); memset(b, 0, sizeof(*b)); }

/* 1 = complete, 0 = incomplete, -1 = invalid. No unbounded integer arithmetic. */
static int number(const unsigned char *p, size_t n, size_t *pos, size_t max, size_t *v) {
    size_t i = *pos, x = 0, digits = 0;
    while (i < n && p[i] >= '0' && p[i] <= '9') {
        unsigned d = p[i++] - '0';
        if (x > max / 10 || (x == max / 10 && d > max % 10)) return -1;
        x = x * 10 + d; digits++;
    }
    if (i == n) return 0;
    if (!digits || p[i++] != '\r') return -1;
    if (i == n) return 0;
    if (p[i++] != '\n') return -1;
    *pos = i; *v = x; return 1;
}
int parse_resp(const unsigned char *p, size_t n, Slice *args, int *argc, size_t *used) {
    size_t pos = 1, count, len;
    if (!n) return 0;
    if (p[0] != '*') return -1;
    int r = number(p, n, &pos, MAX_ARGS, &count);
    if (r != 1) return r;
    if (!count) return -1;
    for (size_t i = 0; i < count; i++) {
        if (pos == n) return 0;
        if (p[pos++] != '$') return -1;
        r = number(p, n, &pos, MAX_REQUEST, &len);
        if (r != 1) return r;
        if (len > MAX_REQUEST - pos || pos + len > MAX_REQUEST - 2) return -1;
        if (n - pos < len + 2) return 0;
        if (p[pos + len] != '\r' || p[pos + len + 1] != '\n') return -1;
        args[i] = (Slice){p + pos, len}; pos += len + 2;
    }
    *argc = (int)count; *used = pos; return 1;
}
int parse_int(Slice s, int64_t *v) {
    if (!s.n || s.n > 20) return -1;
    uint64_t x = 0; size_t i = 0; int neg = s.p[0] == '-';
    if (neg && ++i == s.n) return -1;
    uint64_t max = neg ? (uint64_t)INT64_MAX + 1 : INT64_MAX;
    for (; i < s.n; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') return -1;
        unsigned d = s.p[i] - '0';
        if (x > max / 10 || (x == max / 10 && d > max % 10)) return -1;
        x = x * 10 + d;
    }
    *v = neg ? (x == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)x) : (int64_t)x;
    return 0;
}
int parse_vector(Slice s, unsigned dim, float **out) {
    if (!dim || dim > MAX_DIM || !s.n || s.n > MAX_REQUEST) return -1;
    char *text = malloc(s.n + 1); float *vec = malloc(dim * sizeof(float));
    if (!text || !vec) { free(text); free(vec); return -1; }
    memcpy(text, s.p, s.n); text[s.n] = 0;
    char *p = text; double norm = 0;
    for (unsigned i = 0; i < dim; i++) {
        char *end; errno = 0; double f = strtod(p, &end);
        if (errno || end == p || !isfinite(f) || fabs(f) > 1e30 ||
            (i + 1 < dim ? *end != ',' : end != text + s.n)) goto bad;
        vec[i] = (float)f; norm += (double)vec[i] * vec[i]; p = end + 1;
    }
    if (!isfinite(norm) || norm <= 0) goto bad;
    norm = sqrt(norm);
    for (unsigned i = 0; i < dim; i++) vec[i] = (float)(vec[i] / norm);
    free(text); *out = vec; return 0;
bad: free(text); free(vec); return -1;
}

static size_t hash(Store *s, Slice key) {
    uint64_t h = s->hash_seed;
    for (size_t i = 0; i < key.n; i++) { h ^= key.p[i]; h *= UINT64_C(1099511628211); }
    h ^= h >> 32; return (size_t)(h % BUCKETS);
}
static size_t cost(const Entry *e) { return sizeof(*e) + e->kn + e->vn + (size_t)e->dim * 4; }
static void destroy(Entry *e) { free(e->key); free(e->value); free(e->vector); free(e); }
static Entry **locate(Store *s, Slice key) {
    Entry **pp = &s->buckets[hash(s, key)];
    while (*pp && ((*pp)->kn != key.n || memcmp((*pp)->key, key.p, key.n))) pp = &(*pp)->next;
    return pp;
}
static void remove_at(Store *s, Entry **pp) {
    Entry *e = *pp; *pp = e->next; s->bytes -= cost(e); s->count--; destroy(e);
}
Entry *store_get(Store *s, Slice key) {
    Entry **pp = locate(s, key);
    if (*pp && (*pp)->expires >= 0 && (*pp)->expires <= now_ms()) remove_at(s, pp);
    return *pp;
}
void store_sweep(Store *s) {
    int64_t now = now_ms();
    for (unsigned i = 0; i < BUCKETS; i++) {
        Entry **pp = &s->buckets[i];
        while (*pp) {
            if ((*pp)->expires >= 0 && (*pp)->expires <= now) remove_at(s, pp);
            else pp = &(*pp)->next;
        }
    }
}
static void u32(unsigned char *p, uint32_t n) { for (int i=0;i<4;i++) p[i]=(unsigned char)(n>>(i*8)); }
static uint32_t r32(const unsigned char *p) { uint32_t n=0; for(int i=0;i<4;i++) n|=(uint32_t)p[i]<<(i*8); return n; }
static uint32_t crc(const unsigned char *p, size_t n) {
    uint32_t x = ~0U;
    for (size_t i=0;i<n;i++) { x ^= p[i]; for(int j=0;j<8;j++) x=(x>>1)^(0xedb88320U & (0U-(x&1))); }
    return ~x;
}
static int write_full(int fd, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n) { ssize_t w=write(fd,b,n); if(w<0 && errno==EINTR) continue; if(w<=0) return -1; b+=w; n-=(size_t)w; }
    return 0;
}
/* Versioned little-endian frame. CRC covers metadata AND payload. */
static int frame(int fd, Slice k, Slice v, int64_t expires, unsigned dim, const float *vec, int del, uint64_t *size) {
    size_t n = 24 + k.n + v.n + (size_t)dim * 4;
    unsigned char *p = malloc(n); unsigned char hdr[16];
    if (!p) return -1;
    memset(p,0,24); p[0]=(unsigned char)del;
    u32(p+4,(uint32_t)k.n); u32(p+8,(uint32_t)v.n); u32(p+12,dim);
    uint64_t t=(uint64_t)expires; for(int i=0;i<8;i++) p[16+i]=(unsigned char)(t>>(i*8));
    memcpy(p+24,k.p,k.n); if(v.n) memcpy(p+24+k.n,v.p,v.n);
    for(unsigned i=0;i<dim;i++) { uint32_t bits; memcpy(&bits,&vec[i],4); u32(p+24+k.n+v.n+i*4,bits); }
    memcpy(hdr,"AIC1",4); u32(hdr+4,(uint32_t)n); u32(hdr+8,crc(p,n)); u32(hdr+12,crc(hdr,12));
    int ret=write_full(fd,hdr,sizeof(hdr)) || write_full(fd,p,n);
    free(p); if(ret) return -1; if(size) *size+=n+16; return 0;
}
static int journal(Store *s, Slice k, Slice v, int64_t t, unsigned dim, const float *vec, int del) {
    if(s->failed) return -1;
    if(!s->persistent) return 0;
    /* Bound history growth without a second in-memory snapshot. */
    if(s->wal_bytes>64U*1024U*1024U && s->wal_bytes/4>s->bytes && store_compact(s)) {
        s->failed=1; return -1;
    }
    if(frame(s->fd,k,v,t,dim,vec,del,&s->wal_bytes) || fdatasync(s->fd)) {
        s->failed=1; perror("persistence failed; restart after repairing storage"); return -1;
    }
    return 0;
}
static Entry *allocate(Slice k, Slice v, int64_t t, unsigned dim, const float *vec) {
    Entry *e=calloc(1,sizeof(*e)); if(!e) return NULL;
    e->key=malloc(k.n ? k.n : 1); e->value=malloc(v.n ? v.n : 1);
    if(dim) e->vector=malloc((size_t)dim*4);
    if(!e->key || !e->value || (dim && !e->vector)) { destroy(e); return NULL; }
    memcpy(e->key,k.p,k.n); if(v.n) memcpy(e->value,v.p,v.n);
    if(dim) memcpy(e->vector,vec,(size_t)dim*4);
    e->kn=k.n; e->vn=v.n; e->expires=t; e->dim=dim; return e;
}
int store_put(Store *s, Slice k, Slice v, int64_t t, unsigned dim, const float *vec) {
    Entry **pp=locate(s,k);
    size_t old=*pp ? cost(*pp) : 0, next=sizeof(Entry)+k.n+v.n+(size_t)dim*4;
    if(next>s->limit || s->bytes-old>s->limit-next) return -2;
    Entry *e=allocate(k,v,t,dim,vec); if(!e) return -2;
    if(journal(s,(Slice){e->key,e->kn},(Slice){e->value,e->vn},t,dim,e->vector,0)) { destroy(e); return -1; }
    pp=locate(s,k);
    if(*pp) remove_at(s,pp);
    e->next=*pp; *pp=e; s->bytes+=cost(e); s->count++; s->writes++; return 0;
}
int store_del(Store *s, Slice k) {
    if(!store_get(s,k)) return 0;
    if(journal(s,k,(Slice){NULL,0},-1,0,NULL,1)) return -1;
    Entry **pp=locate(s,k);
    if(*pp) remove_at(s,pp);
    s->writes++; return 1;
}
int store_compact(Store *s) {
    if(!s->persistent || s->failed) return -1;
    store_sweep(s);
    int fd=openat(s->dirfd,"cache.aof.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    uint64_t bytes=0;
    for(unsigned i=0;i<BUCKETS;i++) for(Entry *e=s->buckets[i];e;e=e->next)
        if(frame(fd,(Slice){e->key,e->kn},(Slice){e->value,e->vn},e->expires,e->dim,e->vector,0,&bytes)) goto fail;
    if(fsync(fd)) goto fail;
    if(renameat(s->dirfd,"cache.aof.tmp",s->dirfd,"cache.aof")) goto fail;
    /* Keep using the renamed open descriptor even if directory sync fails. */
    close(s->fd); s->fd=fd; s->wal_bytes=bytes;
    if(fsync(s->dirfd)) { s->failed=1; return -1; }
    return 0;
fail: close(fd); unlinkat(s->dirfd,"cache.aof.tmp",0); return -1;
}
int store_backup(Store *s) {
    if(!s->persistent || s->failed) return -1;
    store_sweep(s);
    int fd=openat(s->dirfd,"backup.aof.tmp",O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
    if(fd<0) return -1;
    int ret=0;
    for(unsigned i=0;i<BUCKETS && !ret;i++) for(Entry *e=s->buckets[i];e;e=e->next) {
        if(frame(fd,(Slice){e->key,e->kn},(Slice){e->value,e->vn},e->expires,e->dim,e->vector,0,NULL)) { ret=-1; break; }
    }
    if(!ret && fsync(fd)) ret=-1;
    close(fd);
    if(!ret && renameat(s->dirfd,"backup.aof.tmp",s->dirfd,"backup.aof")) ret=-1;
    if(!ret && fsync(s->dirfd)) ret=-1;
    if(ret) unlinkat(s->dirfd,"backup.aof.tmp",0);
    return ret;
}
static int read_full(int fd, void *p, size_t n) {
    unsigned char *b=p; size_t got=0;
    while(got<n) { ssize_t r=read(fd,b+got,n-got); if(r<0 && errno==EINTR) continue; if(r<0) return -1; if(!r) return got ? -2 : 0; got+=(size_t)r; }
    return 1;
}
static int replay(Store *s) {
    off_t good=0; unsigned char hdr[16]; int r;
    while((r=read_full(s->fd,hdr,16))!=0) {
        if(r==-2) goto tail;
        if(r<0 || memcmp(hdr,"AIC1",4) || crc(hdr,12)!=r32(hdr+12)) goto bad;
        size_t n=r32(hdr+4);
        if(n<24 || n>MAX_REQUEST+MAX_DIM*4+24) goto bad;
        unsigned char *p=malloc(n); if(!p) return -1;
        r=read_full(s->fd,p,n);
        if(r==0 || r==-2) { free(p); goto tail; }
        if(r<0 || crc(p,n)!=r32(hdr+8)) { free(p); goto bad; }
        size_t kn=r32(p+4),vn=r32(p+8); unsigned dim=r32(p+12);
        uint64_t t=0; for(int i=0;i<8;i++) t|=(uint64_t)p[16+i]<<(i*8);
        if(p[0]>1 || p[1] || p[2] || p[3] || kn>4096 || dim>MAX_DIM || 24+kn+vn+(size_t)dim*4!=n || (p[0] && (vn||dim))) { free(p); goto bad; }
        int64_t expires; memcpy(&expires,&t,8);
        Slice key={p+24,kn}; Entry **pp=locate(s,key); if(*pp) remove_at(s,pp);
        if(!p[0] && (expires<0 || expires>now_ms())) {
            float vec[MAX_DIM];
            for(unsigned i=0;i<dim;i++) { uint32_t bits=r32(p+24+kn+vn+i*4); memcpy(&vec[i],&bits,4); if(!isfinite(vec[i])) { free(p); goto bad; } }
            Entry *e=allocate(key,(Slice){p+24+kn,vn},expires,dim,vec);
            if(!e) { free(p); return -1; }
            if(cost(e)>s->limit || s->bytes>s->limit-cost(e)) { destroy(e); free(p); fprintf(stderr,"Recovery exceeds maxmemory; increase it before restarting.\n"); return -1; }
            e->next=*pp; *pp=e; s->count++; s->bytes+=cost(e);
        }
        free(p); good+=(off_t)(16+n);
    }
    s->wal_bytes=(uint64_t)good; return 0;
tail:
    fprintf(stderr,"Repairing incomplete final journal frame at byte %lld\n",(long long)good);
    if(ftruncate(s->fd,good) || fsync(s->fd)) return -1;
    s->wal_bytes=(uint64_t)good; return 0;
bad: fprintf(stderr,"Journal corrupt at byte %lld; preserve the file and restore a verified backup.\n",(long long)good); return -1;
}
int store_open(Store *s, const Config *cfg) {
    memset(s,0,sizeof(*s)); s->fd=s->lockfd=s->dirfd=-1;
    s->limit=cfg->maxmemory; s->persistent=cfg->persistent;
    s->hash_seed=(uint64_t)now_ms()^(uint64_t)getpid();
    int rnd=open("/dev/urandom",O_RDONLY); if(rnd>=0) { if(read(rnd,&s->hash_seed,8)!=8) s->hash_seed=UINT64_C(1469598103934665603); close(rnd); }
    if(!cfg->persistent) return 0;
    int created=mkdir(cfg->dir,0700)==0;
    if(!created && errno!=EEXIST) { perror("data directory"); return -1; }
    if(created) {
        char parent[4096]; snprintf(parent,sizeof(parent),"%s",cfg->dir);
        size_t len=strlen(parent); while(len>1 && parent[len-1]=='/') parent[--len]=0;
        char *slash=strrchr(parent,'/');
        if(!slash) strcpy(parent,"."); else if(slash==parent) parent[1]=0; else *slash=0;
        int pfd=open(parent,O_RDONLY|O_DIRECTORY);
        if(pfd<0) return -1;
        int synced=fsync(pfd); close(pfd); if(synced) return -1;
    }
    s->dirfd=open(cfg->dir,O_RDONLY|O_DIRECTORY); if(s->dirfd<0) return -1;
    s->lockfd=openat(s->dirfd,"cache.lock",O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(s->lockfd<0 || flock(s->lockfd,LOCK_EX|LOCK_NB)) { fprintf(stderr,"Data directory is already in use or not writable.\n"); return -1; }
    s->fd=openat(s->dirfd,"cache.aof",O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if(s->fd<0 || replay(s) || lseek(s->fd,0,SEEK_END)<0 || fsync(s->fd) || fsync(s->dirfd)) return -1;
    return 0;
}
void store_close(Store *s) {
    for(unsigned i=0;i<BUCKETS;i++) while(s->buckets[i]) remove_at(s,&s->buckets[i]);
    if(s->fd>=0) close(s->fd);
    if(s->lockfd>=0) close(s->lockfd);
    if(s->dirfd>=0) close(s->dirfd);
}
