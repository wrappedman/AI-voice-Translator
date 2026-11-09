/* =====================================================================
 *  OpenSpeech AI — regression tests for the no-bundle / verified-model build
 *
 *  These tests do NOT ship or fabricate model weights. They assert the
 *  fresh-install contract, catalog integrity, atomic activation, Unicode
 *  handling, audio decoding, worker isolation and UI gating. Optional real
 *  inference smoke tests run only when OSAI_TEST_ASR / OSAI_TEST_MT /
 *  OSAI_TEST_LID / OSAI_TEST_WAV point at user-supplied official artifacts.
 * ===================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <unistd.h>

#include "osai_common.h"
#include "osai_platform.h"
#include "osai_process.h"
#include "live_endpoint.h"
#include "tts.h"
#include "tts_sapi.h"
#include "mic.h"
#include "osai_store.h"
#include "osai_app.h"
#include "audio.h"
#include "shell.h"

static int g_pass, g_fail;
static void suite(const char *name){ printf("\n%s\n",name); }
static void ok_(bool cond,const char *fmt,...){
    va_list ap; va_start(ap,fmt); char msg[512]; vsnprintf(msg,sizeof msg,fmt,ap); va_end(ap);
    printf("  %-4s %s\n",cond?"ok":"FAIL",msg); if(cond)g_pass++;else g_fail++;
}
#define CHECK(c) ok_((c),"%s",#c)
#define CHECK_MSG(c,...) ok_((c),__VA_ARGS__)
#define CHECK_STR(a,b) ok_(osa_streq((a),(b)),"%s == \"%s\" (got \"%s\")",#a,(b),(a))

static char g_root[256];
static void use_temp_root(const char *tag){
    snprintf(g_root,sizeof g_root,"/tmp/osai-%s-XXXXXX",tag);
    if(!mkdtemp(g_root)){perror("mkdtemp");exit(2);}
    setenv("OSAI_DATA_DIR",g_root,1);
    osa_paths_reset();
}
static void clean_root(void){ osa_dir_remove_recursive(g_root); }

static void test_json_unicode(void){
    suite("json unicode");
    const char *doc="{\"text\":\"\\u0633\\u0644\\u0627\\u0645 \\uD83D\\uDE00\",\"ok\":true}";
    char out[128]={0}; bool b=false;
    CHECK(osa_json_get_string(doc,"text",out,sizeof out));
    CHECK_STR(out,"سلام 😀");
    CHECK(osa_json_get_bool(doc,"ok",&b)&&b);
    osa_arena a; osa_arena_init(&a);
    char *esc=osa_json_escape(&a,"سلام \"x\"\n");
    char round[128]={0}; char *obj=NULL;
    /* wrap escaped string in an object and decode it back */
    obj=malloc(strlen(esc)+32); sprintf(obj,"{\"v\":%s}",esc);
    CHECK(osa_json_get_string(obj,"v",round,sizeof round));
    CHECK_STR(round,"سلام \"x\"\n");
    free(obj); osa_arena_free(&a);
}

static void test_catalog_and_gate(void){
    suite("fresh install / catalog / gating");
    use_temp_root("gate");
    osa_error err; osa_app *app=osa_app_create(&err);
    CHECK_MSG(app!=NULL,"app create: %s",app?"ok":err.msg);
    if(!app){clean_root();return;}
    osa_app_status st; osa_app_get_status(app,&st);
    CHECK(st.db_ready);
    CHECK_MSG(st.n_models==0,"fresh install has zero installed models (got %d)",st.n_models);
    CHECK(!st.asr_ready&&!st.mt_ready&&!st.lid_ready);
    CHECK(!osa_app_ready(app));
    osa_pack_info packs[32]; int np=osa_app_packs(app,packs,32);
    int asr=0,mt=0,lid=0,light_asr=0,light_mt=0,light_lid=0;
    bool all_verified=true,ram_stated=true;
    for(int i=0;i<np;i++){
        if(osa_streq(packs[i].kind,"asr")){asr++;if(packs[i].min_ram_mb<=1000)light_asr++;}
        if(osa_streq(packs[i].kind,"mt")){mt++;if(packs[i].min_ram_mb<=1200)light_mt++;}
        if(osa_streq(packs[i].kind,"lang")){lid++;if(packs[i].min_ram_mb<=1000)light_lid++;}
        if(!packs[i].url[0]||strlen(packs[i].sha256)!=64||packs[i].size_mb<=0)all_verified=false;
        if(!osa_starts(packs[i].url,"https://"))all_verified=false;
        if(packs[i].min_ram_mb<=0||packs[i].min_ram_mb>16384)ram_stated=false;
        CHECK_MSG(!packs[i].installed,"catalog %s is not installed on fresh state",packs[i].id);
    }
    CHECK_MSG(np>=6&&asr>=2&&mt>=2&&lid>=4,"catalog offers a choice in every category (asr=%d mt=%d lid=%d)",asr,mt,lid);
    CHECK_MSG(light_asr>=2&&light_mt>=1&&light_lid>=2,
              "each category has light models for low-memory PCs (asr=%d mt=%d lid=%d)",
              light_asr,light_mt,light_lid);
    CHECK(all_verified); CHECK_MSG(ram_stated,"every pack states its RAM requirement");
    osa_app_translation tr; CHECK(!osa_app_translate(app,"سلام","fa",&tr,&err));
    osa_app_transcript ts; CHECK(!osa_app_transcribe_file(app,"/nonexistent.wav",&ts,&err));
    CHECK(!osa_app_install_builtin_pack(app,"anything",&err));
    CHECK(!osa_app_set_languages(app,"xx","zz",&err));
    osa_app_destroy(app); clean_root();
}

static void test_store_active(void){
    suite("atomic single-active model store");
    use_temp_root("store");
    char dbpath[512]; snprintf(dbpath,sizeof dbpath,"%s/test.db",g_root);
    osa_error err; osa_db *db=osa_db_open(dbpath,&err);
    CHECK(db&&osa_db_migrate(db,&err));
    if(!db){clean_root();return;}
    osa_model m={0}; osa_strlcpy(m.kind,"asr",sizeof m.kind); osa_strlcpy(m.engine,"whisper",sizeof m.engine);
    osa_strlcpy(m.id,"a",sizeof m.id); osa_strlcpy(m.path,"/a",sizeof m.path); osa_strlcpy(m.status,"ready",sizeof m.status);
    CHECK(osa_store_model_add(db,&m,&err));
    osa_strlcpy(m.id,"b",sizeof m.id); osa_strlcpy(m.path,"/b",sizeof m.path);
    CHECK(osa_store_model_add(db,&m,&err));
    CHECK(osa_store_model_set_active(db,"a",&err));
    osa_rs rs; CHECK(osa_sql_query(db,"SELECT COUNT(*) AS n FROM Models WHERE kind='asr' AND active=1",&rs,&err));
    CHECK_MSG(osa_rs_int(&rs,0,"n")==1,"exactly one active ASR after first activation"); osa_rs_free(&rs);
    CHECK(osa_store_model_set_active(db,"b",&err));
    CHECK(osa_sql_query(db,"SELECT COUNT(*) AS n FROM Models WHERE kind='asr' AND active=1",&rs,&err));
    CHECK_MSG(osa_rs_int(&rs,0,"n")==1,"exactly one active ASR after switching"); osa_rs_free(&rs);
    CHECK(osa_sql_query(db,"SELECT id FROM Models WHERE kind='asr' AND active=1",&rs,&err));
    CHECK_STR(osa_rs_val(&rs,0,"id"),"b"); osa_rs_free(&rs);
    CHECK(!osa_store_model_set_active(db,"missing",&err));
    CHECK(osa_sql_query(db,"SELECT id FROM Models WHERE kind='asr' AND active=1",&rs,&err));
    CHECK_MSG(osa_rs_val(&rs,0,"id")&&osa_streq(osa_rs_val(&rs,0,"id"),"b"),"failed activation preserves previous active"); osa_rs_free(&rs);
    /* re-registering an active model must not silently deactivate it */
    osa_strlcpy(m.id,"b",sizeof m.id); CHECK(osa_store_model_add(db,&m,&err));
    CHECK(osa_sql_query(db,"SELECT COUNT(*) AS n FROM Models WHERE kind='asr' AND active=1",&rs,&err));
    CHECK(osa_rs_int(&rs,0,"n")==1); osa_rs_free(&rs);
    osa_db_close(db); clean_root();
}

