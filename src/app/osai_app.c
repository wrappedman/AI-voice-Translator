/* OpenSpeech AI application core.
 * Fresh installs contain no models. Downloads are verified but inactive;
 * activation loads real weights before atomically recording one active model
 * per kind. All expensive calls are synchronous here and must run off the UI
 * thread. No fixture, phrasebook or script heuristic is an inference fallback. */
#include "osai_app.h"
#include "../core/osai_platform.h"
#include "../core/osai_process.h"
#include "../engines/whisper_bridge.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT_LIMIT 65536
#define CHUNK_BYTES 2400
#define WORKER_TIMEOUT_MS 600000

typedef struct pack_def {
    const char *id, *name, *kind, *languages, *license, *url, *sha256, *notes, *ext;
    const char *langs;          /* capability list for translation models */
    int64_t bytes;
    int min_ram_mb;             /* approximate resident memory while running */
} pack_def;
/* Conservative, documented subset of the languages Qwen2.5 Instruct covers.
 * The list belongs to the model, not to the application: the active pack's own
 * capability list is what the pickers and the gate use. */
static const char QWEN_LANGS[]="en;fa;ar;zh;fr;es;pt;de;it;ru;ja;ko;vi;th;id;tr;hi;he;nl;pl;uk";
static const pack_def PACKS[] = {
 /* ---- speech: whisper.cpp ggml, MIT, multilingual (light first) ---- */
 {"whisper-tiny-q5","Whisper Tiny Q5_1","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-tiny-q5_1.bin",
  "818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7",
  "39M parameters; runs on very light PCs; lowest accuracy, good for a quick test.","bin",NULL,32152673,500},
 {"whisper-base-q5","Whisper Base Q5_1","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-base-q5_1.bin",
  "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898",
  "74M parameters; light and fast; usable for clear speech.","bin",NULL,59707625,600},
 {"whisper-small-q5","Whisper Small Q5_1","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small-q5_1.bin",
  "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb",
  "244M parameters; good balance for 4 GB machines.","bin",NULL,190085487,1000},
 {"whisper-medium-q5","Whisper Medium Q5_0","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-medium-q5_0.bin",
  "19fea4b380c3a618ec4723c3eef2eb785ffba0d0538cf43f8f235e7b3b34220f",
  "769M parameters; balanced quality/memory.","bin",NULL,539212467,1500},
 {"whisper-turbo-q5","Whisper Large v3 Turbo Q5_0","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo-q5_0.bin",
  "394221709cd5ad1f40c46e6031ca61bce88931e6e088c188294c6d5a55ffa7e2",
  "809M parameters; fast decoder, high accuracy.","bin",NULL,574041195,1600},
 {"whisper-medium-q8","Whisper Medium Q8_0","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-medium-q8_0.bin",
  "42a1ffcbe4167d224232443396968db4d02d4e8e87e213d3ee2e03095dea6502",
  "769M parameters at 8-bit; closer to the original weights.","bin",NULL,823369779,2000},
 {"whisper-turbo-q8","Whisper Large v3 Turbo Q8_0","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-turbo-q8_0.bin",
  "317eb69c11673c9de1e1f0d459b253999804ec71ac4c23c17ecf5fbe24e259a1",
  "809M parameters at 8-bit; best turbo quality.","bin",NULL,874188075,2200},
 {"whisper-large-v3-q5","Whisper Large v3 Q5_0","asr","multilingual","MIT",
  "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/ggml-large-v3-q5_0.bin",
  "d75795ecff3f83b5faa89d1900604ad8c780abd5739fae406de19f23ecd98ad1",
  "1550M parameters; highest accuracy, slower.","bin",NULL,1081140203,3000},
 /* ---- translation: official Qwen GGUF, Apache-2.0 (light first) ---- */
 {"qwen25-0.5b-q4km","Qwen2.5 0.5B Instruct Q4_K_M","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/9217f5db79a29953eb74d5343926648285ec7e67/qwen2.5-0.5b-instruct-q4_k_m.gguf",
  "74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db",
  "0.5B; the lightest translator, runs on 2 GB machines; simple wording.","gguf",QWEN_LANGS,491400032,1000},
 {"qwen25-0.5b-q8","Qwen2.5 0.5B Instruct Q8_0","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/9217f5db79a29953eb74d5343926648285ec7e67/qwen2.5-0.5b-instruct-q8_0.gguf",
  "ca59ca7f13d0e15a8cfa77bd17e65d24f6844b554a7b6c12e07a5f89ff76844e",
  "0.5B at 8-bit; still light, cleaner output than Q4.","gguf",QWEN_LANGS,675710816,1200},
 {"qwen25-1.5b-q4km","Qwen2.5 1.5B Instruct Q4_K_M","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf",
  "6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e",
  "1.5B neural translator for low-memory PCs.","gguf",QWEN_LANGS,1117320736,1800},
 {"qwen25-3b-q4km","Qwen2.5 3B Instruct Q4_K_M","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/cc1e68eea5f05f88f41a6de1fc73110178f23715/qwen2.5-3b-instruct-q4_k_m.gguf",
  "626b4a6678b86442240e33df819e00132d3ba7dddfe1cdc4fbb18e0a9615c62d",
  "3.09B neural translator; balanced quality/memory.","gguf",QWEN_LANGS,2104932768,2800},
 {"qwen25-3b-q6k","Qwen2.5 3B Instruct Q6_K","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q6_k.gguf",
  "12da1a5d3fa6905111d8798b00ed49e0f6425441598c8c41bb37a2c36d49d0f3",
  "3.09B at 6-bit; noticeably fewer quantization artefacts.","gguf",QWEN_LANGS,2793410976,3500},
 {"qwen25-3b-q8","Qwen2.5 3B Instruct Q8_0","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q8_0.gguf",
  "6dcc22694c8654b045ec40bbe350212b88893fd9010e8474bae5b19a43578ba1",
  "3.09B at 8-bit; near-original quality.","gguf",QWEN_LANGS,3616088480,4300},
 {"qwen25-7b-q3km","Qwen2.5 7B Instruct Q3_K_M","mt","multilingual","Apache-2.0",
  "https://huggingface.co/Qwen/Qwen2.5-7B-Instruct-GGUF/resolve/main/qwen2.5-7b-instruct-q3_k_m.gguf",
  "a96b16179dc6cc9afdf0cf7a96a80c199cbd00b9be207c3465be21cb721cca5e",
  "7.6B neural translator; strongest wording, slower.","gguf",QWEN_LANGS,3808391072,4800},
 /* ---- language identification: fastText LID 176 and GlotLID v4 ---- */
 {"fasttext-lid176-ftz","fastText LID 176 (compressed)","lang","multilingual","CC-BY-SA-3.0",
  "https://dl.fbaipublicfiles.com/fasttext/supervised-models/lid.176.ftz",
  "8f3472cfe8738a7b6099e8e999c3cbfae0dcd15696aac7d7738a8039db603e83",
  "176 languages, quantized; under 1 MB, the lightest option.","ftz",NULL,938013,300},
 {"fasttext-lid176","fastText LID 176 (full)","lang","multilingual","CC-BY-SA-3.0",
  "https://dl.fbaipublicfiles.com/fasttext/supervised-models/lid.176.bin",
  "7e69ec5451bc261cc7844e49e4792a85d7f09c06789ec800fc4a44aec362764e",
  "176 languages, full precision; needs about 1.4 GB while loading.","bin",NULL,131266198,1400},
 {"glotlid-small-ftz","GlotLID v4 small (compressed)","lang","multilingual","Apache-2.0+notices",
  "https://huggingface.co/cis-lmu/glotlid/resolve/e9592e0a9bc3b6f2a946de94faaa188d1dd6f522/model_v4_small.ftz",
  "27fa708eac9680a14e51ead8feb48e7d1b29db9af3d4f9038f50aa0781cd1d85",
  "2000+ language varieties, compact model; light and much wider coverage.","ftz",NULL,35345340,400},
 {"glotlid-small","GlotLID v4 small (full)","lang","multilingual","Apache-2.0+notices",
  "https://huggingface.co/cis-lmu/glotlid/resolve/e9592e0a9bc3b6f2a946de94faaa188d1dd6f522/model_v4_small.bin",
  "7fdfc49052d42653a1b8db5fbb0754497d670a6a4fae32398692f8e5b3811bdd",
  "2000+ varieties, full precision compact model; better accuracy than its .ftz.","bin",NULL,130969887,900},
 {"glotlid-ftz","GlotLID v4 (compressed)","lang","multilingual","Apache-2.0+notices",
  "https://huggingface.co/cis-lmu/glotlid/resolve/e9592e0a9bc3b6f2a946de94faaa188d1dd6f522/model_v4.ftz",
  "42039e8e9d18a1f75ce334c8aa0b47e8de4b74b887538f0bc7780ab9ae52eab3",
  "2000+ varieties, quantized full model; wide coverage at moderate memory.","ftz",NULL,101204412,700},
 {"glotlid","GlotLID v4 (full)","lang","multilingual","Apache-2.0+notices",
  "https://huggingface.co/cis-lmu/glotlid/resolve/e9592e0a9bc3b6f2a946de94faaa188d1dd6f522/model.bin",
  "3b26297652e105cb63acde0fbf5aa228161528b519331e58a7c94ea9fe58474c",
  "2000+ varieties, highest accuracy; needs several GB while loading.","bin",NULL,782808607,3000},
};
#define NPACKS ((int)(sizeof PACKS/sizeof PACKS[0]))

