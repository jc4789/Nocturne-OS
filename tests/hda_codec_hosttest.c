/* The widget tree's reading (kernel/src/dev/hda_codec.c) against codecs made up from tables, in the
   shapes the High Definition Audio Specification allows and real machines have: several converters
   and pins, mixers and selectors, a multichannel association, a front panel, S/PDIF, the pins of a
   graphics card's HDMI codec. tests/check_hda_codec.py builds and runs it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dev/hda_codec.h"

void *hda_alloc(size_t bytes) { return calloc(1, bytes ? bytes : 1); }

/* ---- what a codec answers ---- */

#define MAX_LIST 16
struct node {
    unsigned nid;
    unsigned type;       /* enum hda_widget_type */
    uint32_t caps;       /* its other Audio Widget Capabilities */
    uint32_t pin_caps;
    uint32_t config;
    uint32_t pcm, formats; /* with CAP_FORMAT */
    uint32_t amp_in, amp_out; /* with CAP_AMP */
    bool long_form;
    unsigned count;
    uint16_t list[MAX_LIST]; /* the raw Connection List */
    uint32_t sense;
    const char *monitor; /* its ELD, when not NULL */
    bool displayport;
};

struct mock {
    struct hda_link link; /* first */
    unsigned group;
    uint32_t group_pcm, group_formats, group_amp_in, group_amp_out, group_caps;
    const struct node *nodes;
    unsigned count;
    bool silent;
    unsigned sets;
    struct { unsigned nid; uint32_t command; } set[2048];
};

#define CAP_STEREO   (1u << 0)
#define CAP_AMP_IN   (1u << 1)
#define CAP_AMP_OUT  (1u << 2)
#define CAP_AMP      (1u << 3)
#define CAP_FORMAT   (1u << 4)
#define CAP_UNSOL    (1u << 7)
#define CAP_LIST     (1u << 8)
#define CAP_DIGITAL  (1u << 9)
#define CAP_POWER    (1u << 10)

#define PIN_SENSE (1u << 2)
#define PIN_HP    (1u << 3)
#define PIN_OUT   (1u << 4)
#define PIN_IN    (1u << 5)
#define PIN_HDMI  (1u << 7)
#define PIN_DP    (1u << 24)

#define CONFIG(port, location, device, no_detect, assoc, sequence) \
    ((uint32_t)(port) << 30 | (uint32_t)(location) << 24 | (uint32_t)(device) << 20 | (uint32_t)(no_detect) << 8 | (uint32_t)(assoc) << 4 | (uint32_t)(sequence))
#define REAR  0x01
#define FRONT 0x02
#define PCM_48 (1u << 6)
#define PCM_44 (1u << 5)
#define PCM_16 (1u << 17)
#define PCM_24 (1u << 19)
#define PCM_32 (1u << 20)

static const struct node *node_of(const struct mock *m, unsigned nid) {
    for (unsigned i = 0; i < m->count; i++)
        if (m->nodes[i].nid == nid) return &m->nodes[i];
    return NULL;
}

static uint32_t eld_byte(const struct node *n, unsigned at) {
    size_t length = strlen(n->monitor);
    if (at == 4) return 1u << 31 | (uint32_t)length;
    if (at == 7) return 1u << 31 | (n->displayport ? 1u << 2 : 0);
    if (at >= 20 && at - 20 < length) return 1u << 31 | (uint8_t)n->monitor[at - 20];
    return 1u << 31;
}

static uint32_t parameter(const struct mock *m, unsigned nid, unsigned id) {
    if (nid == 0) return id == 0x04 ? (uint32_t)m->group << 16 | 1 : 0;
    if (nid == m->group) {
        switch (id) {
        case 0x04: return (uint32_t)m->nodes[0].nid << 16 | (m->nodes[m->count - 1].nid - m->nodes[0].nid + 1);
        case 0x05: return 1; /* an audio function group */
        case 0x08: return m->group_caps;
        case 0x0A: return m->group_pcm;
        case 0x0B: return m->group_formats;
        case 0x0D: return m->group_amp_in;
        case 0x12: return m->group_amp_out;
        default: return 0;
        }
    }
    const struct node *n = node_of(m, nid);
    if (!n) return 0;
    switch (id) {
    case 0x09: return (uint32_t)n->type << 20 | n->caps | (n->count ? CAP_LIST : 0);
    case 0x0A: return n->pcm;
    case 0x0B: return n->formats;
    case 0x0C: return n->pin_caps;
    case 0x0D: return n->amp_in;
    case 0x12: return n->amp_out;
    case 0x0E: return (n->long_form ? 0x80u : 0) | n->count;
    default: return 0;
    }
}