static void test_long_history(void){
    suite("long transcript persistence");
    use_temp_root("hist");
    char dbpath[512]; snprintf(dbpath,sizeof dbpath,"%s/test.db",g_root);
    osa_error err; osa_db *db=osa_db_open(dbpath,&err); CHECK(db&&osa_db_migrate(db,&err));
    if(!db){clean_root();return;}
    osa_transcription t={0}; osa_strlcpy(t.id,"long",sizeof t.id); osa_strlcpy(t.language,"fa",sizeof t.language);
    osa_strlcpy(t.source,"file",sizeof t.source); osa_strlcpy(t.status,"done",sizeof t.status);
    memset(t.text,'x',20000); t.text[20000]=0; t.duration_ms=1234;
    CHECK(osa_store_transcription_add(db,&t,&err));
    osa_rs rs; CHECK(osa_store_transcription_list(db,&rs,10,NULL,&err));
    CHECK_MSG(rs.nrows==1&&strlen(osa_rs_val(&rs,0,"text"))==20000,"20k transcript survives DB round trip");
    CHECK_MSG(osa_rs_i64(&rs,0,"duration_ms")==1234,"duration_ms survives DB round trip");
    osa_rs_free(&rs); osa_db_close(db); clean_root();
}

static void test_audio_wav(void){
    suite("audio wav round trip");
    use_temp_root("audio");
    char path[512]; snprintf(path,sizeof path,"%s/tone.wav",g_root);
    enum{N=1600}; float pcm[N]; for(int i=0;i<N;i++)pcm[i]=0.2f*sinf(2.0f*3.14159265f*440.0f*i/16000.0f);
    CHECK(osa_audio_save_wav(path,pcm,N,16000));
    osa_audio a; osa_error err; CHECK(osa_audio_load(path,&a,&err));
    CHECK_MSG(a.n==N&&a.src_rate==16000,"decoded %lld frames @ %d Hz",(long long)a.n,a.src_rate);
    osa_audio_free(&a); clean_root();
}

/* The exact regression from the field: the app logged a correct Windows path,
 * but the child received C:UsersAdministratorDesktopNew folderdataModels...
 * because the command-line builder counted backslashes before ordinary letters
 * and then dropped them. Quote construction is portable so this test runs on
 * Linux too; a reference MSVCRT parser checks the round trip. */
static int msvcrt_parse(const char *s, char out[][1024], int max){
    int argc=0; const char *p=s;
    while(*p && argc<max){
        while(*p==' '||*p=='\t')p++;
        if(!*p)break;
        char *dst=out[argc]; size_t n=0; bool inq=false;
        while(*p){
            if(*p=='\\'){
                size_t bs=0; while(*p=='\\'){bs++;p++;}
                if(*p=='"'){
                    for(size_t i=0;i<bs/2;i++)dst[n++]='\\';
                    if(bs%2)dst[n++]='"'; else inq=!inq;
                    p++;
                }else{
                    for(size_t i=0;i<bs;i++)dst[n++]='\\';
                }
            }else if(*p=='"'){inq=!inq;p++;}
            else if(!inq && (*p==' '||*p=='\t'))break;
            else dst[n++]=*p++;
            if(n>=1023)break;
        }
        dst[n]=0; argc++;
    }
    return argc;
}
static void test_process_quoting(void){
    suite("Windows argv quoting (backslashes / spaces / Persian)");
    const char *user_argv[]={
        "C:\\Users\\Administrator\\Desktop\\New folder\\data\\Cache\\OpenSpeechWorker.exe",
        "--kind","lang","--model",
        "C:\\Users\\Administrator\\Desktop\\New folder\\data\\Models\\glotlid-small.bin",
        "--threads","2",NULL};
    osa_error err; osa_err_ok(&err);
    char *cmd=osa_process_quote_argv(user_argv,&err);
    CHECK_MSG(cmd!=NULL,"quote builder succeeds: %s",cmd?"ok":err.msg);
    if(cmd){
        const char *want=
            "\"C:\\Users\\Administrator\\Desktop\\New folder\\data\\Cache\\OpenSpeechWorker.exe\""
            " \"--kind\" \"lang\" \"--model\""
            " \"C:\\Users\\Administrator\\Desktop\\New folder\\data\\Models\\glotlid-small.bin\""
            " \"--threads\" \"2\"";
        CHECK_STR(cmd,want);
        char got[16][1024]; int n=msvcrt_parse(cmd,got,16);
        CHECK_MSG(n==7,"the quoted line parses back to 7 arguments (got %d)",n);
        bool same=n==7;
        for(int i=0;same&&user_argv[i];i++)same=osa_streq(got[i],user_argv[i]);
        CHECK_MSG(same,"every argument survives quoting, especially the model path");
        free(cmd);
    }
    const char *cases[][4]={
        {"exe","C:\\Users\\بهنام\\Desktop\\پوشه جدید\\data\\Models\\glotlid-small.bin",NULL},
        {"exe","C:\\folder with space\\trailing\\",NULL},
        {"exe","a\"b\\c","a\\\"b",NULL},
        {"exe","a\\","a\\\\",NULL},
        {"exe",""," space ",NULL},
        {"C:\\Program Files\\OpenSpeech AI\\OpenSpeechWorker.exe","--model","C:\\مدل ها\\z.bin",NULL},
    };
    for(size_t k=0;k<sizeof cases/sizeof cases[0];k++){
        char *q=osa_process_quote_argv(cases[k],&err);
        CHECK_MSG(q!=NULL,"case %zu quotes",(size_t)k);
        if(!q)continue;
        char got[4][1024]; int n=msvcrt_parse(q,got,4);
        int want=0; while(cases[k][want])want++;
        bool same=n==want;
        for(int i=0;same&&i<want;i++)same=osa_streq(got[i],cases[k][i]);
        CHECK_MSG(same,"case %zu round-trips through MSVCRT rules",(size_t)k);
        free(q);
    }
    CHECK(osa_process_quote_argv(NULL,&err)==NULL);
    const char *empty[]={NULL};
    CHECK(osa_process_quote_argv(empty,&err)==NULL);
}

static void test_worker_isolation(void){
    suite("worker isolation / invalid model");
    char exe[512]; snprintf(exe,sizeof exe,"build/OpenSpeechWorker");
    if(!osa_file_exists(exe)){printf("  SKIP worker binary not built (%s)\n",exe);return;}
    const char *argv[]={exe,"--kind","mt","--model","/nonexistent/model.gguf",NULL};
    osa_error err; osa_process *p=osa_process_start(argv,&err);
    CHECK_MSG(p!=NULL,"worker starts: %s",p?"ok":err.msg);
    if(!p)return;
    char *line=NULL; bool got=osa_process_line(p,10000,NULL,&line,&err);
    CHECK_MSG(got&&line&&strstr(line,"\"ok\":false"),"invalid model returns JSON error, not ready");
    osa_process_stop(p);
}