struct osa_app {
    osa_db *db; bool db_ready;
    osa_mutex db_mu, engine_mu, life_mu;
    wsp_ctx *asr; char asr_id[40], asr_name[128];
    osa_process *mt, *lid;
    char mt_id[40], mt_name[128], mt_path[OSAI_MAX_PATH];
    char mt_langs[512];         /* capability list of the active MT model */
    char lid_id[40], lid_name[128], lid_path[OSAI_MAX_PATH];
    bool mt_loaded, lid_loaded;
    char models_dir[OSAI_MAX_PATH], audio_dir[OSAI_MAX_PATH];
    char last_language[8], lang_src[8], lang_dst[8];
    mt_style style; volatile int cancel;
};

static const pack_def *find_pack(const char *id){if(!id)return NULL;for(int i=0;i<NPACKS;i++)if(osa_streq(id,PACKS[i].id))return PACKS+i;return NULL;}
static void json_str(osa_buf *b,const char *s){osa_arena a;osa_arena_init(&a);char*q=osa_json_escape(&a,s);osa_buf_puts(b,q);osa_arena_free(&a);}
static bool has_token(const char *list,const char *code){if(!code||!*code||!list)return false;size_t n=strlen(code);for(const char*p=list;*p;){const char*e=strchr(p,';');if(!e)e=p+strlen(p);if((size_t)(e-p)==n&&!strncmp(p,code,n))return true;p=*e?e+1:e;}return false;}
static const char *mt_langs(osa_app*a);
static void lockd(osa_app*a){osa_mutex_lock(&a->db_mu);} static void unlockd(osa_app*a){osa_mutex_unlock(&a->db_mu);}
static void locke(osa_app*a){osa_mutex_lock(&a->engine_mu);} static void unlocke(osa_app*a){osa_mutex_unlock(&a->engine_mu);}

static const char *mt_langs(osa_app*a){return a&&a->mt_langs[0]?a->mt_langs:QWEN_LANGS;}
static bool get_model(osa_app*a,const char*id,osa_model*m,osa_error*err){
    memset(m,0,sizeof*m);osa_arena ar;osa_arena_init(&ar);char *sql=osa_arena_printf(&ar,"SELECT * FROM Models WHERE id='%s'",osa_sql_escape(&ar,id));osa_rs rs;lockd(a);bool ok=osa_sql_query(a->db,sql,&rs,err);
    if(ok&&rs.nrows){const char*fields[]={"id","name","kind","engine","base_model","source_url","license","sha256","path","version","status","notes","created_at"};char*dst[]={m->id,m->name,m->kind,m->engine,m->base_model,m->source_url,m->license,m->sha256,m->path,m->version,m->status,m->notes,m->created_at};size_t caps[]={sizeof m->id,sizeof m->name,sizeof m->kind,sizeof m->engine,sizeof m->base_model,sizeof m->source_url,sizeof m->license,sizeof m->sha256,sizeof m->path,sizeof m->version,sizeof m->status,sizeof m->notes,sizeof m->created_at};for(int i=0;i<13;i++)osa_strlcpy(dst[i],osa_rs_val(&rs,0,fields[i]),caps[i]);m->size_bytes=osa_rs_i64(&rs,0,"size_bytes");}else if(ok){osa_err_set(err,"model is not installed");ok=false;}
    osa_rs_free(&rs);unlockd(a);osa_arena_free(&ar);return ok;
}
static bool verify(const char*path,const pack_def*p,osa_error*err){char h[65];uint64_t n=0;osa_sha256_file(path,h,&n);if(!osa_file_exists(path)||(int64_t)n!=p->bytes||!osa_strieq(h,p->sha256)){osa_err_set(err,"model integrity check failed (size or SHA-256 mismatch)");return false;}return true;}
static bool register_pack(osa_app*a,const pack_def*p,const char*path,osa_error*err){osa_model m;memset(&m,0,sizeof m);osa_strlcpy(m.id,p->id,sizeof m.id);osa_strlcpy(m.name,p->name,sizeof m.name);osa_strlcpy(m.kind,p->kind,sizeof m.kind);osa_strlcpy(m.engine,osa_streq(p->kind,"asr")?"whisper":osa_streq(p->kind,"mt")?"llama.cpp":"fastText",sizeof m.engine);osa_strlcpy(m.license,p->license,sizeof m.license);osa_strlcpy(m.source_url,p->url,sizeof m.source_url);osa_strlcpy(m.path,path,sizeof m.path);osa_strlcpy(m.version,"verified",sizeof m.version);osa_strlcpy(m.status,"installed",sizeof m.status);osa_strlcpy(m.sha256,p->sha256,sizeof m.sha256);m.size_bytes=p->bytes;osa_iso_time(osa_now_ms(),m.created_at);if(osa_streq(p->kind,"mt")&&p->langs)snprintf(m.notes,sizeof m.notes,"languages=%s",p->langs);else osa_strlcpy(m.notes,p->notes,sizeof m.notes);lockd(a);bool ok=osa_store_model_add(a->db,&m,err);unlockd(a);return ok;}

static char *worker_reply(osa_process*p,const char*request,volatile int*cancel,osa_error*err){
    if(!p||!osa_process_write(p,request,err))return NULL;
    char*line=NULL;
    if(!osa_process_line(p,WORKER_TIMEOUT_MS,cancel,&line,err))return NULL;
    bool ok=false;osa_json_get_bool(line,"ok",&ok);if(!ok){char msg[512];if(!osa_json_get_string(line,"error",msg,sizeof msg))osa_strlcpy(msg,"model worker failed",sizeof msg);osa_err_set(err,"%s",msg);free(line);return NULL;}return line;
}
/* Rough resident-memory estimate for a pack, used only to explain a failure. */
static int estimate_ram_mb(const pack_def*p){
    return p&&p->min_ram_mb>0?p->min_ram_mb:1024;
}
static int free_ram_mb(void){
    osa_sys_stats st;memset(&st,0,sizeof st);osa_sys_stats_sample(&st);
    if(st.ram_total_mb<=0)return -1;
    int free_mb=(int)(st.ram_total_mb-st.ram_used_mb);return free_mb>0?free_mb:0;
}
/* Starts the worker and waits for its ready event. Every failure path keeps the
 * worker's own sentence: "model did not become ready" told the user nothing,
 * while the worker always reports why (bad file, unsupported GGUF, bad_alloc).
 * The attempt and the outcome are also appended to <data>/Logs/worker.log. */
static osa_process *start_worker(const char*kind,const char*path,volatile int*cancel,osa_error*err){
    char exe[OSAI_MAX_PATH];
    /* Say which precondition failed before a process is spawned. A worker that
     * cannot open its model exits with a generic sentence, and "the file is
     * missing" is the one cause the user can fix in a second. */
    if(!path||!*path){osa_err_set(err,"no model file is selected for the %s engine",osa_streq(kind,"mt")?"translation":"language-identification");return NULL;}
    if(!osa_file_exists(path)){
        char p[OSAI_MAX_PATH];osa_strlcpy(p,path,sizeof p);
        osa_worker_log_note("activate kind=%s: model file is missing: %s",kind,p);
        osa_err_set(err,"the model file is missing: %s — run Sync in Settings or download it again",p);
        return NULL;
    }
    if(!osa_worker_path(exe,sizeof exe,err))return NULL;
    char threads[8];snprintf(threads,sizeof threads,"%d",osa_clampi(osa_cpu_count()/2,1,8));
    const char*argv[]={exe,"--kind",kind,"--model",path,"--threads",threads,NULL};
    osa_worker_log_note("activate kind=%s threads=%s model=%s",kind,threads,path);
    osa_process*p=osa_process_start(argv,err);
    if(!p){osa_worker_log_note("spawn failed: %s",err&&err->msg[0]?err->msg:"?");return NULL;}
    char*line=NULL;
    if(!osa_process_line(p,WORKER_TIMEOUT_MS,cancel,&line,err)){
        char tail[512]={0};int code=-1;bool exited=osa_process_exited(p,&code);
        osa_worker_log_tail(tail,sizeof tail);
        char base[600];osa_strlcpy(base,err&&err->msg[0]?err->msg:"no reply from the model worker",sizeof base);
        osa_process_stop(p);
        if(tail[0])osa_err_set(err,"%s — worker log: %s",base,tail);
        else if(exited)osa_err_set(err,"%s — the model worker exited with code %d",base,code);
        else osa_err_set(err,"%s",base);
        osa_worker_log_note("handshake failed: %s",err&&err->msg[0]?err->msg:"?");
        return NULL;
    }
    bool ok=false;char event[32]={0};char msg[OSAI_MAX_MSG]={0};
    osa_json_get_bool(line,"ok",&ok);osa_json_get_string(line,"event",event,sizeof event);
    osa_json_get_string(line,"error",msg,sizeof msg);
    free(line);
    if(!ok||!osa_streq(event,"ready")){
        int code=-1;bool exited=osa_process_exited(p,&code);
        osa_process_stop(p);
        if(!msg[0]){char tail[OSAI_MAX_MSG]={0};if(osa_worker_log_tail(tail,sizeof tail))osa_strlcpy(msg,tail,sizeof msg);}
        if(!msg[0])snprintf(msg,sizeof msg,"unexpected reply from the model worker (exit code %d)",exited?code:-1);
        const char*label=osa_streq(kind,"mt")?"translation":"language-identification";
        osa_err_set(err,"the %s model could not be loaded: %s",label,msg);
        osa_worker_log_note("load failed: %s",msg);
        return NULL;
    }
    osa_worker_log_note("ready kind=%s",kind);
    return p;
}
static bool ensure_worker(osa_app*a,const char*kind,osa_error*err){osa_process**slot=osa_streq(kind,"mt")?&a->mt:&a->lid;const char*path=osa_streq(kind,"mt")?a->mt_path:a->lid_path;if(*slot)return true;if(!path[0]){osa_err_set(err,"activate the required %s model first",kind);return false;}*slot=start_worker(kind,path,&a->cancel,err);return *slot!=NULL;}