static uint32_t mock_verb(struct hda_link *link, unsigned codec, unsigned nid, uint32_t command) {
    struct mock *m = (struct mock *)link;
    (void)codec;
    if (m->silent) return 0;
    const struct node *n = node_of(m, nid);
    unsigned v12 = command >> 8, payload = command & 0xFF;
    if (v12 == 0xF00) return parameter(m, nid, payload);
    if (v12 == 0xF02 && n) {
        uint32_t got = 0;
        unsigned per = n->long_form ? 2 : 4, bits = n->long_form ? 16 : 8;
        for (unsigned k = 0; k < per && payload + k < n->count; k++) got |= (uint32_t)n->list[payload + k] << (k * bits);
        return got;
    }
    if (v12 == 0xF1C && n) return n->config;
    if (v12 == 0xF09 && n) return n->sense;
    if (v12 == 0xF2F && n && n->monitor) return eld_byte(n, payload);
    if (v12 == 0xF2E) return 8; /* a packet of 9 bytes */
    if (v12 == 0xF0C) return 0;
    if (m->sets < sizeof m->set / sizeof m->set[0]) {
        m->set[m->sets].nid = nid;
        m->set[m->sets].command = command;
        m->sets++;
    }
    return 0;
}

static bool was_set(const struct mock *m, unsigned nid, uint32_t command) {
    for (unsigned i = 0; i < m->sets; i++)
        if (m->set[i].nid == nid && m->set[i].command == command) return true;
    return false;
}

/* ---- checking ---- */

static unsigned failures;
#define CHECK(what, condition)                                       \
    do {                                                             \
        if (!(condition)) {                                          \
            printf("FAIL %s: %s (line %d)\n", name, what, __LINE__); \
            failures++;                                              \
        }                                                            \
    } while (0)

static struct hda_output *output_at(struct hda_output *first, unsigned nid) {
    for (; first; first = first->next)
        if (first->pin->nid == nid) return first;
    return NULL;
}

static unsigned output_count(const struct hda_output *o) {
    unsigned n = 0;
    for (; o; o = o->next) n++;
    return n;
}

#define VERB12(v, p) ((uint32_t)(v) << 8 | (uint8_t)(p))
#define VERB4(v, p)  ((uint32_t)(v) << 16 | (uint16_t)(p))
#define AMP_OUT 0xB000u /* output, left and right */
#define AMP_IN(index) (0x7000u | (uint32_t)(index) << 8)
#define AMP_MUTE 0x80u

/* A desktop board: a line out that is the front pair of five channels, its surround and center pins of
   the same association, a front panel's headphones behind a selector, S/PDIF, a microphone, a pin wired
   to nothing. Mixers carry the analog loopback too. */