static void test_shell_gate(void){
    suite("shell gating on fresh install");
    use_temp_root("shell");
    osa_error err; osa_app *app=osa_app_create(&err); CHECK(app!=NULL);
    if(!app){clean_root();return;}
    static shell_state sh; shell_init(&sh,app);
    for(int i=0;i<200&&sh.restoring;i++){shell_job_tick(&sh);osa_sleep_ms(5);}
    CHECK(!sh.restoring);
    CHECK(!sh.models_ready);
    CHECK(!shell_select_audio(&sh,"/tmp/x.wav"));
    CHECK(!shell_run_transcribe(&sh));
    osa_strlcpy(sh.tr_in,"سلام",sizeof sh.tr_in);
    shell_run_translate(&sh); CHECK(!sh.job);
    shell_shutdown(&sh); osa_app_destroy(app); clean_root();
}

/* The real worker, a real (synthetic) fastText file, a real round trip. This is
 * the test that would have caught the child inheriting a non-inheritable stdin:
 * the handshake succeeded and every request afterwards failed. */
static void test_worker_protocol(void){
    suite("worker protocol (real process)");
    const char *exe="build/OpenSpeechWorker", *model="build/test-lid.bin";
    if(!osa_file_exists(exe)||!osa_file_exists(model)){
        printf("  SKIP worker or fixture missing (run make tests)\n");return;
    }
    osa_error err;osa_err_ok(&err);
    const char *argv[]={exe,"--kind","lang","--model",model,"--threads","1",NULL};
    osa_process *p=osa_process_start(argv,&err);
    CHECK_MSG(p!=NULL,"worker starts: %s",p?"ok":err.msg);
    if(!p)return;
    char *line=NULL;
    bool got=osa_process_line(p,20000,NULL,&line,&err);
    CHECK_MSG(got,"ready line arrives: %s",got?"ok":err.msg);
    if(got){
        bool ok=false;char event[32]={0},kind[16]={0};
        osa_json_get_bool(line,"ok",&ok);
        osa_json_get_string(line,"event",event,sizeof event);
        osa_json_get_string(line,"kind",kind,sizeof kind);
        CHECK(ok);CHECK_STR(event,"ready");CHECK_STR(kind,"lang");
        CHECK_MSG(strstr(line,"\"en\"")!=NULL,"the label list is populated (nwords+nlabels)");
        free(line);line=NULL;
    }
    /* two requests over the same pipe: stdin must still be open after ready */
    const char *reqs[]={"{\"op\":\"detect\",\"text\":\"hello world\"}",
                        "{\"op\":\"detect\",\"text\":\"سلام دنیا\"}"};
    const char *want[]={"en","fa"};
    for(int i=0;i<2;i++){
        CHECK(osa_process_write(p,reqs[i],&err));
        got=osa_process_line(p,20000,NULL,&line,&err);
        CHECK_MSG(got,"reply %d arrives: %s",i+1,got?"ok":err.msg);
        if(got){char code[8]={0};osa_json_get_string(line,"language",code,sizeof code);
            CHECK_STR(code,want[i]);free(line);line=NULL;}
    }
    osa_process_write(p,"{\"op\":\"shutdown\"}",&err);
    osa_process_stop(p);

    /* GlotLID / FLORES labels ("pes_Arab") must arrive as the two-letter code
     * the translator and Whisper use. */
    if(osa_file_exists("build/test-lid-flores.bin")){
        const char *fa[]={exe,"--kind","lang","--model","build/test-lid-flores.bin","--threads","1",NULL};
        osa_process *f=osa_process_start(fa,&err);
        CHECK_MSG(f!=NULL,"FLORES-label worker starts");
        if(f){
            line=NULL;
            if(osa_process_line(f,20000,NULL,&line,&err)){
                CHECK_MSG(strstr(line,"\"fa\"")!=NULL,"labels are normalised in the ready list");
                free(line);line=NULL;
            }
            osa_process_write(f,"{\"op\":\"detect\",\"text\":\"سلام دنیا\"}",&err);
            if(osa_process_line(f,20000,NULL,&line,&err)){
                char code[16]={0};osa_json_get_string(line,"language",code,sizeof code);
                CHECK_STR(code,"fa");free(line);line=NULL;
            }else CHECK_MSG(false,"FLORES detect answered");
            osa_process_stop(f);
        }
    }

    /* The worker must receive the path verbatim. On POSIX a backslash is an
     * ordinary filename character, so this file exercises spaces, Persian and
     * literal backslashes in one real load + detect round trip. */
    {
        const char *dir="build/worker path test";
        const char *name="C:\\Users\\Administrator\\Desktop\\New folder\\data\\Models\\glotlid-small.bin";
        char path[700]; snprintf(path,sizeof path,"%s/%s",dir,name);
        osa_mkdirs(dir);
        osa_buf fixture; osa_buf_init(&fixture);
        if(osa_file_read(model,&fixture)){
            CHECK(osa_file_write(path,fixture.data,fixture.len));
            const char *ha[]={exe,"--kind","lang","--model",path,"--threads","1",NULL};
            osa_process *h=osa_process_start(ha,&err);
            CHECK_MSG(h!=NULL,"worker starts with a spaces/Persian/backslash path: %s",h?"ok":err.msg);
            if(h){
                line=NULL;
                bool r=osa_process_line(h,20000,NULL,&line,&err);
                CHECK_MSG(r,"hostile-path ready line arrives: %s",r?"ok":err.msg);
                if(r){free(line);line=NULL;}
                osa_process_write(h,"{\"op\":\"detect\",\"text\":\"سلام دنیا\"}",&err);
                r=osa_process_line(h,20000,NULL,&line,&err);
                if(r){char code[16]={0};osa_json_get_string(line,"language",code,sizeof code);
                    CHECK_STR(code,"fa");free(line);line=NULL;}
                else CHECK_MSG(false,"hostile-path detect answered");
                osa_process_stop(h);
            }
            osa_file_remove(path);
        }else CHECK_MSG(false,"read LID fixture for hostile-path test");
        osa_buf_free(&fixture);
        osa_dir_remove_recursive(dir);
    }

    /* A bad file must report the loader's own sentence, not a generic one. */
    const char *bad[]={exe,"--kind","lang","--model","build/does-not-exist.bin","--threads","1",NULL};
    osa_process *q=osa_process_start(bad,&err);
    if(q){
        line=NULL;
        if(osa_process_line(q,20000,NULL,&line,&err)){
            bool ok=true;char msg[512]={0};
            osa_json_get_bool(line,"ok",&ok);osa_json_get_string(line,"error",msg,sizeof msg);
            CHECK(!ok);
            CHECK_MSG(msg[0]!=0,"the worker explains the failure: %s",msg);
            free(line);
        }else CHECK_MSG(false,"a failing load still answers with one line");
        osa_process_stop(q);
    }
}

/* Models folder: the choice persists, and Sync reports what it cannot verify
 * instead of registering an arbitrary file as an engine. */