/* Remove only legacy demo artifacts. User-downloaded GGUF/Whisper files and
 * history are never deleted by this migration. */
static void migrate_legacy(osa_app*a){
    lockd(a);osa_arena ar;osa_arena_init(&ar);char*v=osa_db_get_setting(a->db,"neural_models_v2",&ar,"");bool done=v&&osa_streq(v,"1");osa_arena_free(&ar);
    if(!done){osa_sql_exec(a->db,"DELETE FROM Models WHERE engine IN('exemplar','phrasebook','osmt','pack','wordpack') OR id LIKE 'builtin-%' OR id LIKE 'wordpack-%';",NULL);osa_db_set_setting(a->db,"neural_models_v2","1");}
    osa_sql_exec(a->db,"UPDATE Models SET active=0 WHERE engine NOT IN('whisper','llama.cpp','fastText');",NULL);unlockd(a);
    static const char*old[]={"builtin-asr-fa.osam","builtin-mt.osmt"};for(int i=0;i<2;i++){char p[OSAI_MAX_PATH];osa_path_join(p,sizeof p,a->models_dir,old[i]);if(osa_file_exists(p))osa_file_remove(p);}
}
osa_app *osa_app_create(osa_error*err){
    osa_err_ok(err);if(!osa_paths_init()){osa_err_set(err,"cannot write to the data folder beside the program; move the app to a writable folder (for example your Desktop) and run it again");return NULL;}
    osa_app*a=calloc(1,sizeof*a);if(!a){osa_err_set(err,"out of memory");return NULL;}osa_mutex_init(&a->db_mu);osa_mutex_init(&a->engine_mu);osa_mutex_init(&a->life_mu);
    osa_strlcpy(a->models_dir,osa_path(OSAI_DIR_MODELS),sizeof a->models_dir);osa_strlcpy(a->audio_dir,osa_path(OSAI_DIR_AUDIO),sizeof a->audio_dir);
    char dbp[OSAI_MAX_PATH];osa_path_join(dbp,sizeof dbp,osa_path(OSAI_DIR_DB),"openspeech.db");a->db=osa_db_open(dbp,err);if(!a->db){osa_mutex_destroy(&a->db_mu);osa_mutex_destroy(&a->engine_mu);osa_mutex_destroy(&a->life_mu);free(a);return NULL;}a->db_ready=true;migrate_legacy(a);
    /* The models folder is a persisted choice; it must be read after the
     * database is open, and only honoured when it still exists. */
    {osa_arena ar2;osa_arena_init(&ar2);char*md=osa_db_get_setting(a->db,"models_dir",&ar2,"");
     if(md&&*md&&osa_dir_exists(md)){osa_strlcpy(a->models_dir,md,sizeof a->models_dir);
         size_t n=strlen(a->models_dir);if(n&&a->models_dir[n-1]!=OSAI_PATH_SEP[0]&&n+1<sizeof a->models_dir){a->models_dir[n]=OSAI_PATH_SEP[0];a->models_dir[n+1]=0;}}
     osa_arena_free(&ar2);}
    lockd(a);osa_arena ar;osa_arena_init(&ar);char*s=osa_db_get_setting(a->db,"lang_src",&ar,"auto");char*d=osa_db_get_setting(a->db,"lang_dst",&ar,"fa");char*st=osa_db_get_setting(a->db,"mt_style",&ar,"0");osa_strlcpy(a->lang_src,s,sizeof a->lang_src);osa_strlcpy(a->lang_dst,d,sizeof a->lang_dst);a->style=(mt_style)osa_clampi(atoi(st),0,2);osa_arena_free(&ar);unlockd(a);return a;
}
void osa_app_destroy(osa_app*a){if(!a)return;osa_atomic_inc(&a->cancel);osa_mutex_lock(&a->life_mu);locke(a);if(a->asr){wsp_free(a->asr);a->asr=NULL;}osa_process*mt=a->mt,*lid=a->lid;a->mt=a->lid=NULL;unlocke(a);osa_process_stop(mt);osa_process_stop(lid);lockd(a);osa_db_close(a->db);a->db=NULL;unlockd(a);osa_mutex_unlock(&a->life_mu);osa_mutex_destroy(&a->life_mu);osa_mutex_destroy(&a->engine_mu);osa_mutex_destroy(&a->db_mu);free(a);}
void osa_app_cancel(osa_app*a){if(a)osa_atomic_inc(&a->cancel);} void osa_app_reset_cancel(osa_app*a){if(a)__sync_lock_test_and_set(&a->cancel,0);}
bool osa_app_ready(osa_app*a){if(!a)return false;locke(a);bool ok=a->asr&&a->mt_loaded&&a->lid_loaded;unlocke(a);return ok;}
bool osa_app_refresh(osa_app*a,osa_error*err){(void)a;osa_err_ok(err);return true;}

/* The recorded path can go stale: the user moves the models folder, restores a
 * backup, or an older profile is migrated next to the exe. Instead of failing
 * with "missing", look for the same pinned file in the places it can legitimately
 * live, verify it, and repair the database row. */
