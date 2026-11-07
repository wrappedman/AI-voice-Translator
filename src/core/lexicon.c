/* lexicon.c — the built-in word list with romanized pronunciations.
 * Shared by the recognizer (token set, LM vocabulary) and the formant TTS
 * (pronunciation), so a word can always be both spoken and recognized. */
#include "tts.h"



static const tts_word_pair FA_WORDS[] = {
    {"سلام", "salaam"}, {"حال", "haal"}, {"شما", "shomaa"}, {"خوب", "khub"}, {"است", "ast"},
    {"این", "in"}, {"یک", "yek"}, {"آزمایش", "aazmaayesh"}, {"نرم", "narm"}, {"افزار", "afzaar"},
    {"هوش", "hosh"}, {"مصنوعی", "masnooi"}, {"لطفا", "lotfan"}, {"فایل", "faayl"}, {"صوتی", "sowti"},
    {"را", "raa"}, {"انتخاب", "entekhaab"}, {"کنید", "konid"}, {"متن", "matn"}, {"ترجمه", "tarjome"},
    {"به", "be"}, {"زبان", "zabaan"}, {"انگلیسی", "engelisi"}, {"فارسی", "faarsi"}, {"مدل", "model"},
    {"آموزش", "aamoozesh"}, {"داده", "daade"}, {"شده", "shode"}, {"ذخیره", "zakhire"}, {"تشخیص", "tashkhis"},
    {"گفتار", "goftaar"}, {"دقیق", "daghiq"}, {"امروز", "emruz"}, {"هوا", "havaa"}, {"خیلی", "kheyli"},
    {"قشنگ", "ghashang"}, {"سیستم", "sistem"}, {"صورت", "sorat"}, {"آفلاین", "aafelaayn"}, {"کار", "kaar"},
    {"می", "mi"}, {"کند", "konad"}, {"میکروفون", "mikrofon"}, {"روشن", "roshan"}, {"ضبط", "zabt"},
    {"صدا", "seda"}, {"نتیجه", "natije"}, {"شروع", "shoroo"}, {"پایان", "paayaan"}, {"سرعت", "sorat"},
};
static const tts_word_pair EN_WORDS[] = {
    {"hello", "helo"}, {"world", "world"}, {"this", "dhis"}, {"is", "iz"}, {"a", "a"},
    {"speech", "speech"}, {"recognition", "rekognishun"}, {"test", "test"}, {"the", "dha"},
    {"quick", "kwik"}, {"brown", "brawn"}, {"fox", "foks"}, {"jumps", "jamps"}, {"over", "over"},
    {"lazy", "leyzi"}, {"dog", "dog"}, {"select", "silekt"}, {"an", "an"}, {"audio", "awdio"},
    {"file", "fayl"}, {"start", "staart"}, {"recording", "rekording"}, {"microphone", "maykrofon"},
    {"artificial", "artifishal"}, {"intelligence", "intelijens"}, {"system", "sistem"},
    {"translation", "transleyshun"}, {"nice", "nays"}, {"today", "tudey"}, {"weather", "wedher"},
    {"very", "very"}, {"save", "seyv"}, {"text", "tekst"}, {"offline", "oflayn"}, {"works", "works"},
    {"quality", "kwaliti"}, {"language", "langgwij"}, {"persian", "pershiyan"}, {"english", "ingglish"},
    {"stop", "stop"},
};

const tts_word_pair *osai_lexicon(const char *lang, int *count) {
    if (osa_starts(lang ? lang : "fa", "en")) {
        if (count) *count = (int)(sizeof EN_WORDS / sizeof EN_WORDS[0]);
        return (const tts_word_pair *)EN_WORDS;
    }
    if (count) *count = (int)(sizeof FA_WORDS / sizeof FA_WORDS[0]);
    return (const tts_word_pair *)FA_WORDS;
}