static void desktop(void) {
    const char *name = "desktop";
    static const struct node nodes[] = {
        {.nid = 0x02, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_AMP_OUT | CAP_POWER},
        {.nid = 0x03, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_AMP_OUT},
        {.nid = 0x04, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | (3u << 16)},
        {.nid = 0x06, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL},
        {.nid = 0x08, .type = HDA_WIDGET_INPUT, .caps = CAP_STEREO},
        {.nid = 0x0B, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO | CAP_AMP_IN, .count = 2, .list = {0x18, 0x1A}},
        {.nid = 0x0C, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO | CAP_AMP_IN | CAP_AMP_OUT, .count = 2, .list = {0x0B, 0x02}},
        {.nid = 0x0D, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO | CAP_AMP_IN, .count = 2, .list = {0x0B, 0x03}},
        {.nid = 0x0E, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO | CAP_AMP_IN | (1u << 16), .count = 2, .list = {0x0B, 0x04}},
        {.nid = 0x0F, .type = HDA_WIDGET_SELECTOR, .caps = CAP_STEREO, .count = 3, .list = {0x0E, 0x0D, 0x0C}},
        {.nid = 0x14, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_AMP_OUT, .pin_caps = PIN_OUT | PIN_SENSE,
         .config = CONFIG(0, REAR, 0x0, 0, 1, 0), .count = 2, .list = {0x0D, 0x0C}, .sense = 1u << 31},
        {.nid = 0x15, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT | PIN_SENSE,
         .config = CONFIG(0, REAR, 0x0, 0, 1, 1), .count = 1, .list = {0x0D}},
        {.nid = 0x16, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT | PIN_SENSE,
         .config = CONFIG(0, REAR, 0x0, 0, 1, 2), .count = 1, .list = {0x0E}},
        {.nid = 0x18, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_IN | PIN_SENSE,
         .config = CONFIG(0, REAR, 0xA, 0, 3, 0)},
        {.nid = 0x19, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(1, REAR, 0x0, 1, 15, 0),
         .count = 1, .list = {0x0C}},
        {.nid = 0x1A, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_IN, .config = CONFIG(0, REAR, 0x8, 0, 4, 0)},
        {.nid = 0x1B, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_UNSOL | CAP_AMP_OUT, .pin_caps = PIN_OUT | PIN_HP | PIN_SENSE,
         .config = CONFIG(0, FRONT, 0x2, 0, 2, 0), .count = 2, .list = {0x0B, 0x0F}},
        {.nid = 0x1E, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL, .pin_caps = PIN_OUT,
         .config = CONFIG(0, REAR, 0x4, 1, 5, 0), .count = 1, .list = {0x06}},
    };
    static struct mock m = {.group = 1, .group_pcm = PCM_48 | PCM_44 | PCM_16 | PCM_24, .group_formats = 1,
                            .group_amp_in = 0x80000000u | 3u << 8 | 3u, .group_amp_out = 2u << 8 | 2u /* 1 dB-step scale */,
                            .nodes = nodes, .count = sizeof nodes / sizeof nodes[0]};
    m.link.verb = mock_verb;
    struct hda_codec *codec = hda_codec_read(&m.link, 0);
    CHECK("the codec reads", codec && codec->groups && codec->groups->nwidgets == 0x1E - 0x02 + 1);
    struct hda_output *outputs = hda_codec_outputs(codec);
    CHECK("three outputs: the association's front pair, the front panel, S/PDIF", output_count(outputs) == 3);
    struct hda_output *line = output_at(outputs, 0x14), *phones = output_at(outputs, 0x1B), *spdif = output_at(outputs, 0x1E);
    CHECK("no surround, center, microphone or unwired pin", !output_at(outputs, 0x15) && !output_at(outputs, 0x16) && !output_at(outputs, 0x18) && !output_at(outputs, 0x19) && !output_at(outputs, 0x1A));
    if (!line || !phones || !spdif) return;

    CHECK("the line out's kind", line->kind == HDA_OUTPUT_LINE && !strcmp(line->name, "Rear line out"));
    CHECK("the line out goes through its first input", line->length == 3 && line->path[1]->nid == 0x0D && line->converter->nid == 0x03);
    CHECK("the line out senses, without announcing", line->senses && !line->announces);
    CHECK("the way's delay adds up where the group says none", phones->delay == 4 && line->delay == 0);
    CHECK("the headphones' kind", phones->kind == HDA_OUTPUT_HEADPHONE && !strcmp(phones->name, "Front headphones"));
    CHECK("the headphones announce", phones->senses && phones->announces);
    CHECK("the headphones go through the selector", phones->length == 4 && phones->path[1]->nid == 0x0F && phones->path[2]->nid == 0x0E && phones->converter->nid == 0x04);
    CHECK("S/PDIF is digital and takes the digital converter", spdif->kind == HDA_OUTPUT_DIGITAL && spdif->converter->nid == 0x06);
    CHECK("S/PDIF does not sense (no detect)", !spdif->senses);
    CHECK("at first 48 kHz and 16 bits where it has them", hda_output_format(line) == 0x0011 && line->channels == 2);

    hda_codec_power(codec, outputs, 5);
    CHECK("the group is powered", was_set(&m, 1, VERB12(0x705, 0)));
    CHECK("the announcing pin has the tag", was_set(&m, 0x1B, VERB12(0x708, 0x80 | 5)));
    CHECK("the other pins do not", !was_set(&m, 0x14, VERB12(0x708, 0x80 | 5)));
    CHECK("sense sees what is plugged in", hda_output_sense(line) && line->plugged);
    CHECK("sense sees no change the second time", !hda_output_sense(line));
    CHECK("sense sees nothing plugged in", !hda_output_sense(phones) && !phones->plugged);

    m.sets = 0;
    hda_output_enable(phones, 3);
    CHECK("the pin selects the selector", was_set(&m, 0x1B, VERB12(0x701, 1)));
    CHECK("the selector selects the way", was_set(&m, 0x0F, VERB12(0x701, 0)));
    CHECK("the mixer's way is open at 0 dB", was_set(&m, 0x0E, VERB4(0x3, AMP_IN(1) | 3)));
    CHECK("the mixer's loopback is shut", was_set(&m, 0x0E, VERB4(0x3, AMP_IN(0) | AMP_MUTE | 3)));
    CHECK("the pin's amplifier is at 0 dB", was_set(&m, 0x1B, VERB4(0x3, AMP_OUT | 2)));
    CHECK("the converter has the format", was_set(&m, 0x04, VERB4(0x2, 0x0011)));
    CHECK("the converter has the stream", was_set(&m, 0x04, VERB12(0x706, 3 << 4)));
    CHECK("the pin drives headphones", was_set(&m, 0x1B, VERB12(0x707, 0xC0)));
    m.sets = 0;
    hda_output_disable(phones);
    CHECK("the pin goes off", was_set(&m, 0x1B, VERB12(0x707, 0)));
    CHECK("the converter leaves the stream", was_set(&m, 0x04, VERB12(0x706, 0)));
    m.sets = 0;
    hda_output_enable(line, 1);
    CHECK("the line out's pin selects its way", was_set(&m, 0x14, VERB12(0x701, 0)));
    CHECK("a line out is no headphone", was_set(&m, 0x14, VERB12(0x707, 0x40)));
    hda_output_enable(spdif, 1);
    CHECK("S/PDIF is switched on", was_set(&m, 0x06, VERB12(0x70D, 1)));
}