static bool relocate_model(osa_app*a,const pack_def*p,osa_model*m){
    const char*bases[3];
    bases[0]=a->models_dir;bases[1]=osa_path(OSAI_DIR_MODELS);bases[2]=osa_path(OSAI_DIR_DOWNLOADS);
    for(int i=0;i<3;i++){
        char cand[OSAI_MAX_PATH];
        snprintf(cand,sizeof cand,"%s%s.%s",bases[i],p->id,p->ext);
        if(osa_streq(cand,m->path)||!osa_file_exists(cand))continue;
        if(osa_file_size(cand)!=p->bytes)continue;
        char hex[65]={0};osa_sha256_file(cand,hex,NULL);
        if(!osa_strieq(hex,p->sha256))continue;
        osa_arena ar;osa_arena_init(&ar);
        char*sql=osa_arena_printf(&ar,"UPDATE Models SET path='%s' WHERE id='%s'",
                                  osa_sql_escape(&ar,cand),osa_sql_escape(&ar,p->id));
        lockd(a);osa_sql_exec(a->db,sql,NULL);unlockd(a);osa_arena_free(&ar);
        osa_worker_log_note("model %s was found at %s; database path repaired",p->id,cand);
        osa_strlcpy(m->path,cand,sizeof m->path);
        return true;
    }
    return false;
}
static bool activate(osa_app*a,const char*id,osa_error*err){
    osa_model m;if(!get_model(a,id,&m,err))return false;
    const pack_def*p=find_pack(id);
    if(!p){osa_err_set(err,"this model is not in the verified catalogue (id %s)",id);return false;}
    if(!osa_streq(p->kind,m.kind)){osa_err_set(err,"model category mismatch for %s",id);return false;}
    /* Say exactly which check failed, and where the file was expected. The old
     * combined sentence made a moved file, a truncated download and an unknown
     * id look identical. */
    if(!osa_file_exists(m.path)&&!relocate_model(a,p,&m)){
        osa_err_set(err,"the model file is missing: %s — run Sync in Settings, or download it again",m.path);
        return false;
    }
    if(!verify(m.path,p,NULL)){
        int64_t have_bytes=osa_file_size(m.path);char hex[65]={0};
        osa_sha256_file(m.path,hex,NULL);
        if(have_bytes!=p->bytes)
            osa_err_set(err,"the model file has the wrong size (%lld bytes, expected %lld): %s",
                        (long long)have_bytes,(long long)p->bytes,m.path);
        else
            osa_err_set(err,"the model file failed its SHA-256 check (integrity): %s",m.path);
        osa_worker_log_note("verify failed for %s: size=%lld expected=%lld sha=%s",
                            m.path,(long long)have_bytes,(long long)p->bytes,hex);
        return false;
    }
    if(osa_streq(m.kind,"asr")){char e[512]={0};wsp_ctx*n=wsp_load(m.path,e,sizeof e);if(!n){osa_err_set(err,"%s",e);return false;}wsp_set_cancel(n,&a->cancel);lockd(a);bool ok=osa_store_model_set_active(a->db,id,err);unlockd(a);if(!ok){wsp_free(n);return false;}locke(a);wsp_ctx*old=a->asr;a->asr=n;osa_strlcpy(a->asr_id,id,sizeof a->asr_id);osa_strlcpy(a->asr_name,m.name,sizeof a->asr_name);unlocke(a);wsp_free(old);return true;}
    /* Release the previous engine of this kind *before* loading the new one:
     * two resident translation models is exactly what a 8 GB PC cannot do. */
    locke(a);
    osa_process*old=osa_streq(m.kind,"mt")?a->mt:a->lid;
    if(osa_streq(m.kind,"mt")){a->mt=NULL;a->mt_loaded=false;}else{a->lid=NULL;a->lid_loaded=false;}
    unlocke(a);
    osa_process_stop(old);
    int need=estimate_ram_mb(p),have=free_ram_mb();
    if(have>=0&&have<need/2)osa_worker_log_note("warning: ~%d MB free, this model needs ~%d MB",have,need);
    osa_process*n=start_worker(m.kind,m.path,&a->cancel,err);
    if(!n){
        if(have>=0&&have<need){
            char base[600];osa_strlcpy(base,err&&err->msg[0]?err->msg:"load failed",sizeof base);
            osa_err_set(err,"%s [free RAM ~%d MB, this model needs ~%d MB — choose a smaller model]",base,have,need);
        }
        return false;
    }
    /* The handshake proves the file loads. Keep the engine out of memory while
     * it is idle so activating the next category still fits; ensure_worker()
     * reloads it on the first request. */
    osa_process_stop(n);
    lockd(a);bool ok=osa_store_model_set_active(a->db,id,err);unlockd(a);
    if(!ok)return false;
    locke(a);
    if(osa_streq(m.kind,"mt")){
        a->mt=NULL;a->mt_loaded=true;
        osa_strlcpy(a->mt_id,id,sizeof a->mt_id);osa_strlcpy(a->mt_name,m.name,sizeof a->mt_name);
        osa_strlcpy(a->mt_path,m.path,sizeof a->mt_path);
        osa_strlcpy(a->mt_langs,p->langs?p->langs:QWEN_LANGS,sizeof a->mt_langs);
    }else{
        a->lid=NULL;a->lid_loaded=true;
        osa_strlcpy(a->lid_id,id,sizeof a->lid_id);osa_strlcpy(a->lid_name,m.name,sizeof a->lid_name);
        osa_strlcpy(a->lid_path,m.path,sizeof a->lid_path);
    }
    unlocke(a);
    return true;
}
/* ---- delete a model ------------------------------------------------ */
bool osa_app_remove_model(osa_app*a,const char*id,osa_error*err){
    if(!a||!id||!*id){osa_err_set(err,"bad arguments");return false;}
    osa_mutex_lock(&a->life_mu);
    osa_model m;
    if(!get_model(a,id,&m,err)){osa_mutex_unlock(&a->life_mu);return false;}
    /* Unload first: an open file cannot be deleted on Windows. */
    locke(a);
    wsp_ctx*old_asr=NULL;osa_process*old_proc=NULL;
    if(osa_streq(m.kind,"asr")&&osa_streq(a->asr_id,id)){old_asr=a->asr;a->asr=NULL;a->asr_id[0]=a->asr_name[0]=0;}
    else if(osa_streq(m.kind,"mt")&&osa_streq(a->mt_id,id)){old_proc=a->mt;a->mt=NULL;a->mt_loaded=false;a->mt_id[0]=a->mt_name[0]=a->mt_path[0]=0;a->mt_langs[0]=0;}
    else if(osa_streq(m.kind,"lang")&&osa_streq(a->lid_id,id)){old_proc=a->lid;a->lid=NULL;a->lid_loaded=false;a->lid_id[0]=a->lid_name[0]=a->lid_path[0]=0;}
    unlocke(a);
    if(old_asr)wsp_free(old_asr);
    if(old_proc)osa_process_stop(old_proc);
    lockd(a);bool ok=osa_store_model_delete(a->db,id,err);unlockd(a);
    if(ok){
        if(m.path[0]&&osa_file_exists(m.path)&&!osa_file_remove(m.path))
            osa_worker_log_note("could not delete model file %s",m.path);
        const pack_def*p=find_pack(id);
        if(p){char part[OSAI_MAX_PATH];
            snprintf(part,sizeof part,"%s%s.%s",osa_path(OSAI_DIR_DOWNLOADS),p->id,p->ext);
            if(osa_file_exists(part))osa_file_remove(part);
            snprintf(part,sizeof part,"%s%s.%s.part",osa_path(OSAI_DIR_DOWNLOADS),p->id,p->ext);
            if(osa_file_exists(part))osa_file_remove(part);}
        osa_worker_log_note("removed model %s (%s)",id,m.path);
    }
    osa_mutex_unlock(&a->life_mu);
    return ok;
}

/* ---- models folder + synchronisation ------------------------------- */
void osa_app_models_dir(osa_app*a,char*out,size_t cap){
    if(!out||!cap)return;out[0]=0;
    if(!a){osa_strlcpy(out,osa_path(OSAI_DIR_MODELS),cap);return;}
    osa_strlcpy(out,a->models_dir,cap);
}
static void norm_dir(char*dir,size_t cap){
    size_t n=strlen(dir);
    while(n>1&&(dir[n-1]=='/'||dir[n-1]=='\\'))dir[--n]=0;
    if(n+1<cap){dir[n]=OSAI_PATH_SEP[0];dir[n+1]=0;}
}
bool osa_app_set_models_dir(osa_app*a,const char*dir,osa_error*err){
    if(!a||!dir||!*dir){osa_err_set(err,"bad arguments");return false;}
    char path[OSAI_MAX_PATH];osa_strlcpy(path,dir,sizeof path);norm_dir(path,sizeof path);
    if(!osa_mkdirs(path)||!osa_dir_exists(path)){osa_err_set(err,"the models folder cannot be created: %s",path);return false;}
    char probe[OSAI_MAX_PATH];snprintf(probe,sizeof probe,"%s.write-test",path);
    if(!osa_file_write(probe,"",0)){osa_err_set(err,"the models folder is not writable: %s",path);return false;}
    osa_file_remove(probe);
    osa_strlcpy(a->models_dir,path,sizeof a->models_dir);
    lockd(a);osa_db_set_setting(a->db,"models_dir",path);unlockd(a);
    osa_worker_log_note("models folder set to %s",path);
    return true;
}
/* Appends a name to a short comma-separated report without overflowing it. */
static void append_name(char*buf,size_t cap,const char*name){
    size_t n=strlen(buf),m=strlen(name);
    const char*sep=n?", ":"";size_t sl=n?2:0;
    if(n+sl+m+1>=cap)return;
    memcpy(buf+n,sep,sl);memcpy(buf+n+sl,name,m+1);
}
static bool model_ext(const char*name){
    return osa_ends(name,".gguf")||osa_ends(name,".bin")||osa_ends(name,".ftz");
}
/* Bridges the byte-level hash progress into the file-level progress the UI
 * already draws, so a 4 GB model shows movement instead of a frozen "0 / 6". */
/* Bridges the byte-level hash progress into the file-level progress the UI
 * already draws, so a 4 GB model shows movement instead of a frozen "0 / 6".
 * Progress is reported in bytes over the whole batch: it never jumps back to
 * zero when the next file starts counting from its own first byte. */
typedef struct {int64_t index,total;void(*progress)(int64_t,int64_t,void*);void*ud;} sync_prog;
/* The UI reports "checked / found" in files, so `total` stays the file count and
 * `done` a file index. While one file is being read the count stays on that
 * file: it never jumps backwards when the next file starts, and it never shows
 * a half-file in a label that means files. */
static void sync_hash_progress(int64_t done,int64_t unused,void*ud){
    (void)done;(void)unused;
    sync_prog*p=ud;
    if(p->progress)p->progress(p->index,p->total,p->ud);
}
/* Reports which file is being read. Called once per file, not per chunk: the
 * UI samples it every frame, so a per-chunk callback would be wasted work. */
static void sync_note_current(osa_sync_result*out,const char*name){
    if(!out)return;
    osa_strlcpy(out->current,name?name:"",sizeof out->current);
}
static bool sync_cancelled(volatile int*cancel){return cancel&&osa_atomic_load(cancel);}
bool osa_app_sync_models(osa_app*a,volatile int*cancel,
                         void(*progress)(int64_t,int64_t,void*),void*ud,
                         osa_sync_result*out,osa_error*err){
    if(!a||!out){osa_err_set(err,"bad arguments");return false;}
    memset(out,0,sizeof*out);
    char dir[OSAI_MAX_PATH];osa_app_models_dir(a,dir,sizeof dir);
    if(!osa_dir_exists(dir)){osa_err_set(err,"the models folder does not exist: %s",dir);return false;}
    char**names=NULL;int n=osa_dir_scan(dir,NULL,&names,4096);
    /* Only model-shaped files are hashed; anything else is ignored silently. */
    int*keep=calloc((size_t)(n>0?n:1),sizeof(int));int total=0;
    if(!keep){osa_dir_free_list(names,n);osa_err_set(err,"out of memory");return false;}
    for(int i=0;i<n;i++){
        char full[OSAI_MAX_PATH];osa_path_join(full,sizeof full,dir,names[i]);
        if(model_ext(names[i])&&!osa_ends(names[i],".part")&&osa_file_size(full)>65536)keep[total++]=i;
    }
    sync_prog sp;memset(&sp,0,sizeof sp);
    sp.total=total;sp.progress=progress;sp.ud=ud;
    bool ok=true;
    for(int k=0;k<total;k++){
        if(sync_cancelled(cancel)){ok=false;osa_err_set(err,"cancelled");break;}
        sp.index=k;
        if(progress)progress(k,total,ud);
        const char*name=names[keep[k]];
        char full[OSAI_MAX_PATH];osa_path_join(full,sizeof full,dir,name);
        out->scanned++;
        int64_t size=osa_file_size(full);
        const pack_def*match=NULL;
        /* Size first: it rejects almost everything without reading a byte. */
        for(int i=0;i<NPACKS;i++)if(PACKS[i].bytes==size){match=PACKS+i;break;}
        if(!match){out->unknown++;append_name(out->unknown_names,sizeof out->unknown_names,name);continue;}
        char hex[65]={0};uint64_t got=0;
        sync_note_current(out,name);
        /* Cancellable: closing the app during Sync must not block on a
         * multi-gigabyte read of a model the user is not installing. */
        bool hashed=osa_sha256_file_ex(full,hex,&got,cancel,sync_hash_progress,&sp);
        sync_note_current(out,NULL);
        if(!hashed){
            ok=false;
            if(sync_cancelled(cancel))osa_err_set(err,"cancelled");
            else osa_err_set(err,"cannot read %s",name);
            break;
        }
        if((int64_t)got!=match->bytes||!osa_strieq(hex,match->sha256)){
            out->unknown++;append_name(out->unknown_names,sizeof out->unknown_names,name);continue;}
        osa_model existing;osa_error ignored;osa_err_ok(&ignored);
        if(get_model(a,match->id,&existing,&ignored)){out->existing++;continue;}
        if(register_pack(a,match,full,err)){out->added++;osa_worker_log_note("sync: registered %s from %s",match->id,full);}
        else{ok=false;break;}
    }
    if(progress)progress(total,total,ud);
    free(keep);osa_dir_free_list(names,n);
    return ok;
}

