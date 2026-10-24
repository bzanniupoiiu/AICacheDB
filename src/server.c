#include "daily.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <math.h>
#include <limits.h>

volatile sig_atomic_t stopping;
static uint64_t commands;
static int64_t started;
typedef struct { int fd, auth, eof; Buffer in, out; size_t sent; int64_t active; } Client;
static void stop_signal(int sig) { (void)sig; stopping=1; }
static int eq(Slice s,const char *text) { return s.n==strlen(text) && !strncasecmp((const char *)s.p,text,s.n); }
static void simple(Buffer *b,const char *s) { buf_printf(b,"+%s\r\n",s); }
static void integer(Buffer *b,int64_t n) { buf_printf(b,":%lld\r\n",(long long)n); }
static void wrong(Buffer *b) { reply_error(b,"ERR invalid arguments; see docs/commands.md"); }
static int password_ok(Slice a,const char *b) {
    size_t n=strlen(b); unsigned diff=(unsigned)(a.n^n);
    for(size_t i=0;i<n;i++) diff|=(unsigned)((i<a.n?a.p[i]:0)^(unsigned char)b[i]);
    return diff==0;
}
static void result(Buffer *out,int r) {
    if(r==-2) reply_error(out,"OOM maxmemory exceeded; delete entries or increase the limit");
    else if(r<0) reply_error(out,"MISCONF persistence failed; inspect server logs");
    else simple(out,"OK");
}
static int ttl_value(Slice s,int millis,int64_t *expires) {
    int64_t n; if(parse_int(s,&n) || n<=0 || n>(INT64_MAX-now_ms())/(millis?1:1000)) return -1;
    *expires=now_ms()+n*(millis?1:1000); return 0;
}
static int score_value(Slice s,double *v) {
    char text[64]; if(!s.n || s.n>=sizeof(text) || memchr(s.p,0,s.n)) return -1;
    memcpy(text,s.p,s.n); text[s.n]=0; char *end; errno=0; *v=strtod(text,&end);
    return errno || end==text || *end || !isfinite(*v) || *v < -1 || *v > 1 ? -1 : 0;
}
typedef struct { Entry *e; double score; } Match;
static int precedes(Match a,Match b) {
    if(a.score!=b.score) return a.score>b.score;
    size_t n=a.e->kn<b.e->kn?a.e->kn:b.e->kn; int cmp=memcmp(a.e->key,b.e->key,n);
    return cmp<0 || (!cmp && a.e->kn<b.e->kn);
}
static void vector_reply(Buffer *out,Entry *e,double score,int withscore) {
    buf_printf(out,"*%d\r\n",withscore?3:2); bulk(out,e->key,e->kn); bulk(out,e->value,e->vn);
    if(withscore) { char b[64]; int n=snprintf(b,sizeof(b),"%.9g",score); bulk(out,b,(size_t)n); }
}
static void search(Store *s,Buffer *out,int argc,Slice *a,int get) {
    int64_t dim,k=1; double threshold; float *q=NULL; int needed=get?4:5;
    if((argc!=needed && argc!=needed+2) || parse_int(a[1],&dim) || dim<1 || dim>MAX_DIM ||
       (!get && (parse_int(a[3],&k) || k<1 || k>MAX_TOPK)) || score_value(a[get?3:4],&threshold) ||
       parse_vector(a[2],(unsigned)dim,&q)) { wrong(out); return; }
    Slice prefix={NULL,0};
    if(argc==needed+2) { if(!eq(a[needed],"PREFIX")) { free(q); wrong(out); return; } prefix=a[needed+1]; }
    Match top[MAX_TOPK]; int count=0; int64_t now=now_ms();
    for(unsigned i=0;i<BUCKETS;i++) for(Entry *e=s->buckets[i];e;e=e->next) {
        if(e->dim!=(unsigned)dim || (e->expires>=0 && e->expires<=now) || e->kn<prefix.n ||
           (prefix.n && memcmp(e->key,prefix.p,prefix.n))) continue;
        double score=0; for(unsigned d=0;d<e->dim;d++) score+=(double)e->vector[d]*q[d];
        score=fmax(-1,fmin(1,score)); if(score<threshold) continue;
        Match m={e,score}; int pos=count;
        if(pos==k) { if(!precedes(m,top[pos-1])) continue; pos--; } else count++;
        while(pos>0 && precedes(m,top[pos-1])) { top[pos]=top[pos-1]; pos--; } top[pos]=m;
    }
    free(q);
    size_t response_size=32;
    for(int i=0;i<count;i++) response_size+=top[i].e->kn+top[i].e->vn+160;
    if(response_size>MAX_OUTPUT-out->n) {
        reply_error(out,"ERR response exceeds 4 MiB; reduce topK or answer size"); return;
    }
    if(get) { if(!count) buf_add(out,"*0\r\n",4); else vector_reply(out,top[0].e,top[0].score,1); }
    else { buf_printf(out,"*%d\r\n",count); for(int i=0;i<count;i++) vector_reply(out,top[i].e,top[i].score,1); }
}
static void execute(Store *s,const Config *cfg,Client *c,int argc,Slice *a) {
    Buffer *out=&c->out; commands++;
    if(eq(a[0],"AUTH")) {
        if(argc!=2) wrong(out);
        else if(!cfg->password[0]) reply_error(out,"ERR AUTH called without configured requirepass");
        else if(password_ok(a[1],cfg->password)) { c->auth=1; simple(out,"OK"); }
        else { c->auth=0; reply_error(out,"WRONGPASS invalid password"); }
        return;
    }
    if(!c->auth) { reply_error(out,"NOAUTH authentication required"); return; }
    if(eq(a[0],"PING")) { if(argc==1) simple(out,"PONG"); else if(argc==2) bulk(out,a[1].p,a[1].n); else wrong(out); return; }
    if(eq(a[0],"QUIT") && argc==1) { simple(out,"OK"); c->eof=2; return; }
    if(eq(a[0],"SELECT") && argc==2 && eq(a[1],"0")) { simple(out,"OK"); return; }
    if(eq(a[0],"CLIENT") && argc>=2 && (eq(a[1],"SETINFO") || eq(a[1],"SETNAME"))) { simple(out,"OK"); return; }
    if(eq(a[0],"INFO") && (argc==1 || argc==2)) {
        Buffer b={0};
        buf_printf(&b,"# Server\r\naicachedb_version:%s\r\nuptime_in_seconds:%lld\r\n# Memory\r\nused_memory:%zu\r\nmaxmemory:%zu\r\n# Stats\r\ntotal_commands_processed:%llu\r\nkeys:%zu\r\n# Persistence\r\npersistence:%s\r\npersistence_ok:%d\r\naof_bytes:%llu\r\nvector_search:exact_cosine\r\n",AIC_VERSION,(long long)((now_ms()-started)/1000),s->bytes,s->limit,(unsigned long long)commands,s->count,s->persistent?"always":"off",!s->failed,(unsigned long long)s->wal_bytes);
        bulk(out,b.p,b.n); buf_free(&b); return;
    }
    if(eq(a[0],"DBSIZE") && argc==1) { store_sweep(s); integer(out,(int64_t)s->count); return; }
    if(eq(a[0],"BACKUP") && argc==1) {
        if(!s->persistent) reply_error(out,"ERR persistence is disabled");
        else if(store_backup(s)) reply_error(out,"ERR backup failed; inspect available disk space");
        else simple(out,"OK");
        return;
    }
    if((eq(a[0],"SAVE") || eq(a[0],"BGSAVE")) && argc==1) {
        if(!s->persistent) reply_error(out,"ERR persistence is disabled");
        else result(out,store_compact(s));
        return;
    }
    if(eq(a[0],"SCAN")) {
        int64_t cursor,count=50;
        if((argc!=2 && argc!=4) || parse_int(a[1],&cursor) || cursor<0 ||
           (argc==4 && (!eq(a[2],"COUNT") || parse_int(a[3],&count) || count<1 || count>100))) { wrong(out); return; }
        store_sweep(s); size_t seen=0; int got=0; Buffer keys={0};
        for(unsigned i=0;i<BUCKETS;i++) for(Entry *e=s->buckets[i];e;e=e->next) {
            if(seen++<(uint64_t)cursor) continue;
            if(got<count) { bulk(&keys,e->key,e->kn); got++; }
        }
        char next[32]; int n=snprintf(next,sizeof(next),"%llu",(unsigned long long)((uint64_t)cursor+(unsigned)got<seen?(uint64_t)cursor+(unsigned)got:0));
        buf_add(out,"*2\r\n",4); bulk(out,next,(size_t)n); buf_printf(out,"*%d\r\n",got); buf_add(out,keys.p,keys.n); buf_free(&keys); return;
    }
    if(eq(a[0],"VSEARCH") || (eq(a[0],"VGET") && argc!=2)) { search(s,out,argc,a,eq(a[0],"VGET")); return; }
    if(argc<2 || a[1].n>4096) { wrong(out); return; }
    if(eq(a[0],"GET") || eq(a[0],"VGET") || eq(a[0],"TYPE")) {
        if(argc!=2) { wrong(out); return; }
        Entry *e=store_get(s,a[1]);
        if(eq(a[0],"TYPE")) simple(out,!e?"none":e->dim?"vector":"string");
        else if(eq(a[0],"VGET")) { if(e && !e->dim) reply_error(out,"WRONGTYPE expected vector"); else if(e) vector_reply(out,e,1,0); else buf_add(out,"*0\r\n",4); }
        else if(!e) buf_add(out,"$-1\r\n",5);
        else if(e->dim) reply_error(out,"WRONGTYPE expected string");
        else bulk(out,e->value,e->vn);
        return;
    }
    if(eq(a[0],"EXISTS") || eq(a[0],"EXIST")) {
        if(argc!=2) wrong(out); else integer(out,store_get(s,a[1])?1:0); return;
    }
    if(eq(a[0],"TTL") || eq(a[0],"PTTL")) {
        if(argc!=2) { wrong(out); return; }
        Entry *e=store_get(s,a[1]); int64_t t=!e?-2:e->expires<0?-1:e->expires-now_ms();
        if(e && e->expires>=0 && t<0) t=0;
        integer(out,t<0?t:eq(a[0],"TTL")?t/1000:t); return;
    }
    if(eq(a[0],"DEL") || eq(a[0],"VDEL")) {
        if(argc!=2) { wrong(out); return; }
        int r=store_del(s,a[1]); if(r<0) result(out,r); else integer(out,r); return;
    }
    if(eq(a[0],"EXPIRE") || eq(a[0],"PEXPIRE") || eq(a[0],"PERSIST")) {
        int persist=eq(a[0],"PERSIST"); int64_t t=-1,n=0;
        if(argc!=(persist?2:3) || (!persist && parse_int(a[2],&n))) { wrong(out); return; }
        Entry *e=store_get(s,a[1]); if(!e || (persist && e->expires<0)) { integer(out,0); return; }
        if(!persist && n<=0) { int r=store_del(s,a[1]); if(r<0) result(out,r); else integer(out,r); return; }
        if(!persist && ttl_value(a[2],eq(a[0],"PEXPIRE"),&t)) { wrong(out); return; }
        int r=store_put(s,a[1],(Slice){e->value,e->vn},t,e->dim,e->vector);
        if(r<0) result(out,r); else integer(out,1); return;
    }
    if(eq(a[0],"SET") || eq(a[0],"SETEX") || eq(a[0],"PSETEX")) {
        int64_t t=-1; Slice value; int nx=0;
        if(eq(a[0],"SET")) {
            if(argc<3 || argc>6) { wrong(out); return; }
            value=a[2]; int i=3;
            if(i<argc && eq(a[i],"NX")) { nx=1; i++; }
            if(i<argc) { int ms=eq(a[i],"PX"); if((!ms && !eq(a[i],"EX")) || i+1>=argc || ttl_value(a[i+1],ms,&t)) { wrong(out); return; } i+=2; }
            if(i<argc && eq(a[i],"NX") && !nx) { nx=1; i++; }
            if(i!=argc) { wrong(out); return; }
        } else {
            if(argc!=4 || ttl_value(a[2],eq(a[0],"PSETEX"),&t)) { wrong(out); return; } value=a[3];
        }
        if(value.n>512*1024) { reply_error(out,"ERR value exceeds 512 KiB"); return; }
        if(nx && store_get(s,a[1])) { buf_add(out,"$-1\r\n",5); return; }
        result(out,store_put(s,a[1],value,t,0,NULL)); return;
    }
    if(eq(a[0],"VSET")) {
        int64_t dim,t=-1; float *vec=NULL;
        if((argc!=5 && argc!=7) || parse_int(a[3],&dim) || dim<1 || dim>MAX_DIM || a[2].n>512*1024 ||
           (argc==7 && (!eq(a[5],"EX") || ttl_value(a[6],0,&t))) || parse_vector(a[4],(unsigned)dim,&vec)) { wrong(out); return; }
        int r=store_put(s,a[1],a[2],t,(unsigned)dim,vec); free(vec); result(out,r); return;
    }
    reply_error(out,"ERR unknown command; see docs/commands.md");
}