/* A graphics card's codec: a converter and a pin to each of four display outputs, every converter
   reachable from every pin, spelled as ranges; a display on two of them, one of them DisplayPort;
   one pin the firmware wired to nothing. */
static void graphics(void) {
    const char *name = "graphics";
    enum { DISPLAY_PIN_CAPS = PIN_OUT | PIN_SENSE | PIN_HDMI | PIN_DP };
    static const struct node nodes[] = {
        {.nid = 0x04, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL | CAP_UNSOL | CAP_AMP_OUT, .pin_caps = DISPLAY_PIN_CAPS,
         .config = CONFIG(0, 0x18, 0x6, 0, 1, 0), .long_form = true, .count = 2, .list = {0x08, 0x800B},
         .sense = 3u << 30, .monitor = "Mock TV"},
        {.nid = 0x05, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL | CAP_UNSOL | CAP_AMP_OUT, .pin_caps = DISPLAY_PIN_CAPS,
         .config = CONFIG(0, 0x18, 0x6, 0, 1, 1), .count = 2, .list = {0x09, 0x8B},
         .sense = 3u << 30, .monitor = "Panel", .displayport = true},
        {.nid = 0x06, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL | CAP_UNSOL | CAP_AMP_OUT, .pin_caps = DISPLAY_PIN_CAPS,
         .config = CONFIG(0, 0x18, 0x6, 0, 2, 0), .count = 2, .list = {0x08, 0x8B}, .sense = 2u << 30},
        {.nid = 0x07, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL | CAP_UNSOL, .pin_caps = DISPLAY_PIN_CAPS,
         .config = CONFIG(1, 0x18, 0x6, 0, 15, 0), .count = 2, .list = {0x08, 0x8B}},
        {.nid = 0x08, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL | (3u << 13)},
        {.nid = 0x09, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL | (3u << 13)},
        {.nid = 0x0A, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL | (3u << 13)},
        {.nid = 0x0B, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL | (3u << 13)},
    };
    static struct mock m = {.group = 1, .group_pcm = PCM_48 | PCM_16 | PCM_24, .group_formats = 1, .nodes = nodes,
                            .count = sizeof nodes / sizeof nodes[0]};
    m.link.verb = mock_verb;
    struct hda_codec *codec = hda_codec_read(&m.link, 2);
    struct hda_output *outputs = codec ? hda_codec_outputs(codec) : NULL;
    CHECK("each wired pin is a display of its own, whatever its association", output_count(outputs) == 3);
    struct hda_output *tv = output_at(outputs, 0x04), *panel = output_at(outputs, 0x05), *empty = output_at(outputs, 0x06);
    if (!tv || !panel || !empty) return;
    const struct hda_widget *pin = codec->groups->widgets;
    CHECK("a long form list with a range is spelled out", pin[0].ninputs == 4 && pin[0].inputs[0] == 0x08 && pin[0].inputs[3] == 0x0B);
    CHECK("a short form list with a range is spelled out", pin[1].ninputs == 3 && pin[1].inputs[0] == 0x09 && pin[1].inputs[2] == 0x0B);
    CHECK("the pin is a display's", tv->kind == HDA_OUTPUT_DISPLAY && !strcmp(tv->name, "Display audio"));
    CHECK("sense reads the ELD", hda_output_sense(tv) && tv->plugged && !tv->displayport && !strcmp(tv->name, "HDMI: Mock TV"));
    CHECK("the ELD tells DisplayPort", hda_output_sense(panel) && panel->plugged && panel->displayport && !strcmp(panel->name, "DisplayPort: Panel"));
    CHECK("no valid ELD, nothing to stream to", !hda_output_sense(empty) && !empty->plugged);
    m.sets = 0;
    hda_output_enable(tv, 1);
    CHECK("the slots carry the stream's two channels", was_set(&m, 0x04, VERB12(0x734, 0x00)) && was_set(&m, 0x04, VERB12(0x734, 0x11)) && was_set(&m, 0x04, VERB12(0x734, 0xF2)));
    CHECK("the converter is told it has two", was_set(&m, 0x08, VERB12(0x72D, 1)));
    CHECK("the converter is on", was_set(&m, 0x08, VERB12(0x70D, 1)));
    CHECK("the InfoFrame goes out", was_set(&m, 0x04, VERB12(0x731, 0x84)) && was_set(&m, 0x04, VERB12(0x732, 0xC0)));
    CHECK("the pin is on", was_set(&m, 0x04, VERB12(0x707, 0x40)));
    m.sets = 0;
    hda_output_enable(panel, 1);
    CHECK("DisplayPort's secondary data packet header", was_set(&m, 0x05, VERB12(0x731, 0x00)) && was_set(&m, 0x05, VERB12(0x731, 0x1B)));
    m.sets = 0;
    hda_output_disable(tv);
    CHECK("the InfoFrame stops", was_set(&m, 0x04, VERB12(0x732, 0)));
}

