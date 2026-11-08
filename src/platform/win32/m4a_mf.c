/* AAC/M4A/MP4 decoding with the Windows Media Foundation source reader.
 * The system owns the file byte stream and every COM object: no hand-written
 * IStream vtable (the old layout/refcount bug caused mfreadwrite.dll AVs).
 * Runs on the file-job thread, initializes MTA there, and balances every
 * successful initialization/release even on error. MF DLLs remain delay-loaded
 * by name so machines without Media Foundation can still use WAV/MP3/FLAC. */
#if defined(_WIN32)
#include <windows.h>
#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "audio_m4a.h"

#define TARGET_RATE 16000
typedef struct mf_api {
    HMODULE plat,rw;
    HRESULT (STDAPICALLTYPE *startup)(ULONG,DWORD);
    HRESULT (STDAPICALLTYPE *shutdown)(void);
    HRESULT (STDAPICALLTYPE *reader_url)(LPCWSTR,IMFAttributes*,IMFSourceReader**);
    HRESULT (STDAPICALLTYPE *media_type)(IMFMediaType**);
} mf_api;
static mf_api g; static bool g_tried;
static mf_api *load_mf(void){
    if(g_tried)return g.plat&&g.rw?&g:NULL;g_tried=true;
    g.plat=LoadLibraryW(L"mfplat.dll");g.rw=LoadLibraryW(L"mfreadwrite.dll");if(!g.plat||!g.rw)return NULL;
    g.startup=(void*)GetProcAddress(g.plat,"MFStartup");g.shutdown=(void*)GetProcAddress(g.plat,"MFShutdown");
    g.reader_url=(void*)GetProcAddress(g.rw,"MFCreateSourceReaderFromURL");g.media_type=(void*)GetProcAddress(g.plat,"MFCreateMediaType");
    return g.startup&&g.shutdown&&g.reader_url&&g.media_type?&g:NULL;
}
bool osa_m4a_decode(const char *path,float **frames,int64_t *n,int *rate,osa_error *err){
    if(frames)*frames=NULL;if(n)*n=0;if(rate)*rate=0;
    if(!path||!*path){osa_err_set(err,"no audio file");return false;}
    mf_api *mf=load_mf();if(!mf){osa_err_set(err,"Media Foundation is unavailable; convert this file to WAV/MP3 or install the Windows Media Feature Pack");return false;}
    wchar_t wpath[32768];int wn=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wpath,32768);if(!wn){osa_err_set(err,"invalid audio file path");return false;}
    HRESULT hc=CoInitializeEx(NULL,COINIT_MULTITHREADED);
    if(FAILED(hc)){osa_err_set(err,"cannot initialize COM MTA for audio decoding (0x%08lx)",(unsigned long)hc);return false;}
    HRESULT hm=mf->startup(MF_VERSION,MFSTARTUP_LITE);
    if(FAILED(hm)){CoUninitialize();osa_err_set(err,"Media Foundation could not start (0x%08lx)",(unsigned long)hm);return false;}
    IMFSourceReader *reader=NULL;IMFMediaType *want=NULL;float *buf=NULL;size_t cap=0,used=0;bool ok=false;
    HRESULT hr=mf->reader_url(wpath,NULL,&reader);
    if(FAILED(hr)||!reader){osa_err_set(err,"Media Foundation cannot open this audio container (0x%08lx)",(unsigned long)hr);goto done;}
    /* Deselect everything except the first audio stream. NULL attributes let
     * the source resolver create and own a correct system byte stream. */
    reader->lpVtbl->SetStreamSelection(reader,(DWORD)MF_SOURCE_READER_ALL_STREAMS,FALSE);
    hr=reader->lpVtbl->SetStreamSelection(reader,(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,TRUE);
    if(FAILED(hr)){osa_err_set(err,"file has no selectable audio stream");goto done;}
    hr=mf->media_type(&want);if(FAILED(hr)||!want){osa_err_set(err,"cannot allocate audio format");goto done;}
    want->lpVtbl->SetGUID(want,&MF_MT_MAJOR_TYPE,&MFMediaType_Audio);
    want->lpVtbl->SetGUID(want,&MF_MT_SUBTYPE,&MFAudioFormat_PCM);
    want->lpVtbl->SetUINT32(want,&MF_MT_AUDIO_NUM_CHANNELS,1);
    want->lpVtbl->SetUINT32(want,&MF_MT_AUDIO_SAMPLES_PER_SECOND,TARGET_RATE);
    want->lpVtbl->SetUINT32(want,&MF_MT_AUDIO_BITS_PER_SAMPLE,16);
    want->lpVtbl->SetUINT32(want,&MF_MT_AUDIO_BLOCK_ALIGNMENT,2);
    want->lpVtbl->SetUINT32(want,&MF_MT_AUDIO_AVG_BYTES_PER_SECOND,TARGET_RATE*2);
    hr=reader->lpVtbl->SetCurrentMediaType(reader,(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,NULL,want);
    if(FAILED(hr)){osa_err_set(err,"Windows cannot decode/resample this audio track (0x%08lx)",(unsigned long)hr);goto done;}
    for(;;){
        DWORD flags=0;IMFSample *sample=NULL;
        hr=reader->lpVtbl->ReadSample(reader,(DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,0,NULL,&flags,NULL,&sample);
        if(FAILED(hr)){if(sample)sample->lpVtbl->Release(sample);osa_err_set(err,"audio read failed (0x%08lx)",(unsigned long)hr);goto done;}
        if(flags&MF_SOURCE_READERF_ENDOFSTREAM){if(sample)sample->lpVtbl->Release(sample);break;}
        if(flags&MF_SOURCE_READERF_STREAMTICK){if(sample)sample->lpVtbl->Release(sample);continue;}
        if(!sample)continue;
        IMFMediaBuffer *mb=NULL;
        if(SUCCEEDED(sample->lpVtbl->ConvertToContiguousBuffer(sample,&mb))&&mb){
            BYTE *data=NULL;DWORD len=0;
            if(SUCCEEDED(mb->lpVtbl->Lock(mb,&data,NULL,&len))&&data&&len){
                size_t add=len/2;
                if(add>(SIZE_MAX/sizeof(float))-used){mb->lpVtbl->Unlock(mb);mb->lpVtbl->Release(mb);sample->lpVtbl->Release(sample);osa_err_set(err,"audio too large");goto done;}
                if(used+add>cap){size_t nc=cap?cap*2:65536;while(nc<used+add)nc*=2;float *nb=realloc(buf,nc*sizeof(float));if(!nb){mb->lpVtbl->Unlock(mb);mb->lpVtbl->Release(mb);sample->lpVtbl->Release(sample);osa_err_set(err,"out of memory decoding audio");goto done;}buf=nb;cap=nc;}
                const BYTE *p=data;for(size_t i=0;i<add;i++){int16_t v=(int16_t)((uint16_t)p[2*i]|((uint16_t)p[2*i+1]<<8));buf[used+i]=(float)v/32768.0f;}used+=add;
                mb->lpVtbl->Unlock(mb);
            }
            mb->lpVtbl->Release(mb);
        }
        sample->lpVtbl->Release(sample);
    }
    if(!used){osa_err_set(err,"audio track contains no samples");goto done;}
    if(frames)*frames=buf;else free(buf);
    if(n)*n=(int64_t)used;if(rate)*rate=TARGET_RATE;buf=NULL;ok=true;osa_err_ok(err);
done:
    free(buf);if(want)want->lpVtbl->Release(want);if(reader)reader->lpVtbl->Release(reader);
    mf->shutdown();CoUninitialize();return ok;
}
#endif