static void test_models_folder_and_sync(void){
    suite("models folder + sync");
    osa_error err;osa_err_ok(&err);
    osa_app *app=osa_app_create(&err);
    CHECK_MSG(app!=NULL,"app created: %s",app?err.msg:"");
    if(!app)return;
    char dir[OSAI_MAX_PATH]={0};osa_app_models_dir(app,dir,sizeof dir);
    CHECK_MSG(strstr(dir,"Models")!=NULL,"default models folder is data/Models (%s)",dir);

    char chosen[512];snprintf(chosen,sizeof chosen,"%smy-models",osa_path(OSAI_DIR_ROOT));
    CHECK(osa_mkdirs(chosen));
    CHECK(osa_app_set_models_dir(app,chosen,&err));
    char back[OSAI_MAX_PATH]={0};osa_app_models_dir(app,back,sizeof back);
    CHECK_MSG(strstr(back,"my-models")!=NULL,"the chosen folder is reported back (%s)",back);

    /* a file that is not in the catalogue: reported, never registered */
    char fake[700];snprintf(fake,sizeof fake,"%s/random.gguf",chosen);
    char *junk=malloc(200000);memset(junk,7,200000);
    CHECK(osa_file_write(fake,junk,200000));free(junk);
    osa_sync_result res;
    CHECK(osa_app_sync_models(app,NULL,NULL,NULL,&res,&err));
    CHECK_MSG(res.added==0,"an unverified file is never registered (%d added)",res.added);
    CHECK_MSG(res.unknown==1,"the unverified file is reported (%d unknown)",res.unknown);
    CHECK_MSG(strstr(res.unknown_names,"random.gguf")!=NULL,"the report names the file");
    char *json=osa_app_models_json(app,NULL);
    CHECK_MSG(json&&osa_streq(json,"[]"),"nothing was installed by the sync");
    free(json);

    /* the choice survives a restart */
    osa_app_destroy(app);
    app=osa_app_create(&err);
    if(app){char again[OSAI_MAX_PATH]={0};osa_app_models_dir(app,again,sizeof again);
        CHECK_MSG(strstr(again,"my-models")!=NULL,"the folder choice persisted (%s)",again);}

    /* delete removes the row and the file */
    osa_model m;memset(&m,0,sizeof m);
    osa_strlcpy(m.id,"test-delete",sizeof m.id);osa_strlcpy(m.name,"Test",sizeof m.name);
    osa_strlcpy(m.kind,"mt",sizeof m.kind);osa_strlcpy(m.engine,"llama.cpp",sizeof m.engine);
    osa_strlcpy(m.path,fake,sizeof m.path);osa_strlcpy(m.status,"installed",sizeof m.status);
    m.size_bytes=200000;
    char appdb[600];snprintf(appdb,sizeof appdb,"%sopenspeech.db",osa_path(OSAI_DIR_DB));
    osa_db *db=osa_db_open(appdb,&err);
    CHECK_MSG(db!=NULL,"second connection to the app database: %s",db?"ok":err.msg);
    if(db){CHECK(osa_store_model_add(db,&m,&err));osa_db_close(db);}
    CHECK(osa_file_exists(fake));
    CHECK_MSG(osa_app_remove_model(app,"test-delete",&err),"remove succeeds: %s",err.msg);
    CHECK(!osa_file_exists(fake));
    json=osa_app_models_json(app,NULL);
    CHECK_MSG(json&&osa_streq(json,"[]"),"the database row is gone");
    free(json);
    osa_app_destroy(app);
}

static void test_worker_path(void){
    suite("worker resolution");
    char path[OSAI_MAX_PATH]={0}; osa_error err;
    bool ok=osa_worker_path(path,sizeof path,&err);
    CHECK_MSG(ok,"worker path resolved: %s",ok?path:err.msg);
    CHECK_MSG(ok&&osa_file_exists(path),"the resolved worker actually exists");
}

/* The product writes only under <exe dir>/data. This runs the real host in a
 * child process with OSAI_DATA_DIR removed and a throwaway HOME that holds an
 * old-style profile, so both the new location and the one-time move are proved
 * rather than asserted in a comment. */
static void test_activation_diagnostics(void){
    suite("activation diagnostics");
    use_temp_root("actdiag");
    osa_error err;osa_err_ok(&err);
    osa_app *app=osa_app_create(&err);
    CHECK(app!=NULL);
    if(app){
        char dbp[600];snprintf(dbp,sizeof dbp,"%sopenspeech.db",osa_path(OSAI_DIR_DB));
        osa_db *db=osa_db_open(dbp,&err);
        if(db){
            osa_model m;memset(&m,0,sizeof m);
            osa_strlcpy(m.id,"fasttext-lid176-ftz",sizeof m.id);
            osa_strlcpy(m.name,"fastText LID 176 (compressed)",sizeof m.name);
            osa_strlcpy(m.kind,"lang",sizeof m.kind);osa_strlcpy(m.engine,"fastText",sizeof m.engine);
            osa_strlcpy(m.status,"installed",sizeof m.status);
            osa_strlcpy(m.path,"/nonexistent/lid.176.ftz",sizeof m.path);
            m.size_bytes=938013;
            CHECK(osa_store_model_add(db,&m,&err));osa_db_close(db);
        }
        CHECK(!osa_app_activate_model(app,"fasttext-lid176-ftz",&err));
        CHECK_MSG(strstr(err.msg,"missing")!=NULL,"the failure says the file is missing: %s",err.msg);
        CHECK_MSG(strstr(err.msg,"/nonexistent/lid.176.ftz")!=NULL,"the failure names the path");
        CHECK_MSG(strstr(err.msg,"Sync")!=NULL,"the failure tells the user how to repair it");
        osa_app_destroy(app);
    }
    clean_root();
}

/* The reported bug: a GGUF that llama.cpp refuses produced one opaque sentence
 * that named neither the reason nor the file. Each cause must now be distinct,
 * and the tooling that survives it must say where the full log is. */
static void test_model_load_diagnostics(void){
    suite("model load diagnostics name the real cause");
    const char *exe="build/OpenSpeechWorker";
    if(!osa_file_exists(exe)){printf("  SKIP worker binary not built (%s)\n",exe);return;}
    char dir[256];snprintf(dir,sizeof dir,"/tmp/osai-mldiag-XXXXXX");
    if(!mkdtemp(dir)){CHECK_MSG(false,"mkdtemp");return;}

    /* 1. a file that is not GGUF at all */
    char notgguf[400];snprintf(notgguf,sizeof notgguf,"%s/not.gguf",dir);
    unsigned char junk[4096];memset(junk,0x5a,sizeof junk);
    CHECK(osa_file_write(notgguf,junk,sizeof junk));
    const char *a1[]={exe,"--kind","mt","--model",notgguf,"--threads","2",NULL};
    osa_error err;osa_err_ok(&err);
    osa_process *p=osa_process_start(a1,&err);
    CHECK_MSG(p!=NULL,"worker starts on a non-GGUF file: %s",p?"ok":err.msg);
    if(p){
        char *line=NULL;
        CHECK(osa_process_line(p,20000,NULL,&line,&err));
        CHECK_MSG(line&&strstr(line,"\"ok\":false"),"it reports a failure, not ready");
        CHECK_MSG(line&&strstr(line,"not a GGUF")!=NULL,
                  "the message says the file is not a GGUF container: %s",line?line:"?");
        free(line);osa_process_stop(p);
    }

    /* 2. a fastText model handed to the translation engine, and the reverse:
     *    each engine names the other format instead of "wrong file format" */
    char ft[400];snprintf(ft,sizeof ft,"%s/real.ftz",dir);
    FILE *f=fopen(ft,"wb");
    CHECK(f!=NULL);
    if(f){
        int32_t magic=793712314,version=12;
        fwrite(&magic,sizeof magic,1,f);fwrite(&version,sizeof version,1,f);
        unsigned char pad[9000];memset(pad,1,sizeof pad);fwrite(pad,1,sizeof pad,f);
        fclose(f);
    }
    const char *a2[]={exe,"--kind","mt","--model",ft,"--threads","2",NULL};
    p=osa_process_start(a2,&err);
    if(p){
        char *line=NULL;
        CHECK(osa_process_line(p,20000,NULL,&line,&err));
        CHECK_MSG(line&&strstr(line,"fastText")!=NULL&&strstr(line,"GGUF")!=NULL,
                  "a fastText file is named as such, not blamed on memory: %s",line?line:"?");
        free(line);osa_process_stop(p);
    }
    const char *a3[]={exe,"--kind","lang","--model",notgguf,NULL};
    p=osa_process_start(a3,&err);
    if(p){
        char *line=NULL;
        CHECK(osa_process_line(p,20000,NULL,&line,&err));
        CHECK_MSG(line&&strstr(line,"fastText")!=NULL,
                  "the language engine says it did not get a fastText model: %s",line?line:"?");
        free(line);osa_process_stop(p);
    }
    remove(notgguf);remove(ft);
    char cmd[512];snprintf(cmd,sizeof cmd,"rmdir %s 2>/dev/null",dir);system(cmd);
}