/* A loop among mixers and selectors, with a converter behind it for one pin and none for another. */
static void loops(void) {
    const char *name = "loops";
    static const struct node nodes[] = {
        {.nid = 0x02, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO},
        {.nid = 0x03, .type = HDA_WIDGET_SELECTOR, .caps = CAP_STEREO, .count = 2, .list = {0x04, 0x04}},
        {.nid = 0x04, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO, .count = 2, .list = {0x03, 0x05}},
        {.nid = 0x05, .type = HDA_WIDGET_SELECTOR, .caps = CAP_STEREO, .count = 2, .list = {0x04, 0x02}},
        {.nid = 0x06, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO, .count = 1, .list = {0x07}},
        {.nid = 0x07, .type = HDA_WIDGET_MIXER, .caps = CAP_STEREO, .count = 1, .list = {0x06}},
        {.nid = 0x10, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x0, 1, 1, 0),
         .count = 1, .list = {0x03}},
        {.nid = 0x11, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x1, 1, 2, 0),
         .count = 1, .list = {0x06}},
        {.nid = 0x12, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x1, 1, 3, 0)},
    };
    static struct mock m = {.group = 1, .group_pcm = PCM_48 | PCM_16, .group_formats = 1, .nodes = nodes,
                            .count = sizeof nodes / sizeof nodes[0]};
    m.link.verb = mock_verb;
    struct hda_codec *codec = hda_codec_read(&m.link, 0);
    struct hda_output *outputs = codec ? hda_codec_outputs(codec) : NULL;
    CHECK("only the pin with a way to a converter", output_count(outputs) == 1);
    struct hda_output *o = output_at(outputs, 0x10);
    if (!o) return;
    CHECK("its way ends at the converter", o->converter->nid == 0x02 && o->length == 5);
    CHECK("each step is the input the next one is", o->via[0] == 0 && o->via[1] == 0 && o->via[2] == 1 && o->via[3] == 1);
}