static void close_client(Client *c) {
    if(c->fd>=0) close(c->fd);
    buf_free(&c->in); buf_free(&c->out); c->fd=-1;
}
static int nonblock(int fd) { int f=fcntl(fd,F_GETFL,0); return f<0?-1:fcntl(fd,F_SETFL,f|O_NONBLOCK); }
int run_server(Store *s,const Config *cfg) {
    int listener=socket(AF_INET,SOCK_STREAM,0); if(listener<0) return -1;
    int one=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    struct sockaddr_in addr={0}; addr.sin_family=AF_INET; addr.sin_port=htons((uint16_t)cfg->port);
    if(inet_pton(AF_INET,cfg->bind,&addr.sin_addr)!=1 || bind(listener,(struct sockaddr *)&addr,sizeof(addr)) || listen(listener,128) || nonblock(listener)) { perror("listen"); close(listener); return -1; }
    Client *clients=calloc(cfg->maxclients,sizeof(*clients)); struct pollfd *fds=calloc(cfg->maxclients+1,sizeof(*fds));
    if(!clients || !fds) { free(clients); free(fds); close(listener); return -1; }
    for(unsigned i=0;i<cfg->maxclients;i++) clients[i].fd=-1;
    started=now_ms(); int64_t swept=started; int status=0;
    printf("AICacheDB %s ready at %s:%u | %zu MiB data limit | persistence=%s\n",AIC_VERSION,cfg->bind,cfg->port,cfg->maxmemory/(1024*1024),cfg->persistent?"always":"off"); fflush(stdout);
    while(!stopping) {
        fds[0]=(struct pollfd){listener,POLLIN,0}; int buffered=0;
        for(unsigned i=0;i<cfg->maxclients;i++) {
            Client *c=&clients[i]; short events=0;
            if(c->fd>=0) {
                if(c->out.n>c->sent) events|=POLLOUT;
                if(!c->eof && c->out.n-c->sent<256*1024) events|=POLLIN;
                if(c->in.n && c->out.n-c->sent<256*1024) {
                    Slice a[MAX_ARGS]; int ac; size_t used;
                    if(parse_resp(c->in.p,c->in.n,a,&ac,&used)!=0) {
                        buffered=1;
                        events=(short)(events & ~POLLIN);
                    }
                }
            }
            fds[i+1]=(struct pollfd){c->fd,events,0};
        }
        int ready=poll(fds,cfg->maxclients+1,buffered?0:200);
        if(ready<0) { if(errno==EINTR) continue; status=-1; break; }
        if(fds[0].revents&POLLIN) {
            int fd=accept(listener,NULL,NULL);
            if(fd>=0) {
                unsigned i=0; while(i<cfg->maxclients && clients[i].fd>=0) i++;
                if(i==cfg->maxclients || nonblock(fd)) { const char *e="-ERR maxclients reached\r\n"; send(fd,e,strlen(e),MSG_NOSIGNAL); close(fd); }
                else clients[i]=(Client){.fd=fd,.auth=!cfg->password[0],.active=now_ms()};
            }
        }
        for(unsigned i=0;i<cfg->maxclients;i++) {
            Client *c=&clients[i]; if(c->fd<0 || fds[i+1].fd!=c->fd) continue; short re=fds[i+1].revents;
            if(re&(POLLERR|POLLNVAL)) { close_client(c); continue; }
            if((re&POLLIN) && !c->eof) {
                unsigned char chunk[8192]; size_t room=MAX_REQUEST-c->in.n;
                if(!room) { reply_error(&c->out,"ERR request exceeds 1 MiB"); c->eof=1; }
                else {
                    ssize_t n=recv(c->fd,chunk,room<sizeof(chunk)?room:sizeof(chunk),0);
                    if(n>0) { buf_add(&c->in,chunk,(size_t)n); c->active=now_ms(); }
                    else if(!n) c->eof=1;
                    else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) { close_client(c); continue; }
                }
            }
            for(int budget=0;c->in.n && c->out.n-c->sent<256*1024 && budget<64;budget++) {
                Slice a[MAX_ARGS]; int ac; size_t used; int r=parse_resp(c->in.p,c->in.n,a,&ac,&used);
                if(!r) break;
                if(r<0) { reply_error(&c->out,"ERR malformed or oversized RESP request"); c->eof=1; c->in.n=0; break; }
                execute(s,cfg,c,ac,a); memmove(c->in.p,c->in.p+used,c->in.n-used); c->in.n-=used;
                if(c->eof==2) { c->in.n=0; break; }
            }
            if(c->in.error || c->out.error) { close_client(c); continue; }
            if(c->out.n>c->sent) {
                ssize_t n=send(c->fd,c->out.p+c->sent,c->out.n-c->sent,MSG_NOSIGNAL);
                if(n>0) { c->sent+=(size_t)n; c->active=now_ms(); }
                else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) { close_client(c); continue; }
                if(c->sent==c->out.n) { buf_free(&c->out); c->sent=0; }
                else if(c->sent>65536) { memmove(c->out.p,c->out.p+c->sent,c->out.n-c->sent); c->out.n-=c->sent; c->sent=0; }
            }
            if(c->eof && c->in.n) {
                Slice a[MAX_ARGS]; int ac; size_t used;
                if(parse_resp(c->in.p,c->in.n,a,&ac,&used)==0) c->in.n=0;
            }
            if((c->eof && !c->out.n && !c->in.n) || now_ms()-c->active>(int64_t)cfg->idle*1000) close_client(c);
        }
        if(now_ms()-swept>=1000) { store_sweep(s); swept=now_ms(); }
    }
    for(unsigned i=0;i<cfg->maxclients;i++) close_client(&clients[i]);
    free(clients); free(fds); close(listener); return status;
}
static int config_load(Config *cfg,const char *path) {
    FILE *fp=fopen(path,"r"); if(!fp) { perror(path); return -1; }
    char line[4608]; unsigned lineno=0;
    while(fgets(line,sizeof(line),fp)) {
        lineno++; if(!strchr(line,'\n') && !feof(fp)) goto bad;
        char *p=line; while(*p==' ' || *p=='\t') p++; if(!*p || *p=='#' || *p=='\n' || *p=='\r') continue;
        char *end=p+strlen(p); while(end>p && (end[-1]=='\n' || end[-1]=='\r' || end[-1]==' ' || end[-1]=='\t')) *--end=0;
        char *v=strpbrk(p," \t"); if(!v) goto bad; *v++=0; while(*v==' ' || *v=='\t') v++;
        if(!*v) goto bad;
        int64_t n;
        if(!strcmp(p,"bind")) { if(strlen(v)>=sizeof(cfg->bind)) goto bad; strcpy(cfg->bind,v); }
        else if(!strcmp(p,"dir")) { if(strlen(v)>=sizeof(cfg->dir)) goto bad; strcpy(cfg->dir,v); }
        else if(!strcmp(p,"requirepass")) { if(strlen(v)>=sizeof(cfg->password)) goto bad; strcpy(cfg->password,v); }
        else if(!strcmp(p,"appendonly")) { if(!strcmp(v,"yes")) cfg->persistent=1; else if(!strcmp(v,"no")) cfg->persistent=0; else goto bad; }
        else if(!strcmp(p,"appendfsync")) { if(strcmp(v,"always")) goto bad; }
        else if(!strcmp(p,"port") || !strcmp(p,"maxclients") || !strcmp(p,"maxmemory") || !strcmp(p,"timeout")) {
            if(parse_int((Slice){(unsigned char *)v,strlen(v)},&n) || n<1) goto bad;
            if(!strcmp(p,"port")) { if(n>65535) goto bad; cfg->port=(unsigned)n; }
            else if(!strcmp(p,"maxclients")) { if(n>1024) goto bad; cfg->maxclients=(unsigned)n; }
            else if(!strcmp(p,"timeout")) { if(n>86400) goto bad; cfg->idle=(unsigned)n; }
            else { if(n<1048576 || (uint64_t)n>SIZE_MAX/2) goto bad; cfg->maxmemory=(size_t)n; }
        } else goto bad;
    }
    if(ferror(fp)) goto bad;
    fclose(fp); return 0;
bad: fprintf(stderr,"Invalid or unsupported configuration at %s:%u\n",path,lineno); fclose(fp); return -1;
}
int main(int argc,char **argv) {
    Config cfg={.bind="127.0.0.1",.dir="./data",.port=8888,.maxclients=128,.idle=300,.maxmemory=64*1024*1024,.persistent=1};
    const char *path=NULL;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-c") && i+1<argc) path=argv[++i];
        else if(!strcmp(argv[i],"--check-config")) cfg.check=1;
        else if(!strcmp(argv[i],"--version")) { puts("AICacheDB " AIC_VERSION); return 0; }
        else if(!strcmp(argv[i],"--help")) { puts("Usage: kvstore [-c config] [--check-config] [--version]\nDefault: 127.0.0.1:8888, 64 MiB data, ./data, durable writes."); return 0; }
        else { fprintf(stderr,"Unknown option: %s\n",argv[i]); return 2; }
    }
    if(path && config_load(&cfg,path)) return 2;
    const char *pass=getenv("AICACHEDB_PASSWORD");
    if(pass) { if(strlen(pass)>=sizeof(cfg.password)) { fputs("Password too long\n",stderr); return 2; } strcpy(cfg.password,pass); }
    struct in_addr addr;
    if(inet_pton(AF_INET,cfg.bind,&addr)!=1 || ((ntohl(addr.s_addr)>>24)!=127 && !cfg.password[0])) { fputs("Non-loopback bind requires requirepass or AICACHEDB_PASSWORD; bind must be an IPv4 address.\n",stderr); return 2; }
    if(cfg.check) { puts("Configuration valid"); return 0; }
    signal(SIGINT,stop_signal); signal(SIGTERM,stop_signal); signal(SIGPIPE,SIG_IGN);
    Store *s=calloc(1,sizeof(*s)); if(!s) return 1;
    if(store_open(s,&cfg)) { perror("Cannot open store"); store_close(s); free(s); return 1; }
    int ret=run_server(s,&cfg); store_close(s); free(s); return ret?1:0;
}