/* Hashes must be cancellable: Sync runs on a background thread whose Cancel and
 * window-close paths wait for it, so an uninterruptible multi-gigabyte read is
 * exactly what made the window freeze. */
static void test_hash_cancel(void){
    suite("cancellable hashing");
    char path[256];snprintf(path,sizeof path,"/tmp/osai-hash-XXXXXX");
    int fd=mkstemp(path);
    CHECK(fd>=0);
    if(fd<0)return;
    char chunk[65536];memset(chunk,3,sizeof chunk);
    for(int i=0;i<160;i++){                       /* ~10 MB */
        if(write(fd,chunk,sizeof chunk)!=(ssize_t)sizeof chunk)break;
    }
    close(fd);

    char a[65]={0},b[65]={0};uint64_t na=0,nb=0;
    CHECK(osa_sha256_file_ex(path,a,&na,NULL,NULL,NULL));
    osa_sha256_file(path,b,&nb);
    CHECK_MSG(na==nb&&na>0,"the extended hash reports the same size");
    CHECK_MSG(osa_streq(a,b),"the extended hash agrees with osa_sha256_file");
    CHECK_MSG(strlen(a)==64,"the digest is 64 hex characters");

    volatile int stop=1;
    char c[65]={0};uint64_t nc=0;
    bool ok=osa_sha256_file_ex(path,c,&nc,&stop,NULL,NULL);
    CHECK_MSG(!ok,"an already-cancelled hash returns false");
    CHECK_MSG(c[0]==0,"a cancelled hash leaves no digest behind");

    remove(path);
}

/* A genuine fastText container, byte for byte. These are the files that used to
 * fail with "worker: load failed: vector": the worker handed fastText a stream
 * starting at offset 0, so the magic was read as the first parameter and every
 * field after it was skewed. Writing the container ourselves keeps the test
 * independent of whether any model can be downloaded.
 *
 * Layout (fastText saveModel/loadModel):
 *   magic:int32 version:int32
 *   Args: 12x int32 + double
 *   Dict: size,nwords,nlabels:int32  ntokens,pruneidx_size:int64
 *         per entry: word\0 | count:int64 | type:int8      (type is ONE byte)
 *   pruneidx pairs (int32,int32) when pruneidx_size>0   (-1 = never pruned)
 *   quant:bool | Matrix input | qout:bool | Matrix output
 *   Matrix: dense  -> m:int64 n:int64 floats[m*n]
 *           quant  -> qnorm:bool m:int32 n:int32 codesize:int32 codes
 *                     ProductQuantizer: dim,nsubq,dsub,lastdsub:int32 floats[dim*256]
 */
static void test_fasttext_language_model(void){
    suite("fastText language model container");
    const char *exe="build/OpenSpeechWorker";
    if(!osa_file_exists(exe)){printf("  SKIP worker binary not built (%s)\n",exe);return;}

    /* ---- writer ---------------------------------------------------- */
    char dir[256];snprintf(dir,sizeof dir,"/tmp/osai-ft-XXXXXX");
    if(!mkdtemp(dir)){CHECK_MSG(false,"mkdtemp");return;}

    struct { const char *name; int quant; int nlabels; int truncate_at; const char *expect; } cases[]={
        {"dense.bin",   0, 4, 0, NULL},                    /* must load        */
        {"quant.bin",   1, 4, 0, NULL},                    /* must load        */
        {"short.bin",   0, 4, 40, "ends inside"},          /* truncated dict   */
        {"onelabel.bin",0, 1, 0, "fewer than two labels"}, /* not a LID model  */
    };
    const char *words[]={"salam","hello","hallo","marhaba","kotob","book","buch","kitab"};
    const char *labels[]={"__label__fa","__label__en","__label__de","__label__ar"};

    for(size_t k=0;k<sizeof cases/sizeof cases[0];k++){
        unsigned char buf[65536];size_t n=0;
        int dim=16;
        /* header */
        #define PUTI32(v) do{int32_t _v=(int32_t)(v);memcpy(buf+n,&_v,4);n+=4;}while(0)
        #define PUTI64(v) do{int64_t _v=(int64_t)(v);memcpy(buf+n,&_v,8);n+=8;}while(0)
        PUTI32(793712314);PUTI32(12);
        int32_t args[12]={dim,5,5,1,5,1,2,3,0,0,0,100};
        for(int i=0;i<12;i++)PUTI32(args[i]);
        {double t=0.0001;memcpy(buf+n,&t,8);n+=8;}
        int nwords=8,nlabels=cases[k].nlabels,total=nwords+nlabels;
        PUTI32(total);PUTI32(nwords);PUTI32(nlabels);PUTI64(1000);PUTI64(-1);
        for(int i=0;i<nwords;i++){
            size_t l=strlen(words[i]);memcpy(buf+n,words[i],l);n+=l;buf[n++]=0;
            PUTI64(i+1);buf[n++]=0;                        /* entry_type::word */
        }
        for(int i=0;i<nlabels;i++){
            size_t l=strlen(labels[i]);memcpy(buf+n,labels[i],l);n+=l;buf[n++]=0;
            PUTI64(50);buf[n++]=1;                         /* entry_type::label */
        }
        buf[n++]=cases[k].quant?1:0;
        if(cases[k].quant){
            int codesize=total*((dim+1)/2);
            buf[n++]=0;                                    /* qnorm = false: one BYTE */
            PUTI32(total);PUTI32(dim);PUTI32(codesize);
            memset(buf+n,0,(size_t)codesize);n+=(size_t)codesize;
            PUTI32(dim);PUTI32(dim/2);PUTI32(2);PUTI32(dim%2?dim%2:2);
            memset(buf+n,0,(size_t)dim*256*4);n+=(size_t)dim*256*4;
        }else{
            PUTI64(total);PUTI64(dim);
            memset(buf+n,0,(size_t)total*dim*4);n+=(size_t)total*dim*4;
        }
        buf[n++]=0;                                        /* qout = false */
        PUTI64(nlabels);PUTI64(dim);
        memset(buf+n,0,(size_t)nlabels*dim*4);n+=(size_t)nlabels*dim*4;
        #undef PUTI32
        #undef PUTI64
        if(cases[k].truncate_at) n=(size_t)cases[k].truncate_at;
        char path[400];snprintf(path,sizeof path,"%s/%s",dir,cases[k].name);
        CHECK_MSG(osa_file_write(path,buf,n),"fixture written: %s",cases[k].name);

        const char *argv[]={exe,"--kind","lang","--model",path,NULL};
        osa_error err;osa_err_ok(&err);
        osa_process *p=osa_process_start(argv,&err);
        CHECK_MSG(p!=NULL,"worker starts on %s: %s",cases[k].name,p?"ok":err.msg);
        if(!p)continue;
        char *line=NULL;
        bool got=osa_process_line(p,60000,NULL,&line,&err);
        CHECK_MSG(got,"the worker replies for %s",cases[k].name);
        if(got&&line){
            if(!cases[k].expect){
                CHECK_MSG(strstr(line,"\"ok\":true")!=NULL&&strstr(line,"ready")!=NULL,
                          "%s loads and reports ready: %s",cases[k].name,line);
                CHECK_MSG(strstr(line,"fa")!=NULL,"%s exposes its language labels: %s",cases[k].name,line);
            }else{
                CHECK_MSG(strstr(line,"\"ok\":false")!=NULL,"%s is refused: %s",cases[k].name,line);
                CHECK_MSG(strstr(line,cases[k].expect)!=NULL,
                          "%s explains itself (wanted \"%s\"): %s",cases[k].name,cases[k].expect,line);
                CHECK_MSG(strcmp(line,"vector")!=0&&strstr(line,"\"error\":\"vector\"")==NULL,
                          "%s never reports a bare C++ exception name",cases[k].name);
            }
            free(line);
        }
        osa_process_stop(p);
        remove(path);
    }
    char cmd[512];snprintf(cmd,sizeof cmd,"rmdir %s 2>/dev/null",dir);system(cmd);
}

