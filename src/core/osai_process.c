#include "osai_process.h"
#include "osai_platform.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
extern char **environ;
#endif

struct osa_process {
#ifdef _WIN32
    HANDLE child, rd, wr, thread;
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE cv;
#else
    pid_t pid; int rd, wr;
#endif
    char *line; size_t len, cap; int done, failed, eof;
};
static int reserve(osa_process *p,size_t n) {
    if(n<=p->cap) return 1;
    size_t c=p->cap? p->cap:4096; while(c<n)c*=2;
    char *s=realloc(p->line,c); if(!s)return 0; p->line=s;p->cap=c;return 1;
}
#ifdef _WIN32
static DWORD WINAPI reader(LPVOID v) {
    osa_process *p=v; char b[4096]; DWORD n;
    for(;;) {
        BOOL ok=ReadFile(p->rd,b,sizeof(b),&n,NULL);
        EnterCriticalSection(&p->lock);
        if(!ok||!n){p->done=1;p->eof=1;p->failed=!ok;}
        else {
            size_t old=p->len;
            if(old+n>1048576 || !reserve(p,old+n+1)){p->failed=1;p->done=1;}
            else {memcpy(p->line+old,b,n);p->len=old+n;p->line[p->len]=0;
                if(memchr(b,'\n',n))p->done=1;}
        }
        WakeAllConditionVariable(&p->cv); LeaveCriticalSection(&p->lock);
        if(!ok||!n||p->failed)break;
        /* One request at a time. Stop reading after a line; remaining bytes
         * stay in the pipe until the parent consumes that response. */
        EnterCriticalSection(&p->lock); int stop=p->done; LeaveCriticalSection(&p->lock);
        if(stop)break;
    }
    return 0;
}
static wchar_t *wide(const char *s){int n=MultiByteToWideChar(CP_UTF8,0,s,-1,NULL,0);wchar_t*w=calloc(n,sizeof(*w));if(w)MultiByteToWideChar(CP_UTF8,0,s,-1,w,n);return w;}
/* Both ends start inheritable; the caller then marks exactly the two handles
 * the child owns (its stdin read end and its stdout write end) inheritable and
 * hides the parent's ends. Clearing inheritance on "the read end" for both
 * pipes handed the child an invalid stdin: it saw EOF immediately and exited
 * right after its ready line, so every request after activation failed. */
static void pipes(HANDLE *r,HANDLE *w,SECURITY_ATTRIBUTES *sa){if(!CreatePipe(r,w,sa,0)){*r=*w=NULL;return;}}
/* The GUI subsystem has no console, so GetStdHandle(STD_ERROR_HANDLE) is NULL
 * and passing it with STARTF_USESTDHANDLES made CreateProcessW fail outright on
 * some machines. The child always gets a real, inheritable stderr: its own log
 * file under <exe>/data/Logs, which also makes a failed model load diagnosable. */