bool osa_app_activate_model(osa_app*a,const char*id,osa_error*err){if(!a||!id){osa_err_set(err,"bad arguments");return false;}osa_mutex_lock(&a->life_mu);bool ok=activate(a,id,err);osa_mutex_unlock(&a->life_mu);return ok;}
bool osa_app_restore_saved(osa_app*a,osa_error*err){if(!a)return false;osa_err_ok(err);osa_mutex_lock(&a->life_mu);osa_rs rs;lockd(a);bool ok=osa_sql_query(a->db,"SELECT id FROM Models WHERE active=1 ORDER BY CASE kind WHEN 'lang' THEN 0 WHEN 'mt' THEN 1 ELSE 2 END",&rs,err);char ids[3][40]={{0}};int n=ok?osa_clampi(rs.nrows,0,3):0;for(int i=0;i<n;i++)osa_strlcpy(ids[i],osa_rs_val(&rs,i,"id"),40);osa_rs_free(&rs);unlockd(a);for(int i=0;i<n;i++){osa_error e={0};if(!activate(a,ids[i],&e)){osa_err_set(err,"%s",e.msg);ok=false;}}osa_mutex_unlock(&a->life_mu);return ok;}

void osa_app_get_status(osa_app*a,osa_app_status*out){if(!a||!out)return;memset(out,0,sizeof*out);locke(a);out->asr_ready=a->asr!=NULL;out->mt_ready=a->mt_loaded;out->lid_ready=a->lid_loaded;osa_strlcpy(out->asr_model,a->asr_name,sizeof out->asr_model);osa_strlcpy(out->mt_model,a->mt_name,sizeof out->mt_model);osa_strlcpy(out->lid_model,a->lid_name,sizeof out->lid_model);osa_strlcpy(out->asr_backend,a->asr?"whisper.cpp":"",sizeof out->asr_backend);osa_strlcpy(out->mt_backend,a->mt_loaded?"llama.cpp CPU":"",sizeof out->mt_backend);osa_strlcpy(out->lid_backend,a->lid_loaded?"fastText":"",sizeof out->lid_backend);unlocke(a);out->db_ready=a->db_ready;osa_strlcpy(out->data_root,osa_path(OSAI_DIR_ROOT),sizeof out->data_root);lockd(a);osa_rs rs;if(osa_sql_query(a->db,"SELECT (SELECT COUNT(*) FROM Models) AS m,(SELECT COUNT(*) FROM Transcriptions) AS tr,(SELECT COUNT(*) FROM Translations) AS mt",&rs,NULL)){out->n_models=osa_rs_int(&rs,0,"m");out->n_transcripts=osa_rs_int(&rs,0,"tr");out->n_translations=osa_rs_int(&rs,0,"mt");osa_rs_free(&rs);}unlockd(a);osa_sys_stats st;memset(&st,0,sizeof st);osa_sys_stats_sample(&st);out->cpu_percent=st.cpu_percent;out->ram_used_mb=st.ram_used_mb;out->app_ram_mb=st.proc_ram_mb;out->disk_free_mb=(double)osa_disk_free(osa_path(OSAI_DIR_ROOT))/1048576.0;}

/* ---- languages: capabilities come from the loaded models, not a UI enum ---- */
static const struct{const char*c,*fa,*en;} LANG_NAMES[]={
 {"fa","فارسی","Persian"},{"en","انگلیسی","English"},{"ar","عربی","Arabic"},{"zh","چینی","Chinese"},{"fr","فرانسوی","French"},{"de","آلمانی","German"},{"es","اسپانیایی","Spanish"},{"it","ایتالیایی","Italian"},{"pt","پرتغالی","Portuguese"},{"ru","روسی","Russian"},{"ja","ژاپنی","Japanese"},{"ko","کره‌ای","Korean"},{"tr","ترکی","Turkish"},{"hi","هندی","Hindi"},{"ur","اردو","Urdu"},{"id","اندونزیایی","Indonesian"},{"vi","ویتنامی","Vietnamese"},{"th","تایلندی","Thai"},{"nl","هلندی","Dutch"},{"pl","لهستانی","Polish"},{"uk","اوکراینی","Ukrainian"},{"he","عبری","Hebrew"},{"sv","سوئدی","Swedish"},{"da","دانمارکی","Danish"},{"fi","فنلاندی","Finnish"},{"no","نروژی","Norwegian"},{"el","یونانی","Greek"},{"cs","چکی","Czech"},{"hu","مجاری","Hungarian"},{"ro","رومانیایی","Romanian"},{"bg","بلغاری","Bulgarian"},{"bn","بنگالی","Bengali"},{"ta","تامیلی","Tamil"},{"te","تلوگویی","Telugu"},{"mr","مراتی","Marathi"},{"gu","گجراتی","Gujarati"},{"pa","پنجابی","Punjabi"},{"ml","مالایالامی","Malayalam"},{"kn","کانادایی","Kannada"},{"si","سینهالی","Sinhala"},{"ne","نپالی","Nepali"},{"ms","مالایی","Malay"},{"tl","تاگالوگ","Tagalog"},{"sw","سواحیلی","Swahili"},{"af","آفریکانس","Afrikaans"},{"az","آذربایجانی","Azerbaijani"},{"kk","قزاقی","Kazakh"},{"uz","ازبکی","Uzbek"},{"hy","ارمنی","Armenian"},{"ka","گرجی","Georgian"},{"et","استونیایی","Estonian"},{"lv","لتونیایی","Latvian"},{"lt","لیتوانیایی","Lithuanian"},{"hr","کرواتی","Croatian"},{"sr","صربی","Serbian"},{"sk","اسلواکی","Slovak"},{"sl","اسلوونیایی","Slovenian"},{"is","ایسلندی","Icelandic"},{"ga","ایرلندی","Irish"},{"cy","ولزی","Welsh"},{"ca","کاتالان","Catalan"},{"eu","باسکی","Basque"},{"gl","گالیسی","Galician"},{"la","لاتین","Latin"},{"yi","ییدی","Yiddish"},{"ps","پشتو","Pashto"},{"ku","کردی","Kurdish"},{"sd","سندی","Sindhi"},{"my","برمه‌ای","Burmese"},{"km","خمر","Khmer"},{"lo","لائو","Lao"},{"bo","تبتی","Tibetan"},{"mn","مغولی","Mongolian"},{"am","امهری","Amharic"},{"so","سومالیایی","Somali"},{"ha","هوسایی","Hausa"},{"yo","یوروبایی","Yoruba"},{"zu","زولویی","Zulu"},{"be","بلاروسی","Belarusian"},{"mk","مقدونی","Macedonian"},{"sq","آلبانیایی","Albanian"},{"bs","بوسنیایی","Bosnian"},{"mt","مالتی","Maltese"},{"eo","اسپرانتو","Esperanto"},{"haw","هاوائی","Hawaiian"},{"mi","مائوری","Maori"},{"jw","جاوه‌ای","Javanese"},{"su","سوندایی","Sundanese"},{"sa","سانسکریت","Sanskrit"},{"oc","اکسیتان","Occitan"},{"br","برتون","Breton"},{"fo","فارویی","Faroese"},{"lb","لوکزامبورگی","Luxembourgish"},{"nn","نینوشک","Nynorsk"},{"tt","تاتاری","Tatar"},{"ba","باشقیری","Bashkir"},{"ky","قرقیزی","Kyrgyz"},{"tg","تاجیکی","Tajik"},{"tk","ترکمنی","Turkmen"}
};
const char*osa_lang_name(const char*c,bool persian){if(!c||!*c)return persian?"تشخیص خودکار":"Auto-detect";if(osa_streq(c,"auto"))return persian?"تشخیص خودکار":"Auto-detect";for(unsigned i=0;i<sizeof LANG_NAMES/sizeof LANG_NAMES[0];i++)if(osa_streq(c,LANG_NAMES[i].c))return persian?LANG_NAMES[i].fa:LANG_NAMES[i].en;return c;}
int osa_app_languages(osa_app*a,osa_lang_info*out,int max){if(!a||!out||max<=0)return 0;locke(a);bool ready=a->mt_loaded;char langs[512];osa_strlcpy(langs,a->mt_langs[0]?a->mt_langs:QWEN_LANGS,sizeof langs);unlocke(a);if(!ready)return 0;int n=0;for(const char*p=langs;*p&&n<max;){const char*e=strchr(p,';');if(!e)e=p+strlen(p);osa_lang_info*x=out+n++;memset(x,0,sizeof*x);osa_strlcpy(x->code,p,(size_t)(e-p)+1<8?(size_t)(e-p)+1:8);osa_strlcpy(x->name_fa,osa_lang_name(x->code,true),sizeof x->name_fa);osa_strlcpy(x->name_en,osa_lang_name(x->code,false),sizeof x->name_en);x->installed=true;x->paired=true;p=*e?e+1:e;}return n;}
int osa_app_asr_languages(osa_app*a,osa_lang_info*out,int max){if(!a||!out||max<=0)return 0;locke(a);const char*codes[128];int n=a->asr?wsp_languages(a->asr,codes,osa_clampi(max,0,128)):0;for(int i=0;i<n;i++){memset(out+i,0,sizeof*out);osa_strlcpy(out[i].code,codes[i],8);osa_strlcpy(out[i].name_fa,osa_lang_name(codes[i],true),48);osa_strlcpy(out[i].name_en,osa_lang_name(codes[i],false),48);out[i].installed=true;}unlocke(a);return n;}
bool osa_app_set_languages(osa_app*a,const char*src,const char*dst,osa_error*err){if(!a||!dst||!*dst||osa_streq(dst,"auto")){osa_err_set(err,"choose a target language");return false;}char s[8]={0},d[8]={0};osa_strlcpy(s,src&&*src?src:"auto",8);osa_strlcpy(d,dst,8);if(!osa_app_ready(a)){osa_err_set(err,"download and activate speech, translation and language models first");return false;}const char*langs=mt_langs(a);if(!has_token(langs,d)||(!osa_streq(s,"auto")&&!has_token(langs,s))){osa_err_set(err,"language is not supported by the active translation model");return false;}locke(a);osa_strlcpy(a->lang_src,s,8);osa_strlcpy(a->lang_dst,d,8);unlocke(a);lockd(a);osa_db_set_setting(a->db,"lang_src",s);osa_db_set_setting(a->db,"lang_dst",d);unlockd(a);osa_err_ok(err);return true;}
void osa_app_get_languages(osa_app*a,char*s,size_t sc,char*d,size_t dc){if(!a)return;locke(a);if(s&&sc)osa_strlcpy(s,a->lang_src,sc);if(d&&dc)osa_strlcpy(d,a->lang_dst,dc);unlocke(a);}
void osa_app_set_style(osa_app*a,mt_style st){if(!a)return;st=(mt_style)osa_clampi((int)st,0,2);locke(a);a->style=st;unlocke(a);lockd(a);char v[8];snprintf(v,sizeof v,"%d",(int)st);osa_db_set_setting(a->db,"mt_style",v);unlockd(a);} mt_style osa_app_style(const osa_app*a){return a?a->style:MT_STYLE_NEUTRAL;}