/* The live screen's endpoint: speech start, hangover, noise-floor tracking and
 * the forced cut. Driven by dBFS frames, so these run in microseconds and cover
 * the parts that used to make live capture feel broken: a segment that never
 * closed while the speaker paused, and one that grew without bound. */
static void test_live_endpoint(void){
    suite("live endpointing (speech boundaries)");
    live_ep_params p;
    live_ep_defaults(&p);
    CHECK(p.frame_s>0.0f && p.hangover_s>0.2f && p.max_segment_s>1.0f);

    /* A quiet room is silence: nothing starts, however many frames pass. */
    live_ep e;
    live_ep_init(&e,&p);
    bool quiet_still=true;
    for(int i=0;i<300;i++) if(live_ep_frame(&e,-62.0f)!=LIVE_EP_NONE) quiet_still=false;
    CHECK_MSG(quiet_still,"300 frames of a quiet room produce no event");
    CHECK(!e.in_segment);
    CHECK_MSG(live_ep_floor_db(&e)>-75.0f,"the noise floor is learned, not -inf: %.1f",live_ep_floor_db(&e));

    /* Speech starts a segment after min_speech_s, and reports it. */
    int events=0;
    for(int i=0;i<40 && !events;i++) events|=live_ep_frame(&e,-30.0f);
    CHECK_MSG(events==LIVE_EP_SPEECH_START,"speech reports a start: %d",events);
    CHECK(e.in_segment);

    /* While the speaker keeps talking, the segment stays open. */
    bool no_flapping=true;
    for(int i=0;i<100;i++) if(live_ep_frame(&e,-30.0f)!=LIVE_EP_NONE) no_flapping=false;
    CHECK_MSG(no_flapping,"sustained speech does not re-trigger the start event");
    CHECK(e.in_segment);

    /* A pause shorter than the hangover does not end the sentence. */
    int n=p.hangover_s/p.frame_s/2;
    for(int i=0;i<n;i++) live_ep_frame(&e,-64.0f);
    CHECK_MSG(e.in_segment,"a brief pause keeps the segment open (%d frames)",n);

    /* A pause longer than the hangover closes it exactly once. */
    events=0;int frames=0;
    while(frames<(int)(p.hangover_s/p.frame_s)+6){
        int ev=live_ep_frame(&e,-64.0f);
        if(ev==LIVE_EP_SEGMENT_READY){events++;break;}
        frames++;
    }
    CHECK_MSG(events==1,"the hangover closes the segment: events=%d frames=%d",events,frames);
    CHECK(!e.in_segment);
    /* and the next sentence starts a new segment after real speech again */
    events=0;
    for(int i=0;i<40 && !events;i++) events|=live_ep_frame(&e,-30.0f);
    CHECK_MSG(events==LIVE_EP_SPEECH_START,"a second sentence starts a new segment: %d",events);

    /* A click (too short to be speech) never opens a segment. */
    live_ep_init(&e,&p);
    for(int i=0;i<200;i++) live_ep_frame(&e,-64.0f);
    for(int i=0;i<2;i++) live_ep_frame(&e,-10.0f);
    for(int i=0;i<100;i++) live_ep_frame(&e,-64.0f);
    CHECK_MSG(!e.in_segment,"a two-frame click does not start a segment");
    CHECK(e.events==0);

    /* A segment that never pauses is cut at max_segment_s instead of growing. */
    live_ep_init(&e,&p);
    for(int i=0;i<100;i++) live_ep_frame(&e,-64.0f);      /* learn the room first */
    events=0;
    for(int i=0;i<(int)((p.max_segment_s+1.0f)/p.frame_s);i++){
        int ev=live_ep_frame(&e,-30.0f);
        if(ev==LIVE_EP_SEGMENT_READY){events++;break;}
    }
    CHECK_MSG(events==1,"an endless sentence is cut, not grown forever: %d",events);

    /* The floor must not climb into speech: a floor that followed the voice
     * would go deaf after a few seconds and never close a segment. This is the
     * failure that made live capture "stop after the first sentence". */
    live_ep_init(&e,&p);
    for(int i=0;i<200;i++) live_ep_frame(&e,-64.0f);   /* learn a quiet room */
    float floor_quiet=live_ep_floor_db(&e);
    for(int i=0;i<400;i++) live_ep_frame(&e,-28.0f);   /* 8 s of speech      */
    float floor_speech=live_ep_floor_db(&e);
    CHECK_MSG(floor_speech<floor_quiet+6.0f,
              "8 s of speech does not drag the floor into the voice (%.1f -> %.1f)",
              floor_quiet,floor_speech);
    events=0;
    for(int i=0;i<(int)(p.hangover_s/p.frame_s)+8;i++){
        int ev=live_ep_frame(&e,-64.0f);
        if(ev==LIVE_EP_SEGMENT_READY){events++;break;}
    }
    CHECK_MSG(events==1,"after a long stretch of speech the pause still closes it: %d",events);

    /* Cold start: the user is already talking when capture begins. That opening
     * word must be heard — the first frame is not the room. */
    live_ep_init(&e,&p);
    events=0;
    for(int i=0;i<40 && !events;i++) events|=live_ep_frame(&e,-22.0f);
    CHECK_MSG(events==LIVE_EP_SPEECH_START,"speech on the very first frame is heard: %d",events);

    /* The floor does follow a quieter room, so a hiss that stops does not leave
     * the threshold stuck above the speech that follows. */
    live_ep_init(&e,&p);
    for(int i=0;i<200;i++) live_ep_frame(&e,-30.0f);   /* a noisy room       */
    float floor_noisy=live_ep_floor_db(&e);
    for(int i=0;i<200;i++) live_ep_frame(&e,-60.0f);   /* the noise stops    */
    float floor_after=live_ep_floor_db(&e);
    CHECK_MSG(floor_after<floor_noisy-3.0f,
              "the floor follows a room that gets quieter (%.1f -> %.1f)",floor_noisy,floor_after);

    /* The forced cut lands on the quietest recent frame, not in the middle. */
    float db[100];
    for(int i=0;i<100;i++) db[i]=-30.0f;
    db[88]=-61.0f;                                        /* a pause at 1.76 s */
    float cut=live_ep_cut_point(db,100,0,&p);
    CHECK_MSG(cut>1.74f&&cut<1.80f,"the cut goes to the pause at 1.76 s: %.2f",cut);
}

