#include "media.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

bool nmedia_audio_extension(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && (!strcasecmp(dot,".wav") || !strcasecmp(dot,".mp3") || !strcasecmp(dot,".flac") || !strcasecmp(dot,".aac") || !strcasecmp(dot,".m4a"));
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
const char *nmedia_can_play_type(const char *type) {
    if (!type || strlen(type) >= 256) return "";
    char text[256];
    size_t n = strlen(type);
    for (size_t i = 0; i <= n; i++) text[i] = (char)tolower((unsigned char)type[i]);
    char *semi = strchr(text,';'); if (semi) *semi++ = 0;
    char *mime = trim(text);
    enum { NONE, MP3, FLAC, WAV, AAC, MP4, AVI } container = NONE;
    if (!strcmp(mime,"audio/mpeg") || !strcmp(mime,"audio/mp3")) container = MP3;
    else if (!strcmp(mime,"audio/flac") || !strcmp(mime,"audio/x-flac")) container = FLAC;
    else if (!strcmp(mime,"audio/wav") || !strcmp(mime,"audio/x-wav") || !strcmp(mime,"audio/wave")) container = WAV;
    else if (!strcmp(mime,"audio/aac") || !strcmp(mime,"audio/x-aac")) container = AAC;
    else if (!strcmp(mime,"video/mp4") || !strcmp(mime,"audio/mp4") || !strcmp(mime,"video/quicktime")) container = MP4;
    else if (!strcmp(mime,"video/x-msvideo") || !strcmp(mime,"video/avi")) container = AVI;
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
        if (!ok || (next && !*next)) return "";
        p = next;
    }
    return "probably";
}