/* ---- real ASR ---- */
const char*osa_app_last_language(const osa_app*a){return a?a->last_language:"";}
static bool recognize(osa_app*a,const float*pcm,int64_t n,wsp_result*r,osa_error*err){if(!pcm||n<1600){osa_err_set(err,"not enough audio (at least 0.1 s)");return false;}locke(a);if(!a->asr){unlocke(a);osa_err_set(err,"download and activate a speech model first");return false;}char hint[8]={0};if(!osa_streq(a->lang_src,"auto"))osa_strlcpy(hint,a->lang_src,8);char e[512]={0};bool ok=wsp_transcribe(a->asr,pcm,n,hint,r,e,sizeof e);if(ok)osa_strlcpy(a->last_language,r->language,sizeof a->last_language);unlocke(a);if(!ok)osa_err_set(err,"%s",e);return ok;}
static bool persist_transcript(osa_app*a,const wsp_result*r,double dur,double ms,const char*audio,osa_app_transcript*out,osa_error*err){memset(out,0,sizeof*out);osa_uuid(out->id);osa_strlcpy(out->text,r->text,sizeof out->text);osa_strlcpy(out->language,r->language,sizeof out->language);osa_strlcpy(out->model_id,a->asr_id,sizeof out->model_id);out->confidence=r->confidence;out->duration_s=dur;out->proc_ms=ms;out->rtf=dur>0?ms/(dur*1000):0;for(char*p=out->text;*p;){while(*p==' '||*p=='\n'||*p=='\t')p++;if(!*p)break;out->n_words++;while(*p&&*p!=' '&&*p!='\n'&&*p!='\t')p++;}osa_transcription t;memset(&t,0,sizeof t);osa_strlcpy(t.id,out->id,sizeof t.id);osa_strlcpy(t.audio_path,audio?audio:"",sizeof t.audio_path);osa_strlcpy(t.model_id,out->model_id,sizeof t.model_id);osa_strlcpy(t.language,out->language,sizeof t.language);osa_strlcpy(t.source,audio&&*audio?"file":"microphone",sizeof t.source);osa_strlcpy(t.text,out->text,sizeof t.text);osa_strlcpy(t.status,"done",sizeof t.status);t.confidence=r->confidence;t.duration_ms=(int64_t)(dur*1000);t.proc_ms=(int64_t)ms;osa_iso_time(osa_now_ms(),t.created_at);lockd(a);bool ok=osa_store_transcription_add(a->db,&t,err);unlockd(a);return ok;}
bool osa_app_recognize(osa_app*a,const float*s,int64_t n,char*text,size_t tc,char*lang,size_t lc,float*conf,osa_error*err){if(!a)return false;wsp_result r;memset(&r,0,sizeof r);if(!recognize(a,s,n,&r,err))return false;if(text&&tc){if(strlen(r.text)>=tc){osa_err_set(err,"recognition output buffer too small");return false;}osa_strlcpy(text,r.text,tc);}if(lang&&lc)osa_strlcpy(lang,r.language,lc);if(conf)*conf=r.confidence;return true;}
bool osa_app_transcribe_samples(osa_app*a,const float*s,int64_t n,osa_app_transcript*out,osa_error*err){if(!a||!out)return false;int64_t t=osa_now_us();wsp_result r;memset(&r,0,sizeof r);if(!recognize(a,s,n,&r,err))return false;return persist_transcript(a,&r,(double)n/OSAI_SR,(double)(osa_now_us()-t)/1000,"",out,err);}
bool osa_app_transcribe_file(osa_app*a,const char*path,osa_app_transcript*out,osa_error*err){if(!a||!path||!out)return false;if(!osa_app_ready(a)){osa_err_set(err,"download and activate all three model types first");return false;}osa_audio au;if(!osa_audio_load(path,&au,err))return false;if(osa_atomic_load(&a->cancel)){osa_audio_free(&au);osa_err_set(err,"cancelled");return false;}int64_t t=osa_now_us();wsp_result r;memset(&r,0,sizeof r);bool ok=recognize(a,au.samples,au.n,&r,err);double ms=(double)(osa_now_us()-t)/1000,dur=osa_audio_duration_s(&au);if(ok){char dst[OSAI_MAX_PATH],name[64];osa_uuid(out->id);snprintf(name,sizeof name,"%s.wav",out->id);osa_path_join(dst,sizeof dst,a->audio_dir,name);if(!osa_audio_save_wav(dst,au.samples,au.n,OSAI_SR)){osa_err_set(err,"cannot save transcript audio");ok=false;}else ok=persist_transcript(a,&r,dur,ms,dst,out,err);if(ok){lockd(a);osa_store_audio_add(a->db,dst,name,(int64_t)(au.n*2),(int64_t)(dur*1000),"wav",out->language,NULL);unlockd(a);}}osa_audio_free(&au);return ok;}