/* The CPU budget. A translation used to start its worker with "cores/2" and the
 * recogniser with every core, which is why the machine sat at 100% while
 * translating. There is now one number, it is never the whole machine by
 * default, and Settings can change it. */
static void test_thread_budget(void){
    suite("CPU budget (threads)");
    use_temp_root("threads");
    osa_error err; osa_app *app=osa_app_create(&err);
    CHECK_MSG(app!=NULL,"app create: %s",app?err.msg:"ok");
    if(!app){clean_root();return;}
    int cores=osa_cpu_count();
    int def=osa_app_threads(app);
    CHECK_MSG(def>=1,"the default budget is at least one thread (got %d)",def);
    CHECK_MSG(def<=8,"the default budget never takes a small PC's everything (got %d of %d cores)",def,cores);
    if(cores>2) CHECK_MSG(def<=cores-1,"one core is always left for the window (%d of %d)",def,cores);
    osa_app_set_threads(app,3);
    CHECK_MSG(osa_app_threads(app)==3,"the budget can be lowered (got %d)",osa_app_threads(app));
    osa_app_set_threads(app,0);
    CHECK_MSG(osa_app_threads(app)==1,"zero threads is clamped to one, never to zero");
    osa_app_set_threads(app,999);
    CHECK_MSG(osa_app_threads(app)==16,"the budget is capped at 16 (got %d)",osa_app_threads(app));
    osa_app_destroy(app); clean_root();
}

/* The builtin synthesiser is the fallback for a machine with no system voices.
 * It cannot pronounce an unknown word, but it must not answer with a growl or
 * with near-silence: every letter has to be audible, nothing may be clipped, and
 * the word has to be spelled out in full rather than cut off after four letters.
 * (On Windows the installed system voices speak the sentence instead.) */
static void test_speech_fallback(void){
    suite("speech synthesis fallback");
    const char *words[]={"hello","world","book","translation","xyz"};
    for(int k=0;k<5;k++){
        int64_t n=0;
        float *pcm=tts_speak(words[k],"en",&n);
        CHECK_MSG(pcm&&n>0,"\"%s\" produces audio: %d samples",words[k],(int)n);
        if(!pcm)continue;
        /* Per 30 ms window: how much of the word is actually audible, and is any
         * of it clipping? A growl is a constant buzz; near-silence is the old
         * peak-normalisation bug. */
        int win=OSAI_SR/33, total=0, audible=0;
        float peak=0;
        for(int64_t i=0;i+win<=n;i+=win){
            double a=0;
            for(int j=0;j<win;j++){a+=(double)pcm[i+j]*pcm[i+j]; if(fabsf(pcm[i+j])>peak)peak=fabsf(pcm[i+j]);}
            total++;
            if(sqrt(a/win)>0.005)audible++;    /* ~-46 dBFS: above any hiss */
        }
        CHECK_MSG(total>0&&audible*10>=total*4,
                  "\"%s\" is audible for most of its length: %d of %d windows above -46 dBFS",
                  words[k],audible,total);
        CHECK_MSG(peak<=0.99f,"\"%s\" never clips: peak=%.2f",words[k],peak);
        /* Longer words are spelled in full: the clip grows with the word. */
        if(osa_streq(words[k],"translation"))
            CHECK_MSG((double)n/OSAI_SR>0.6,"a long word is spelled out, not cut short: %.2f s",(double)n/OSAI_SR);
        free(pcm);
    }
    int64_t n=0;
    float *pcm=tts_speak("سلام دنیا","fa",&n);
    CHECK_MSG(pcm&&n>0,"Persian text with no lexicon entry still produces audio: %d samples",(int)n);
    free(pcm);
    n=0; pcm=tts_speak("","en",&n);
    CHECK_MSG(pcm==NULL&&n==0,"empty text produces no clip at all");
    free(pcm);
}

/* "Listen to the translation" had a path that produced a growl and another that
 * produced near-silence. On a machine with system voices the sentence goes to
 * them; without, the builtin synthesiser must still produce an audible, plausible
 * clip. Which of the two happened is reported, not assumed. */
static void test_speak_path(void){
    suite("speak the translation (listen button)");
    bool platform = osa_tts_platform_available();
    printf("  note the platform speaks with: %s\n",
           platform ? osa_tts_platform_name() : "the builtin synthesiser (no system voices)");
    /* Whatever the backend, the user must never get silence without a message:
     * either the backend accepts the sentence, or the fallback clip exists. */
    /* .wav on the end: osa_audio_load picks the decoder from the extension. */
    char wav[300];
    snprintf(wav,sizeof wav,"/tmp/osai-speak-%d.wav",(int)getpid());
    bool clipped = tts_speak_to_wav("از آشنایی با شما خوشحالم","fa",wav);
    CHECK_MSG(clipped,"the fallback writes a WAV even for Persian text");
    osa_audio au;
    if(osa_audio_load(wav,&au,NULL)){
        double secs=osa_audio_duration_s(&au);
        CHECK_MSG(secs>0.2&&secs<30.0,"the spoken clip has a plausible length: %.2f s",secs);
        double acc=0;
        for(int64_t i=0;i<au.n;i++) acc+=(double)au.samples[i]*au.samples[i];
        double rms=au.n?sqrt(acc/(double)au.n):0;
        CHECK_MSG(rms>0.003,"the spoken clip is audible, not a hiss: rms=%.4f",rms);
        osa_audio_free(&au);
    } else CHECK_MSG(false,"the spoken clip is a readable WAV");
    remove(wav);
    if(platform){
        CHECK_MSG(osa_tts_platform_speak("Hello","en"),"the platform voice accepts a sentence");
    } else {
        printf("  note platform voices absent here: the fallback clip above is what a user would hear\n");
    }
}

/* Capture -> endpoint, end to end, with a real WAV and the real microphone API.
 * The headless build plays a file instead of a device (OSAI_MIC_FILE), which is
 * how this path can be tested without a microphone: three spoken bursts
 * separated by pauses must be reported as three sentences, not one. */
static bool make_burst_wav(const char *path, double *burst_starts, int *n_bursts){
    const double bursts[] = {1.5, 1.2, 0.9};
    const double pause = 1.2;
    int64_t total = 0;
    for (int i = 0; i < 3; i++) total += (int64_t)((bursts[i] + pause) * OSAI_SR);
    float *pcm = (float *)calloc((size_t)total, sizeof(float));
    if (!pcm) return false;
    int64_t at = 0;
    for (int i = 0; i < 3; i++) {
        burst_starts[i] = (double)at / OSAI_SR;
        int64_t n = (int64_t)(bursts[i] * OSAI_SR);
        for (int64_t k = 0; k < n; k++) {
            double t = (double)k / OSAI_SR;
            double env = 1.0;
            if (t < 0.02) env = t / 0.02;
            if (t > bursts[i] - 0.02) env = (bursts[i] - t) / 0.02;
            pcm[at + k] = (float)(0.25 * env * sin(2.0 * 3.14159265358979 * 220.0 * t));
        }
        at += n + (int64_t)(pause * OSAI_SR);
    }
    bool ok = osa_audio_save_wav(path, pcm, total, OSAI_SR);
    free(pcm);
    *n_bursts = 3;
    return ok;
}