/* What converters declare decides: the rate every codec is to have missing, one channel, only 24 bits,
   and a function group that has no audio. */
static void formats(void) {
    const char *name = "formats";
    static const struct node nodes[] = {
        {.nid = 0x02, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_FORMAT, .pcm = PCM_44 | PCM_16, .formats = 1},
        {.nid = 0x03, .type = HDA_WIDGET_OUTPUT, .caps = CAP_FORMAT, .pcm = PCM_48 | PCM_24, .formats = 1},
        {.nid = 0x04, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_FORMAT, .pcm = PCM_48 | PCM_16, .formats = 4 /* no PCM */},
        {.nid = 0x05, .type = HDA_WIDGET_OUTPUT, .caps = CAP_STEREO | CAP_DIGITAL | CAP_FORMAT, .pcm = PCM_48 | PCM_32, .formats = 1},
        {.nid = 0x10, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x0, 1, 1, 0), .count = 1, .list = {0x02}},
        {.nid = 0x11, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x0, 1, 2, 0), .count = 2, .list = {0x02, 0x03}},
        {.nid = 0x12, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x0, 1, 3, 0), .count = 1, .list = {0x04}},
        {.nid = 0x13, .type = HDA_WIDGET_PIN, .caps = CAP_STEREO | CAP_DIGITAL, .pin_caps = PIN_OUT, .config = CONFIG(0, REAR, 0x4, 1, 4, 0), .count = 1, .list = {0x05}},
    };
    static struct mock m = {.group = 1, .group_caps = 7, .nodes = nodes, .count = sizeof nodes / sizeof nodes[0]};
    m.link.verb = mock_verb;
    struct hda_codec *codec = hda_codec_read(&m.link, 0);
    struct hda_output *outputs = codec ? hda_codec_outputs(codec) : NULL;
    CHECK("the group's delay is the output's", outputs && outputs->delay == 7);
    CHECK("a converter without 48 kHz PCM, or with only raw digital frames, gives no output", output_count(outputs) == 1);
    struct hda_output *o = output_at(outputs, 0x11);
    if (!o) return;
    CHECK("the pin goes on to the converter that fits", o->converter->nid == 0x03);
    uint8_t bits[4];
    CHECK("one channel, 24 bits", o->channels == 1 && hda_output_sizes(o, bits) == 1 && bits[0] == 24);
    CHECK("the format's word: 48 kHz, 24 bits, one channel", hda_output_format(o) == 0x0030);
    /* table 53 of the specification, every rate it can say */
    CHECK("the word of 8 kHz", hda_format_word(8000, 16, 2) == (5u << 8 | 1u << 4 | 1));
    CHECK("the word of 11.025 kHz", hda_format_word(11025, 16, 2) == (1u << 14 | 3u << 8 | 1u << 4 | 1));
    CHECK("the word of 44.1 kHz", hda_format_word(44100, 24, 2) == (1u << 14 | 3u << 4 | 1));
    CHECK("the word of 32 kHz", hda_format_word(32000, 20, 1) == (1u << 11 | 2u << 8 | 2u << 4));
    CHECK("the word of 96 kHz", hda_format_word(96000, 32, 2) == (1u << 11 | 4u << 4 | 1));
    CHECK("the word of 176.4 kHz", hda_format_word(176400, 16, 2) == (1u << 14 | 3u << 11 | 1u << 4 | 1));
    CHECK("the word of 192 kHz", hda_format_word(192000, 16, 2) == (3u << 11 | 1u << 4 | 1));
    CHECK("no word for a rate the table does not have", !hda_format_word(12345, 16, 2));
    CHECK("no word for a size it does not have", !hda_format_word(48000, 18, 2));
}

/* A codec that answers nothing at all. */
static void silent(void) {
    const char *name = "silent";
    static const struct node nodes[] = {{.nid = 2, .type = HDA_WIDGET_OUTPUT}};
    static struct mock m = {.group = 1, .nodes = nodes, .count = 1, .silent = true};
    m.link.verb = mock_verb;
    CHECK("no codec comes of it", !hda_codec_read(&m.link, 0));
}

int main(void) {
    desktop();
    graphics();
    loops();
    formats();
    silent();
    if (failures) {
        printf("%u failures\n", failures);
        return 1;
    }
    printf("HD Audio codecs: all cases hold\n");
    return 0;
}
