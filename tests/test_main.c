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
    osa_pack_info packs[16]; int np=osa_app_packs(app,packs,16);
    bool asr=false,mt=false,lid=false,all_verified=true,no_tiny=true;
    for(int i=0;i<np;i++){
        if(osa_streq(packs[i].kind,"asr"))asr=true;
        if(osa_streq(packs[i].kind,"mt"))mt=true;
        if(osa_streq(packs[i].kind,"lang"))lid=true;
        if(!packs[i].url[0]||strlen(packs[i].sha256)!=64||packs[i].size_mb<=1)all_verified=false;
        if(strstr(packs[i].id,"tiny")||strstr(packs[i].id,"base")||strstr(packs[i].id,"phrase"))no_tiny=false;
        CHECK_MSG(!packs[i].installed,"catalog %s is not installed on fresh state",packs[i].id);
    }
    CHECK_MSG(np>=3&&asr&&mt&&lid,"catalog has ASR+MT+LID (%d entries)",np);
    CHECK(all_verified); CHECK(no_tiny);
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
    osa_paths_init();
    test_json_unicode();
    test_catalog_and_gate();
    test_store_active();
    test_long_history();
    test_audio_wav();
    test_worker_isolation();
    test_shell_gate();
    test_optional_real_models();
    printf("\n%d passed, %d failed\n",g_pass,g_fail);
    return g_fail?1:0;
}