static void test_live_capture_pipeline(void){
    suite("live capture -> endpoint (real microphone API, file-backed)");
    char wav[300];
    snprintf(wav, sizeof wav, "/tmp/osai-mic-%d.wav", (int)getpid());
    double starts[3];
    int nb = 0;
    if (!make_burst_wav(wav, starts, &nb)) { CHECK_MSG(false, "could not write the test WAV"); return; }

    setenv("OSAI_MIC_FILE", wav, 1);
    setenv("OSAI_MIC_FAST", "1", 1);
    CHECK_MSG(osa_mic_available(), "capture reports available when a file is configured");

    osa_error err;
    osa_err_ok(&err);
    osa_mic *mic = osa_mic_open(OSAI_SR, &err);
    CHECK_MSG(mic != NULL, "the capture device opens: %s", mic ? "ok" : err.msg);
    if (!mic) { remove(wav); return; }

    live_ep ep;
    live_ep_init(&ep, NULL);
    int starts_seen = 0, segments = 0;
    float buf[1600];
    int reads = 0;
    int n;
    /* Feed the endpoint exactly the way shell_live_tick does: 20 ms frames
     * carved out of whatever the device hands us. */
    static float acc[64 * 1024];
    int n_acc = 0;
    while ((n = osa_mic_read(mic, buf, 1600)) > 0 && reads < 20000) {
        reads++;
        for (int i = 0; i < n; i++) {
            if (n_acc < (int)(sizeof acc / sizeof acc[0])) acc[n_acc++] = buf[i];
            if (n_acc >= OSAI_SR / 50) {
                int ev = live_ep_frame(&ep, live_ep_db(acc, OSAI_SR / 50));
                if (ev == LIVE_EP_SPEECH_START) starts_seen++;
                else if (ev == LIVE_EP_SEGMENT_READY) segments++;
                n_acc = 0;
            }
        }
    }
    osa_mic_close(mic);
    printf("  note %d reads, %d sentences started, %d closed\n", reads, starts_seen, segments);
    CHECK_MSG(starts_seen >= 3, "every spoken burst is detected as speech (%d)", starts_seen);
    CHECK_MSG(segments >= 3, "every pause closes a sentence (%d)", segments);

    unsetenv("OSAI_MIC_FILE");
    unsetenv("OSAI_MIC_FAST");
    remove(wav);
}

static void test_portable_data_dir(void){
    suite("portable data folder + profile migration");
    const char *host="build/openspeech";
    if(!osa_file_exists(host)){printf("  SKIP headless host not built\n");return;}
    char home[256]; snprintf(home,sizeof home,"/tmp/osai-home-XXXXXX");
    if(!mkdtemp(home)){CHECK_MSG(false,"mkdtemp");return;}
    char legacy[512],models[600],probe[700],prefs[600];
    snprintf(legacy,sizeof legacy,"%s/.local/share/OpenSpeechAI",home);
    snprintf(models,sizeof models,"%s/Models",legacy);
    snprintf(probe,sizeof probe,"%s/portable-probe.txt",models);
    snprintf(prefs,sizeof prefs,"%s/settings.ini",legacy);
    CHECK(osa_mkdirs(models));
    CHECK(osa_file_write(probe,"moved",5));
    CHECK(osa_file_write(prefs,"lang=fa\ndark=0\n",14));

    const char *old_home=getenv("HOME");
    char saved_home[512]={0}; if(old_home)osa_strlcpy(saved_home,old_home,sizeof saved_home);
    setenv("HOME",home,1);
    unsetenv("OSAI_DATA_DIR");
    const char *argv[]={host,"--paths-json",NULL};
    osa_error err; osa_process *p=osa_process_start(argv,&err);
    CHECK_MSG(p!=NULL,"host starts: %s",p?"ok":err.msg);
    char root[OSAI_MAX_PATH]={0},exedir[OSAI_MAX_PATH]={0};
    if(p){
        char *line=NULL;
        if(osa_process_line(p,15000,NULL,&line,&err)){
            osa_json_get_string(line,"root",root,sizeof root);
            osa_json_get_string(line,"exe_dir",exedir,sizeof exedir);
            free(line);
        }
        osa_process_stop(p);
    }
    char want[OSAI_MAX_PATH+16]; snprintf(want,sizeof want,"%s/data/",exedir);
    CHECK_MSG(root[0]&&osa_streq(root,want),"data root is <exe dir>/data (%s)",root);
    char moved[OSAI_MAX_PATH+64],movedprefs[OSAI_MAX_PATH+32];
    snprintf(moved,sizeof moved,"%sModels/portable-probe.txt",root);
    snprintf(movedprefs,sizeof movedprefs,"%ssettings.ini",root);
    CHECK_MSG(osa_file_exists(moved),"old profile models were moved beside the exe");
    CHECK_MSG(!osa_file_exists(probe),"nothing is left in the old profile model folder");
    CHECK_MSG(!osa_dir_exists(legacy),"the emptied profile folder is removed");
    /* clean the probe out of the build tree; keep any real developer data */
    osa_file_remove(moved);
    if(osa_file_exists(movedprefs)){
        osa_buf b; osa_buf_init(&b);
        if(osa_file_read(movedprefs,&b)&&b.len==14&&!memcmp(b.data,"lang=fa\ndark=0\n",14))osa_file_remove(movedprefs);
        osa_buf_free(&b);
    }
    osa_dir_remove_recursive(home);
    if(saved_home[0])setenv("HOME",saved_home,1);
    if(g_root[0])setenv("OSAI_DATA_DIR",g_root,1);
}

static void test_optional_real_models(void){
    suite("optional real-model smoke (user-supplied weights)");
    const char *asr=getenv("OSAI_TEST_ASR"),*mt=getenv("OSAI_TEST_MT"),*lid=getenv("OSAI_TEST_LID"),*wav=getenv("OSAI_TEST_WAV");
    if(!asr||!mt||!lid||!wav){printf("  SKIP set OSAI_TEST_ASR/MT/LID/WAV to run real inference\n");return;}
    use_temp_root("real");
    osa_error err; osa_app *app=osa_app_create(&err); CHECK(app!=NULL);
    if(!app){clean_root();return;}
    CHECK(osa_app_install_pack_file(app,asr,&err));
    CHECK(osa_app_install_pack_file(app,mt,&err));
    CHECK(osa_app_install_pack_file(app,lid,&err));
    CHECK(osa_app_activate_model(app,"whisper-medium-q5",&err));
    CHECK(osa_app_activate_model(app,"qwen25-3b-q4km",&err));
    CHECK(osa_app_activate_model(app,"fasttext-lid176",&err));
    CHECK(osa_app_ready(app));
    CHECK(osa_app_set_languages(app,"fa","en",&err));
    osa_app_translation tr; CHECK(osa_app_translate(app,"سلام، حال شما چطور است؟","fa",&tr,&err));
    CHECK_MSG(tr.text[0]!=0,"real MT produced non-empty output");
    osa_app_transcript ts; osa_app_translation mt2;
    CHECK(osa_app_transcribe_and_translate(app,wav,&ts,&mt2,&err));
    CHECK_MSG(ts.text[0]!=0,"real ASR produced non-empty transcript");
    CHECK_MSG(mt2.text[0]!=0,"file transcription translation produced output");
    osa_app_destroy(app); clean_root();
}

int main(void){
    test_json_unicode();
    test_catalog_and_gate();
    test_store_active();
    test_long_history();
    test_audio_wav();
    test_process_quoting();
    test_worker_isolation();
    test_shell_gate();
    test_worker_path();
    test_worker_protocol();
    test_models_folder_and_sync();
    test_activation_diagnostics();
    test_model_load_diagnostics();
    test_hash_cancel();
    test_live_endpoint();
    test_thread_budget();
    test_speech_fallback();
    test_speak_path();
    test_live_capture_pipeline();
    test_fasttext_language_model();
    test_portable_data_dir();
    test_optional_real_models();
    printf("\n%d passed, %d failed\n",g_pass,g_fail);
    return g_fail?1:0;
}
