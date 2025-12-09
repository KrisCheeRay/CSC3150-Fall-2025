#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

static const char *BRANCH_MID = "├─";
static const char *BRANCH_END = "└─";
static const char *VERTICAL   = "│ ";
static const char *INDENT     = "  ";

typedef struct {
    int *data;
    size_t n, cap;
} IntVec;

typedef struct {
    int pid;
    int ppid;
    char *name;     
    IntVec children;
    int alive;
} Proc;

typedef struct {
    Proc *arr;
    size_t n, cap;
} ProcTable;

static ProcTable T = {0};

static void die(const char *msg) { perror(msg); exit(EXIT_FAILURE); }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n); if (!p) die("realloc"); return p; }
static void *xmalloc(size_t n) { void *p = malloc(n); if (!p) die("malloc"); return p; }
static char *xstrdup(const char *s){ if(!s) return NULL; size_t L=strlen(s)+1; char *d=xmalloc(L); memcpy(d,s,L); return d; }

static void iv_push(IntVec *v, int x){
    if (v->n == v->cap){ v->cap = v->cap? v->cap*2 : 4; v->data = xrealloc(v->data, v->cap*sizeof(int)); }
    v->data[v->n++] = x;
}

static int is_numstr(const char *s){
    if (!s || !*s) return 0;
    for (const char *p=s; *p; ++p) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static Proc* get_or_add(int pid){
    for (size_t i=0;i<T.n;++i) if (T.arr[i].pid==pid) return &T.arr[i];
    if (T.n == T.cap){ T.cap = T.cap? T.cap*2 : 512; T.arr = xrealloc(T.arr, T.cap*sizeof(Proc)); }
    Proc z={0}; z.pid=pid; T.arr[T.n++] = z;
    return &T.arr[T.n-1];
}

static int read_ppid_and_name_from_stat(int pid, int *ppid_out, char **name_out){
    char path[64]; snprintf(path,sizeof(path),"/proc/%d/stat",pid);
    FILE *f = fopen(path,"r"); if(!f) return -1;
    int pid_in=0, ppid=0; char state;
    if (fscanf(f,"%d",&pid_in)!=1){ fclose(f); return -1; }
    int ch; while((ch=fgetc(f))!=EOF && ch!='(') {}
    if (ch==EOF){ fclose(f); return -1; }
    char *comm = NULL; size_t cap=0, len=0; int depth=1;
    while((ch=fgetc(f))!=EOF){
        if (ch=='(') depth++;
        if (ch==')' && --depth==0) break;
        if (len+2>cap){ cap = cap? cap*2:32; comm = xrealloc(comm,cap); }
        comm[len++] = (char)ch;
    }
    if (comm){ comm[len]='\0'; }
    if (fscanf(f," %c",&state)!=1){ free(comm); fclose(f); return -1; }
    if (fscanf(f," %d",&ppid)!=1){ free(comm); fclose(f); return -1; }
    fclose(f);
    if (ppid_out) *ppid_out = ppid;
    if (name_out && !*name_out) *name_out = comm; else free(comm);
    return 0;
}

static char* read_comm(int pid){
    char path[64]; snprintf(path,sizeof(path),"/proc/%d/comm",pid);
    FILE *f = fopen(path,"r"); if(!f) return NULL;
    char *line=NULL; size_t sz=0; ssize_t n = getline(&line,&sz,f);
    fclose(f);
    if (n>0){
        if (line[n-1]=='\n') line[n-1]='\0';
        return line;
    }
    free(line); return NULL;
}

static int cmp_by_name_then_pid(const void *a, const void *b){
    int pa = *(const int*)a, pb = *(const int*)b;
    Proc *A = get_or_add(pa), *B = get_or_add(pb);
    const char *na = A->name? A->name : "";
    const char *nb = B->name? B->name : "";
    int c = strcmp(na, nb);
    if (c!=0) return c;
    return (pa>pb) - (pa<pb);
}

static void load_proc(void){
    DIR *d = opendir("/proc"); if(!d) die("opendir /proc");
    struct dirent *e;
    while((e=readdir(d))){
        if (e->d_type!=DT_DIR) continue;
        if (!is_numstr(e->d_name)) continue;
        int pid = atoi(e->d_name);
        Proc *n = get_or_add(pid);
        n->alive = 1;

        if (!n->name) n->name = read_comm(pid);
        int ppid=0; (void)read_ppid_and_name_from_stat(pid, &ppid, &n->name);
        n->ppid = ppid;
    }
    closedir(d);

    for (size_t i=0;i<T.n;++i){
        Proc *n = &T.arr[i];
        if (!n->alive) continue;
        if (n->ppid>0){
            Proc *p = get_or_add(n->ppid);
            iv_push(&p->children, n->pid);
        }
    }
    for (size_t i=0;i<T.n;++i){
        Proc *n = &T.arr[i];
        if (n->children.n>1)
            qsort(n->children.data, n->children.n, sizeof(int), cmp_by_name_then_pid);
    }
}

static int count_threads_except_main(int pid) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return 0;

    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        if (!is_numstr(e->d_name)) continue;
        int tid = atoi(e->d_name);
        if (tid == pid) continue; 
        cnt++;
    }
    closedir(d);
    return cnt;
}

static void print_children(int pid, const char *prefix){
    Proc *p = get_or_add(pid);

    int tcnt = count_threads_except_main(pid);
    if (tcnt > 0) {
        int last_threads = (p->children.n == 0);
        printf("%s%s%d*[{%s}]\n",
               prefix,
               last_threads ? BRANCH_END : BRANCH_MID,
               tcnt,
               p->name ? p->name : "unknown");
    }

    for (size_t i = 0; i < p->children.n; ++i) {
        int child = p->children.data[i];
        Proc *c = get_or_add(child);
        int is_last = (i + 1 == p->children.n);

        printf("%s%s%s\n", prefix, is_last ? BRANCH_END : BRANCH_MID,
               c->name ? c->name : "unknown");

        char next[1024];
        snprintf(next, sizeof(next), "%s%s", prefix, is_last ? INDENT : VERTICAL);
        print_children(child, next);
    }
}

static void print_tree(void){
    Proc *init = get_or_add(1);
    if (init && init->alive){
        printf("%s\n", init->name ? init->name : "unknown");
        print_children(1, "");
    } else {
        fprintf(stderr, "PID 1 (init/systemd) not found\n");
    }
}

int main(void){
    load_proc();
    if (T.n==0){ fprintf(stderr, "No processes found under /proc\n"); return 1; }
    print_tree();
    return 0;
}