static HANDLE worker_stderr(void){
    if(!osa_paths_init())return NULL;
    char path[OSAI_MAX_PATH];snprintf(path,sizeof path,"%sworker.log",osa_path(OSAI_DIR_LOGS));
    wchar_t *w=wide(path);if(!w)return NULL;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};
    HANDLE h=CreateFileW(w,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    free(w);
    if(h==INVALID_HANDLE_VALUE)return NULL;
    SetHandleInformation(h,HANDLE_FLAG_INHERIT,TRUE);
    SetFilePointer(h,0,NULL,FILE_END);
    return h;
}
osa_process *osa_process_start(const char *const *argv,osa_error *err){
    osa_process*p=calloc(1,sizeof(*p));if(!p){osa_err_set(err,"out of memory");return NULL;}
    InitializeCriticalSection(&p->lock);InitializeConditionVariable(&p->cv);p->rd=p->wr=NULL;
    SECURITY_ATTRIBUTES sa={sizeof(sa),NULL,TRUE};HANDLE cr=NULL,cw=NULL,sr=NULL,sw=NULL;
    pipes(&cr,&cw,&sa);
    if(cr)SetHandleInformation(cr,HANDLE_FLAG_INHERIT,TRUE);   /* child stdin  */
    if(cw)SetHandleInformation(cw,HANDLE_FLAG_INHERIT,FALSE);  /* parent only  */
    pipes(&sr,&sw,&sa);
    if(sr)SetHandleInformation(sr,HANDLE_FLAG_INHERIT,FALSE);  /* parent only  */
    if(sw)SetHandleInformation(sw,HANDLE_FLAG_INHERIT,TRUE);   /* child stdout */
    HANDLE elog=worker_stderr();
    wchar_t cmd[32768];cmd[0]=0;size_t used=0;
    for(int i=0;argv[i];i++){wchar_t*w=wide(argv[i]);if(!w)goto fail;
        size_t n=wcslen(w); if(used+n*2+4>=32760){free(w);goto fail;}
        if(i)cmd[used++]=L' ';cmd[used++]=L'"';
        unsigned bs=0;for(size_t j=0;j<n;j++){wchar_t ch=w[j];if(ch==L'\\')bs++;else{if(ch==L'"'){for(unsigned k=0;k<bs*2+1;k++)cmd[used++]=L'\\';bs=0;}else bs=0;cmd[used++]=ch;}}
        for(unsigned k=0;k<bs*2;k++)cmd[used++]=L'\\';cmd[used++]=L'"';free(w);
    }
    cmd[used]=0;
    STARTUPINFOW si={0};si.cb=sizeof(si);si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=cr;si.hStdOutput=sw;si.hStdError=elog;
    /* An explicit application name removes any ambiguity in parsing the command
     * line, and the working directory is the worker's own folder so a DLL search
     * or a relative path can never depend on how the app was launched. */
    wchar_t *app=wide(argv[0]);wchar_t *dir=wide(osa_exe_dir());
    DWORD gle=0;
    PROCESS_INFORMATION pi={0};
    BOOL ok=app?CreateProcessW(app,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,dir,&si,&pi):FALSE;
    if(!ok)gle=GetLastError();
    free(app);free(dir);
    if(elog){CloseHandle(elog);elog=NULL;}
    if(cr){CloseHandle(cr);cr=NULL;}if(sw){CloseHandle(sw);sw=NULL;}
    if(!ok){if(cr)CloseHandle(cr);if(cw)CloseHandle(cw);if(sr)CloseHandle(sr);if(sw)CloseHandle(sw);
        osa_process_stop(p);
        osa_err_set(err,"cannot start the model worker (%s, Windows error %lu)",argv[0],(unsigned long)gle);
        return NULL;}
    CloseHandle(pi.hThread);p->child=pi.hProcess;p->rd=sr;p->wr=cw;p->thread=CreateThread(NULL,0,reader,p,0,NULL);
    if(!p->thread)goto fail;if(cw==NULL&&cr)CloseHandle(cw);if(sr==NULL&&sw)CloseHandle(sr);return p;
fail:
    if(elog)CloseHandle(elog);
    if(cr)CloseHandle(cr);if(cw)CloseHandle(cw);if(sr)CloseHandle(sr);if(sw)CloseHandle(sw);
    osa_process_stop(p);osa_err_set(err,"cannot prepare the model worker pipes (%s)",argv[0]);return NULL;
}
bool osa_process_write(osa_process*p,const char*s,osa_error*err){size_t n=strlen(s),off=0;while(off<n){DWORD k=0;if(!WriteFile(p->wr,s+off,(DWORD)(n-off),&k,NULL)||!k){osa_err_set(err,"model worker pipe closed");return false;}off+=k;}DWORD k; if(!WriteFile(p->wr,"\n",1,&k,NULL)){osa_err_set(err,"model worker pipe closed");return false;}return true;}
bool osa_process_line(osa_process*p,int timeout,volatile int*cancel,char**out,osa_error*err){
    *out=NULL;int waited=0;EnterCriticalSection(&p->lock);
    for(;;){char*nl=p->line?memchr(p->line,'\n',p->len):NULL;
        if(nl){size_t n=nl-p->line;char*s=malloc(n+1);if(!s)break;memcpy(s,p->line,n);s[n]=0;memmove(p->line,p->line+n+1,p->len-n-1);p->len-=n+1;p->line[p->len]=0;LeaveCriticalSection(&p->lock);*out=s;return true;}
        if(p->eof)break;
        if(p->done&&!p->len){p->thread? (CloseHandle(p->thread),p->thread=NULL):0;p->thread=CreateThread(NULL,0,reader,p,0,NULL);p->done=0;if(!p->thread)break;}
        if(p->failed)break;
        if(cancel&&osa_atomic_load(cancel))break;
        if(timeout>0&&waited>=timeout)break;
        SleepConditionVariableCS(&p->cv,&p->lock,20);waited+=20;
    }
    LeaveCriticalSection(&p->lock);osa_err_set(err,"model worker stopped, timed out or returned invalid output");return false;
}
bool osa_process_exited(osa_process*p,int*code){
    if(code)*code=-1;if(!p||!p->child)return false;
    if(WaitForSingleObject(p->child,0)!=WAIT_OBJECT_0)return false;
    DWORD c=0;GetExitCodeProcess(p->child,&c);if(code)*code=(int)c;return true;
}
void osa_process_stop(osa_process*p){if(!p)return;if(p->child){TerminateProcess(p->child,1);WaitForSingleObject(p->child,5000);CloseHandle(p->child);}if(p->wr)CloseHandle(p->wr);if(p->rd)CloseHandle(p->rd);if(p->thread){WaitForSingleObject(p->thread,5000);CloseHandle(p->thread);}DeleteCriticalSection(&p->lock);free(p->line);free(p);}
#else
osa_process *osa_process_start(const char *const *argv,osa_error *err){
    signal(SIGPIPE,SIG_IGN);
    int in[2]={-1,-1},out[2]={-1,-1};if(pipe(in)||pipe(out)){osa_err_set(err,"cannot create worker pipes");goto fail;}
    posix_spawn_file_actions_t a;posix_spawn_file_actions_init(&a);posix_spawn_file_actions_adddup2(&a,in[0],0);posix_spawn_file_actions_adddup2(&a,out[1],1);posix_spawn_file_actions_addclose(&a,in[1]);posix_spawn_file_actions_addclose(&a,out[0]);
    osa_process*p=calloc(1,sizeof(*p));p->pid=-1;p->rd=p->wr=-1;
    int rc=posix_spawnp(&p->pid,argv[0],&a,NULL,(char*const*)argv,environ);posix_spawn_file_actions_destroy(&a);
    close(in[0]);close(out[1]);in[0]=out[1]=-1;
    if(rc){free(p);goto fail;}p->wr=in[1];p->rd=out[0];in[1]=out[0]=-1;fcntl(p->wr,F_SETFD,FD_CLOEXEC);fcntl(p->rd,F_SETFD,FD_CLOEXEC);return p;
fail:for(int i=0;i<2;i++){if(in[i]>=0)close(in[i]);if(out[i]>=0)close(out[i]);}return NULL;
}
bool osa_process_write(osa_process*p,const char*s,osa_error*err){size_t n=strlen(s),off=0;while(off<n){ssize_t k=write(p->wr,s+off,n-off);if(k<=0){osa_err_set(err,"model worker pipe closed");return false;}off+=k;}return write(p->wr,"\n",1)==1;}
bool osa_process_line(osa_process*p,int timeout,volatile int*cancel,char**out,osa_error*err){
    *out=NULL;int waited=0;for(;;){char*nl=p->line?memchr(p->line,'\n',p->len):NULL;if(nl){size_t n=nl-p->line;char*s=malloc(n+1);if(!s)break;memcpy(s,p->line,n);s[n]=0;memmove(p->line,p->line+n+1,p->len-n-1);p->len-=n+1;p->line[p->len]=0;*out=s;return true;}
        if(cancel&&osa_atomic_load(cancel))break;if(timeout>0&&waited>=timeout)break;
        struct pollfd fd={p->rd,POLLIN,0};int rc=poll(&fd,1,50);waited+=50;if(rc<0)break;if(rc==0)continue;
        char b[4096];ssize_t n=read(p->rd,b,sizeof(b));if(n<=0){p->done=1;break;}if(p->len+n>1048576||!reserve(p,p->len+n+1))break;memcpy(p->line+p->len,b,n);p->len+=n;p->line[p->len]=0;
    }osa_err_set(err,"model worker stopped, timed out or returned invalid output");return false;
}
bool osa_process_exited(osa_process*p,int*code){
    if(code)*code=-1;if(!p||p->pid<=0)return false;int st=0;
    if(waitpid(p->pid,&st,WNOHANG)!=p->pid)return false;
    if(code)*code=WIFEXITED(st)?WEXITSTATUS(st):(128+(WIFSIGNALED(st)?WTERMSIG(st):0));
    p->pid=-1;return true;
}
void osa_process_stop(osa_process*p){if(!p)return;if(p->wr>=0)close(p->wr);if(p->rd>=0)close(p->rd);if(p->pid>0){kill(p->pid,SIGTERM);for(int i=0;i<50;i++){if(waitpid(p->pid,NULL,WNOHANG)==p->pid)return;usleep(10000);}kill(p->pid,SIGKILL);waitpid(p->pid,NULL,0);}free(p->line);free(p);}
#endif