/* ---- neural translation, complete UTF-8 chunks, no silent truncation ---- */
static size_t split_chunk(const char*s,size_t pos,char sep[3]){size_t len=strlen(s+pos);if(len<=CHUNK_BYTES){sep[0]=0;return len;}size_t cut=0;for(size_t i=0;i<=CHUNK_BYTES;i++){if(s[pos+i]=='\n'||s[pos+i]==' '||s[pos+i]=='\t')cut=i;}if(!cut){cut=CHUNK_BYTES;while(cut&&((unsigned char)s[pos+cut]&0xc0)==0x80)cut--;}char c=s[pos+cut];sep[0]=(c=='\n'||c==' '||c=='\t')?c:0;sep[1]=0;if(!sep[0])return cut;return cut;}
static bool detect(osa_app*a,const char*text,char code[8],float*conf,osa_error*err){if(!ensure_worker(a,"lang",err))return false;osa_arena ar;osa_arena_init(&ar);osa_buf req;osa_buf_init(&req);osa_buf_printf(&req,"{\"op\":\"detect\",\"text\":%s}",osa_json_escape(&ar,text));char*line=worker_reply(a->lid,req.data,&a->cancel,err);osa_buf_free(&req);osa_arena_free(&ar);if(!line){if(a->lid){osa_process_stop(a->lid);a->lid=NULL;}a->lid_loaded=false;return false;}bool ok=osa_json_get_string(line,"language",code,8);double c=0;osa_json_get_num(line,"confidence",&c);*conf=(float)c;free(line);if(!ok){osa_err_set(err,"language model returned no label");return false;}return true;}
static bool translate_all(osa_app*a,const char*text,const char*src,const char*dst,mt_style style,char*out,size_t cap,osa_error*err){
    if(!ensure_worker(a,"mt",err))return false;
    size_t pos=0,used=0;out[0]=0;char code[8]={0};float confidence=0;
    if(osa_streq(src,"auto")){if(!detect(a,text,code,&confidence,err))return false;if(confidence<0.35f){osa_err_set(err,"language detection is uncertain; choose the source language explicitly");return false;}if(!has_token(mt_langs(a),code)){osa_err_set(err,"detected language '%s' is not supported by the active translation model",code);return false;}src=code;}
    while(text[pos]){if(osa_atomic_load(&a->cancel)){osa_err_set(err,"cancelled");return false;}char sep[3]={0};size_t n=split_chunk(text,pos,sep);if(!n){pos++;continue;}char*chunk=osa_strndup(text+pos,n);if(!chunk){osa_err_set(err,"out of memory");return false;}osa_arena ar;osa_arena_init(&ar);osa_buf req;osa_buf_init(&req);osa_buf_printf(&req,"{\"op\":\"translate\",\"src\":%s,\"dst\":%s,\"style\":%d,\"max_tokens\":2048,\"text\":%s}",osa_json_escape(&ar,src),osa_json_escape(&ar,dst),(int)style,osa_json_escape(&ar,chunk));char*line=worker_reply(a->mt,req.data,&a->cancel,err);osa_buf_free(&req);osa_arena_free(&ar);free(chunk);if(!line){if(a->mt){osa_process_stop(a->mt);a->mt=NULL;}a->mt_loaded=false;return false;}char*part=osa_json_string_dup(line,"text");bool complete=false;osa_json_get_bool(line,"complete",&complete);free(line);if(!part||!complete){free(part);osa_err_set(err,"translation was incomplete; nothing was saved");return false;}size_t pn=strlen(part),sn=strlen(sep);if(used+pn+sn>=cap){free(part);osa_err_set(err,"translation exceeds 64 KiB; split the input");return false;}memcpy(out+used,part,pn);used+=pn;free(part);if(text[pos+n]&&sn){memcpy(out+used,sep,sn);used+=sn;}out[used]=0;pos+=n+(sep[0]?1:0);}
    return used>0;
}
static bool translate_core(osa_app*a,const char*text,const char*src,osa_app_translation*out,bool store,osa_error*err){if(!a||!text||!out){osa_err_set(err,"bad arguments");return false;}if(!osa_app_ready(a)){osa_err_set(err,"download and activate speech, translation and language models first");return false;}size_t len=strlen(text);if(!len||len>=TEXT_LIMIT||!osa_utf8_valid(text,len)){osa_err_set(err,"enter valid UTF-8 text under 64 KiB");return false;}locke(a);char s[8],d[8];osa_strlcpy(s,src&&*src?src:a->lang_src,8);osa_strlcpy(d,a->lang_dst,8);mt_style style=a->style;const char*alangs=a->mt_langs[0]?a->mt_langs:QWEN_LANGS;if(!has_token(alangs,d)||(!osa_streq(s,"auto")&&!has_token(alangs,s))){unlocke(a);osa_err_set(err,"language pair is not supported by the active translation model");return false;}memset(out,0,sizeof*out);int64_t t=osa_now_us();bool ok;if(osa_streq(s,d)){osa_strlcpy(out->text,text,sizeof out->text);osa_strlcpy(out->source_lang,s,8);out->confidence=1;ok=true;}else{
        if(osa_streq(s,"auto")){
            char detected[8]={0};float c=0;
            if(!detect(a,text,detected,&c,err)){unlocke(a);return false;}
            if(c<0.35f){unlocke(a);osa_err_set(err,"language detection is uncertain; choose the source language explicitly");return false;}
            if(!has_token(alangs,detected)){unlocke(a);osa_err_set(err,"detected language '%s' is not supported by the active translation model",detected);return false;}
            osa_strlcpy(s,detected,8);
        }
        ok=translate_all(a,text,s,d,style,out->text,sizeof out->text,err);
        if(ok){osa_strlcpy(out->source_lang,s,8);out->confidence=0;}
    }
    if(ok){osa_uuid(out->id);osa_strlcpy(out->target_lang,d,8);osa_strlcpy(out->engine,"llama.cpp",sizeof out->engine);out->elapsed_ms=(double)(osa_now_us()-t)/1000;osa_strlcpy(out->engine,a->mt_id,sizeof out->engine);}unlocke(a);if(!ok)return false;osa_translation row;memset(&row,0,sizeof row);osa_strlcpy(row.id,out->id,sizeof row.id);osa_strlcpy(row.source_lang,out->source_lang,sizeof row.source_lang);osa_strlcpy(row.target_lang,d,sizeof row.target_lang);osa_strlcpy(row.engine,out->engine,sizeof row.engine);osa_strlcpy(row.text,out->text,sizeof row.text);osa_strlcpy(row.source_text,text,sizeof row.source_text);osa_iso_time(osa_now_ms(),row.created_at);if(!store)return true;lockd(a);bool saved=osa_store_translation_add(a->db,&row,err);unlockd(a);return saved;
}
bool osa_app_translate(osa_app*a,const char*t,const char*s,osa_app_translation*o,osa_error*e){return translate_core(a,t,s,o,true,e);}
bool osa_app_translate_live(osa_app*a,const char*t,const char*s,osa_app_translation*o,osa_error*e){return translate_core(a,t,s,o,false,e);}
bool osa_app_transcribe_and_translate(osa_app*a,const char*path,osa_app_transcript*tr,osa_app_translation*mt,osa_error*err){if(!osa_app_transcribe_file(a,path,tr,err))return false;if(!tr->text[0]){osa_err_set(err,"no speech recognised");return false;}if(!osa_app_translate(a,tr->text,tr->language,mt,err))return false;osa_arena ar;osa_arena_init(&ar);char*sql=osa_arena_printf(&ar,"UPDATE Translations SET transcription_id='%s' WHERE id='%s'",osa_sql_escape(&ar,tr->id),osa_sql_escape(&ar,mt->id));lockd(a);bool ok=osa_sql_exec(a->db,sql,err);unlockd(a);osa_arena_free(&ar);return ok;}
const char*osa_app_translate_preview(osa_app*a,const char*t){(void)a;(void)t;return "";} /* history renders stored output, never inference while painting */
bool osa_app_pronounce(const char*t,const char*l,char*out,size_t cap){if(!out||!cap)return false;out[0]=0;if(!t||!*t||!(osa_streq(l,"fa")||osa_streq(l,"en")))return false;return mt_pronounce(t,osa_streq(l,"fa")?MT_FA:MT_EN,out,cap);}

/* ---- history/corrections ---- */
char*osa_app_history_json(osa_app*a,int limit,osa_error*err){if(!a)return NULL;osa_rs rs;lockd(a);bool ok=osa_store_transcription_list(a->db,&rs,limit>0?limit:50,NULL,err);osa_buf b;osa_buf_init(&b);osa_buf_putc(&b,'[');if(ok)for(int i=0;i<rs.nrows;i++){if(i)osa_buf_putc(&b,',');osa_buf_puts(&b,"{\"id\":");json_str(&b,osa_rs_val(&rs,i,"id"));osa_buf_puts(&b,",\"text\":");json_str(&b,osa_rs_val(&rs,i,"text"));osa_buf_puts(&b,",\"language\":");json_str(&b,osa_rs_val(&rs,i,"language"));osa_buf_printf(&b,",\"confidence\":%.3f,\"duration_ms\":%lld,\"created_at\":",osa_rs_dbl(&rs,i,"confidence"),(long long)osa_rs_i64(&rs,i,"duration_ms"));json_str(&b,osa_rs_val(&rs,i,"created_at"));osa_buf_puts(&b,",\"audio\":");json_str(&b,osa_rs_val(&rs,i,"audio_path"));osa_arena ar;osa_arena_init(&ar);char*sql=osa_arena_printf(&ar,"SELECT text,target_lang FROM Translations WHERE transcription_id='%s' ORDER BY created_at DESC LIMIT 1",osa_sql_escape(&ar,osa_rs_val(&rs,i,"id")));osa_rs tr;if(osa_sql_query(a->db,sql,&tr,NULL)&&tr.nrows){osa_buf_puts(&b,",\"translation\":");json_str(&b,osa_rs_val(&tr,0,"text"));osa_buf_puts(&b,",\"target_lang\":");json_str(&b,osa_rs_val(&tr,0,"target_lang"));}osa_rs_free(&tr);osa_arena_free(&ar);osa_buf_putc(&b,'}');}osa_rs_free(&rs);unlockd(a);osa_buf_putc(&b,']');if(!ok){osa_buf_free(&b);return NULL;}return osa_buf_detach(&b);}
bool osa_app_delete_transcript(osa_app*a,const char*id,osa_error*err){if(!a||!id)return false;osa_arena ar;osa_arena_init(&ar);char*sql=osa_arena_printf(&ar,"SELECT audio_path FROM Transcriptions WHERE id='%s'",osa_sql_escape(&ar,id));char audio[OSAI_MAX_PATH]={0};lockd(a);osa_rs rs;if(osa_sql_query(a->db,sql,&rs,NULL)&&rs.nrows)osa_strlcpy(audio,osa_rs_val(&rs,0,"audio_path"),sizeof audio);osa_rs_free(&rs);sql=osa_arena_printf(&ar,"BEGIN IMMEDIATE;DELETE FROM Translations WHERE transcription_id='%s';DELETE FROM UserCorrections WHERE transcription_id='%s';DELETE FROM Transcriptions WHERE id='%s';DELETE FROM AudioFiles WHERE path='%s';COMMIT;",osa_sql_escape(&ar,id),osa_sql_escape(&ar,id),osa_sql_escape(&ar,id),osa_sql_escape(&ar,audio));bool ok=osa_sql_exec(a->db,sql,err);if(!ok)osa_sql_exec(a->db,"ROLLBACK;",NULL);unlockd(a);osa_arena_free(&ar);if(ok&&audio[0])osa_file_remove(audio);return ok;}
bool osa_app_add_correction(osa_app*a,const char*id,const char*corrected,osa_error*err){if(!a||!corrected)return false;osa_arena ar;osa_arena_init(&ar);char*original=osa_arena_strdup(&ar,"");if(id&&*id){char*sql=osa_arena_printf(&ar,"SELECT text FROM Transcriptions WHERE id='%s'",osa_sql_escape(&ar,id));lockd(a);osa_rs rs;if(osa_sql_query(a->db,sql,&rs,NULL)&&rs.nrows)original=osa_arena_strdup(&ar,osa_rs_val(&rs,0,"text"));osa_rs_free(&rs);unlockd(a);}lockd(a);bool ok=osa_store_correction_add(a->db,id,original,corrected,0,err);unlockd(a);osa_arena_free(&ar);return ok;}

