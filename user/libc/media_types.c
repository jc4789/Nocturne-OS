#include "media.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

bool nmedia_audio_extension(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && (!strcasecmp(dot,".wav") || !strcasecmp(dot,".mp3") || !strcasecmp(dot,".flac") || !strcasecmp(dot,".aac") || !strcasecmp(dot,".m4a") || !strcasecmp(dot,".ogg") || !strcasecmp(dot,".oga") || !strcasecmp(dot,".opus"));
}

/* This allow-list matches the configured decoders, containers AND the native
 * PCM/pixel output converter. Codec availability alone is not playback support. */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = 0;
    return s;
}
static bool avc8(const char *s) {
    if (strncmp(s,"avc1.",5) && strncmp(s,"avc3.",5)) return false;
    if (strlen(s) != 11) return false;
    for (int i = 5; i < 11; i++) if (!isxdigit((unsigned char)s[i])) return false;
    unsigned profile = (unsigned)strtoul(s+5,NULL,16) >> 16;
    return profile == 66 || profile == 77 || profile == 88 || profile == 100;
}
static bool vp9_8(const char *s) {
    if (!strcmp(s,"vp9") || !strcmp(s,"vp9.0")) return true;
    /* WebM legacy spelling, or the standardized all-or-none color suffix.
     * Profile 0, 8-bit 4:2:0 SDR only; no HDR/10-bit capability inflation. */
    size_t n = strlen(s);
    if ((n != 13 && n != 28) || strncmp(s,"vp09.",5)) return false;
    for (size_t i=5; i<n; i+=3) {
        if (!isdigit((unsigned char)s[i]) || !isdigit((unsigned char)s[i+1])) return false;
        if (i+2<n && s[i+2]!='.') return false;
    }
    if (s[5]!='0' || s[6]!='0' || s[11]!='0' || s[12]!='8') return false;
    int level=(s[8]-'0')*10+s[9]-'0';
    if (level!=10 && level!=11 && level!=20 && level!=21 && level!=30 && level!=31 &&
        level!=40 && level!=41 && level!=50 && level!=51 && level!=52 && level!=60 && level!=61 && level!=62) return false;
    if (n==28) {
        if (s[14]!='0' || (s[15]!='0' && s[15]!='1') || s[26]!='0' || (s[27]!='0' && s[27]!='1')) return false;
        if (strncmp(s+17,"01.01.01.",9) && strncmp(s+17,"05.06.05.",9) && strncmp(s+17,"06.06.06.",9)) return false;
    }
    return true;
}
const char *nmedia_can_play_type(const char *type) {
    if (!type || strlen(type) >= 256) return "";
    char text[256];
    size_t n = strlen(type);
    for (size_t i = 0; i <= n; i++) text[i] = (char)tolower((unsigned char)type[i]);
    char *semi = strchr(text,';'); if (semi) *semi++ = 0;
    char *mime = trim(text);
    enum { NONE, MP3, FLAC, WAV, AAC, MP4, AVI, WEBM, OGG } container = NONE;
    if (!strcmp(mime,"audio/mpeg") || !strcmp(mime,"audio/mp3")) container = MP3;
    else if (!strcmp(mime,"audio/flac") || !strcmp(mime,"audio/x-flac")) container = FLAC;
    else if (!strcmp(mime,"audio/wav") || !strcmp(mime,"audio/x-wav") || !strcmp(mime,"audio/wave")) container = WAV;
    else if (!strcmp(mime,"audio/aac") || !strcmp(mime,"audio/x-aac")) container = AAC;
    else if (!strcmp(mime,"video/mp4") || !strcmp(mime,"audio/mp4") || !strcmp(mime,"video/quicktime")) container = MP4;
    else if (!strcmp(mime,"video/x-msvideo") || !strcmp(mime,"video/avi")) container = AVI;
    else if (!strcmp(mime,"video/webm") || !strcmp(mime,"audio/webm")) container = WEBM;
    else if (!strcmp(mime,"audio/ogg") || !strcmp(mime,"application/ogg")) container = OGG;
    if (container == NONE) return "";
    char *codecs = NULL;
    while (semi && *semi) {
        char *next = strchr(semi,';'); if (next) *next++ = 0;
        char *param = trim(semi), *equal = strchr(param,'=');
        if (!equal) return "";
        *equal++ = 0;
        if (!strcmp(trim(param),"codecs")) {
            if (codecs) return "";
            codecs = trim(equal);
            size_t z = strlen(codecs);
            if (*codecs == '"') { if (z < 2 || codecs[z-1] != '"') return ""; codecs[z-1] = 0; codecs++; }
        }
        semi = next;
    }
    if (!codecs) return "maybe";
    if (!*codecs) return "";
    for (char *p = codecs; p;) {
        char *next = strchr(p,','); if (next) *next++ = 0;
        char *codec = trim(p); bool ok = false;
        if (container == MP3) ok = !strcmp(codec,"mp3");
        else if (container == FLAC) ok = !strcmp(codec,"flac");
        else if (container == WAV) ok = !strcmp(codec,"1") || !strcmp(codec,"3") || !strcmp(codec,"pcm");
        else if (container == AAC) ok = !strcmp(codec,"mp4a.40.2");
        else if (container == MP4) ok = !strcmp(codec,"mp4a.40.2") || (strncmp(mime,"audio/",6) && avc8(codec));
        else if (container == AVI) ok = !strcmp(codec,"mjpeg") || !strcmp(codec,"mp3") || !strcmp(codec,"pcm");
        else if (container == WEBM) ok = !strcmp(codec,"opus") || !strcmp(codec,"vorbis") || (strncmp(mime,"audio/",6) && vp9_8(codec));
        else if (container == OGG) ok = !strcmp(codec,"opus") || !strcmp(codec,"vorbis");
        if (!ok || (next && !*next)) return "";
        p = next;
    }
    return "probably";
}