/* ---- verified downloads / installation, never automatic activation ---- */
int osa_app_packs(osa_app*a,osa_pack_info*out,int max){if(!a||!out||max<=0)return 0;osa_rs rs;lockd(a);bool q=osa_store_model_list(a->db,&rs,NULL,NULL);int n=0;for(int i=0;i<NPACKS&&n<max;i++){const pack_def*p=PACKS+i;osa_pack_info*x=out+n++;memset(x,0,sizeof*x);osa_strlcpy(x->id,p->id,sizeof x->id);osa_strlcpy(x->name,p->name,sizeof x->name);osa_strlcpy(x->kind,p->kind,sizeof x->kind);osa_strlcpy(x->language,p->languages,sizeof x->language);osa_strlcpy(x->license,p->license,sizeof x->license);osa_strlcpy(x->url,p->url,sizeof x->url);osa_strlcpy(x->sha256,p->sha256,sizeof x->sha256);osa_strlcpy(x->notes,p->notes,sizeof x->notes);x->size_mb=(double)p->bytes/1048576.0;x->min_ram_mb=p->min_ram_mb;x->installable=true;if(q)for(int r=0;r<rs.nrows;r++)if(osa_streq(osa_rs_val(&rs,r,"id"),p->id))x->installed=true;}osa_rs_free(&rs);unlockd(a);return n;}
int osa_app_fetch_pack(const char*id,void(*progress)(int64_t,int64_t,void*),void*ud,volatile int*cancel,char*file_out,size_t cap,osa_error*err){const pack_def*p=find_pack(id);if(!p||!file_out||!cap){osa_err_set(err,"unknown model or invalid output buffer");return -1;}if(!osa_paths_init()){osa_err_set(err,"cannot initialize download directory");return -1;}char dst[OSAI_MAX_PATH];snprintf(dst,sizeof dst,"%s%s.%s",osa_path(OSAI_DIR_DOWNLOADS),p->id,p->ext);int64_t need=p->bytes*2+268435456;if(osa_disk_free(osa_path(OSAI_DIR_DOWNLOADS))<need){osa_err_set(err,"not enough free disk space for download plus installed copy");return -1;}int rc=osa_http_download(p->url,dst,true,progress,ud,cancel,err);if(rc)return rc;if(!verify(dst,p,err)){osa_file_remove(dst);return -1;}osa_strlcpy(file_out,dst,cap);return 0;}
static bool install_verified(osa_app*a,const char*path,const pack_def*p,osa_error*err){
    if(!verify(path,p,err))return false;
    char dst[OSAI_MAX_PATH],tmp[OSAI_MAX_PATH+8];
    if(snprintf(dst,sizeof dst,"%s%s.%s",a->models_dir,p->id,p->ext)>=(int)sizeof dst){osa_err_set(err,"model path too long");return false;}
    if(osa_file_exists(dst)&&verify(dst,p,NULL))return register_pack(a,p,dst,err);
    snprintf(tmp,sizeof tmp,"%s.tmp",dst);
    if(!osa_file_copy(path,tmp)){osa_err_set(err,"cannot copy model into data directory");return false;}
    if(!verify(tmp,p,err)||!osa_file_rename(tmp,dst)){osa_file_remove(tmp);osa_err_set(err,"cannot finalize verified model");return false;}
    return register_pack(a,p,dst,err);
}
bool osa_app_install_pack_file(osa_app*a,const char*path,osa_error*err){if(!a||!path){osa_err_set(err,"bad arguments");return false;}char h[65];osa_sha256_file(path,h,NULL);for(int i=0;i<NPACKS;i++)if(osa_strieq(h,PACKS[i].sha256))return install_verified(a,path,PACKS+i,err);osa_err_set(err,"unverified model; choose a model from the verified download catalog");return false;}
int osa_app_download_pack(osa_app*a,const char*id,void(*progress)(int64_t,int64_t,void*),void*ud,volatile int*cancel,osa_error*err){const pack_def*p=find_pack(id);if(!a||!p){osa_err_set(err,"unknown model");return -1;}osa_mutex_lock(&a->life_mu);char file[OSAI_MAX_PATH];int rc=osa_app_fetch_pack(id,progress,ud,cancel,file,sizeof file,err);if(rc==0&&!install_verified(a,file,p,err))rc=-1;if(rc==0)osa_file_remove(file);osa_mutex_unlock(&a->life_mu);return rc;}
bool osa_app_install_builtin_pack(osa_app*a,const char*id,osa_error*err){(void)a;(void)id;osa_err_set(err,"no models are bundled; download a verified model first");return false;}
bool osa_app_import_model(osa_app*a,const char*path,char*id_out,size_t cap,osa_error*err){char h[65];osa_sha256_file(path,h,NULL);const pack_def*p=NULL;for(int i=0;i<NPACKS;i++)if(osa_strieq(h,PACKS[i].sha256)){p=PACKS+i;break;}if(!p){osa_err_set(err,"only catalog-verified GGUF, Whisper and fastText models can be imported");return false;}if(!install_verified(a,path,p,err))return false;if(id_out&&cap)osa_strlcpy(id_out,p->id,cap);return true;}
char*osa_app_models_json(osa_app*a,osa_error*err){if(!a)return NULL;osa_rs rs;lockd(a);bool ok=osa_store_model_list(a->db,&rs,NULL,err);osa_buf b;osa_buf_init(&b);osa_buf_putc(&b,'[');if(ok)for(int i=0;i<rs.nrows;i++){if(i)osa_buf_putc(&b,',');osa_buf_puts(&b,"{\"id\":");json_str(&b,osa_rs_val(&rs,i,"id"));osa_buf_puts(&b,",\"name\":");json_str(&b,osa_rs_val(&rs,i,"name"));osa_buf_puts(&b,",\"kind\":");json_str(&b,osa_rs_val(&rs,i,"kind"));osa_buf_puts(&b,",\"engine\":");json_str(&b,osa_rs_val(&rs,i,"engine"));osa_buf_puts(&b,",\"license\":");json_str(&b,osa_rs_val(&rs,i,"license"));osa_buf_puts(&b,",\"source\":");json_str(&b,osa_rs_val(&rs,i,"source_url"));osa_buf_puts(&b,",\"version\":");json_str(&b,osa_rs_val(&rs,i,"version"));osa_buf_puts(&b,",\"sha256\":");json_str(&b,osa_rs_val(&rs,i,"sha256"));osa_buf_printf(&b,",\"bytes\":%lld,\"active\":%s}",(long long)osa_rs_i64(&rs,i,"size_bytes"),osa_rs_int(&rs,i,"active")?"true":"false");}osa_rs_free(&rs);unlockd(a);osa_buf_putc(&b,']');if(!ok){osa_buf_free(&b);return NULL;}return osa_buf_detach(&b);}

/* Fine-tuning these quantized inference artifacts is not implemented. Never
 * disguise exemplars or synthetic TTS as neural training/benchmark results. */
int osa_app_datasets(osa_app*a,osa_dataset_info*out,int max){(void)a;(void)out;(void)max;return 0;}
bool osa_app_install_dataset(osa_app*a,const char*id,char*p,size_t c,osa_error*err){(void)a;(void)id;if(p&&c)p[0]=0;osa_err_set(err,"no synthetic datasets are bundled; use your own labelled recordings with an external trainer");return false;}
bool osa_app_train(osa_app*a,const char*p,int epochs,osa_train_result*out,osa_error*err){(void)a;(void)p;(void)epochs;if(out)memset(out,0,sizeof*out);osa_err_set(err,"fine-tuning is not supported for these inference models; corrections are saved for review only");return false;}
char*osa_app_evaluate(osa_app*a,osa_error*err){if(!osa_app_ready(a)){osa_err_set(err,"activate all three models first");return NULL;}osa_buf b;osa_buf_init(&b);osa_buf_puts(&b,"{\"engine\":\"whisper.cpp\",\"model\":");locke(a);json_str(&b,a->asr_name);unlocke(a);osa_buf_puts(&b,",\"wer\":0,\"cer\":0,\"rtf\":0,\"samples\":0,\"audio_s\":0,\"proc_s\":0,\"languages\":[],\"skipped\":[],\"items\":[],\"note\":\"No labelled recordings supplied. Synthetic demo scores have been removed.\"}");osa_err_ok(err);return osa_buf_detach(&b);}
