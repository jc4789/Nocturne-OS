/* The codecs of High Definition Audio (see hda_codec.h). Section numbers are the specification's,
   revision 1.0a. */
#include "dev/hda_codec.h"

/* A verb is 12 bits and an 8-bit payload, or 4 bits and a 16-bit one (7.3). */
#define VERB12(v, payload) ((uint32_t)(v) << 8 | (uint8_t)(payload))
#define VERB4(v, payload)  ((uint32_t)(v) << 16 | (uint16_t)(payload))

#define GET_PARAMETER     0xF00
#define SET_SELECT        0x701
#define GET_CONNECTIONS   0xF02
#define SET_POWER         0x705
#define SET_STREAM        0x706
#define SET_PIN_CONTROL   0x707
#define SET_UNSOLICITED   0x708
#define GET_SENSE         0xF09
#define GET_EAPD          0xF0C
#define SET_EAPD          0x70C
#define SET_DIGITAL       0x70D
#define GET_CONFIG        0xF1C
#define SET_CHANNELS      0x72D
#define GET_DIP_SIZE      0xF2E
#define GET_ELD           0xF2F
#define SET_DIP_INDEX     0x730
#define SET_DIP_DATA      0x731
#define SET_DIP_TRANSMIT  0x732
#define SET_ASP_MAP       0x734
#define SET_FORMAT        0x2   /* 4-bit */
#define SET_AMP           0x3   /* 4-bit */

#define PARAM_NODES       0x04
#define PARAM_GROUP_TYPE  0x05
#define PARAM_GROUP_CAPS  0x08
#define PARAM_WIDGET_CAPS 0x09
#define PARAM_PCM         0x0A
#define PARAM_FORMATS     0x0B
#define PARAM_PIN_CAPS    0x0C
#define PARAM_AMP_IN      0x0D
#define PARAM_CONNECTIONS 0x0E
#define PARAM_AMP_OUT     0x12

#define GROUP_AUDIO 0x01

/* Audio Widget Capabilities (7.3.4.6) */
#define CAPS_TYPE(c)      (((c) >> 20) & 0xF)
#define CAPS_DELAY(c)     (((c) >> 16) & 0xF)
#define CAPS_STEREO       (1u << 0)
#define CAPS_AMP_IN       (1u << 1)
#define CAPS_AMP_OUT      (1u << 2)
#define CAPS_AMP_OVERRIDE (1u << 3)
#define CAPS_FORMAT_OVERRIDE (1u << 4)
#define CAPS_UNSOLICITED  (1u << 7)
#define CAPS_CONNECTIONS  (1u << 8)
#define CAPS_DIGITAL      (1u << 9)
#define CAPS_POWER        (1u << 10)
#define CAPS_CHANNELS(c)  ((((c) >> 13 & 7) << 1 | ((c) & 1)) + 1)

/* Pin Capabilities (7.3.4.9) */
#define PIN_PRESENCE  (1u << 2)
#define PIN_HEADPHONE (1u << 3)
#define PIN_OUTPUT    (1u << 4)
#define PIN_HDMI      (1u << 7)
#define PIN_EAPD      (1u << 16)
#define PIN_DP        (1u << 24)

/* Pin Widget Control (7.3.3.13) */
#define CONTROL_OUT       (1u << 6)
#define CONTROL_HEADPHONE (1u << 7)

/* Configuration Default (7.3.3.31) */
#define CONFIG_PORT(c)     ((c) >> 30)
#define CONFIG_LOCATION(c) (((c) >> 24) & 0x3F)
#define CONFIG_DEVICE(c)   (((c) >> 20) & 0xF)
#define CONFIG_NO_DETECT(c) (((c) >> 8) & 1)
#define CONFIG_ASSOC(c)    (((c) >> 4) & 0xF)
#define CONFIG_SEQUENCE(c) ((c) & 0xF)
#define PORT_JACK 0
#define PORT_NONE 1
#define DEVICE_LINE 0x0
#define DEVICE_SPEAKER 0x1
#define DEVICE_HEADPHONE 0x2
#define DEVICE_SPDIF 0x4
#define DEVICE_DIGITAL 0x5

/* Supported PCM Size, Rates (7.3.4.7) */
#define PCM_48K  (1u << 6)
#define PCM_B16  (1u << 17)
#define PCM_B20  (1u << 18)
#define PCM_B24  (1u << 19)
#define PCM_B32  (1u << 20)
#define FORMATS_PCM 1u

/* Amplifier Capabilities (7.3.4.10) and its Set payload (figure 64) */
#define AMP_OFFSET(a) ((a) & 0x7F)
#define AMP_STEPS(a)  (((a) >> 8) & 0x7F)
#define AMP_SET_OUT   (1u << 15)
#define AMP_SET_IN    (1u << 14)
#define AMP_SET_LR    (3u << 12)
#define AMP_INDEX(i)  ((uint32_t)(i) << 8)
#define AMP_MUTE      (1u << 7)

static uint32_t verb(const struct hda_group *g, unsigned nid, uint32_t command) {
    return g->codec->link->verb(g->codec->link, g->codec->address, nid, command);
}
static uint32_t parameter(const struct hda_group *g, unsigned nid, unsigned id) {
    return verb(g, nid, VERB12(GET_PARAMETER, id));
}

/* ---- reading a function group ---- */

/* The Connection List (7.3.3.3) with its ranges spelled out (7.1.2): an entry with the high bit
   set closes a range the entry before it opened. */
static bool read_inputs(const struct hda_group *g, struct hda_widget *w) {
    uint32_t length = parameter(g, w->nid, PARAM_CONNECTIONS);
    bool long_form = length & 0x80;
    unsigned entries = length & 0x7F, per = long_form ? 2 : 4, bits = long_form ? 16 : 8;
    if (!entries) return true;
    uint16_t *raw = hda_alloc(entries * sizeof *raw);
    if (!raw) return false;
    uint32_t mask = long_form ? 0xFFFF : 0xFF;
    uint32_t spell = 0;
    uint16_t range = long_form ? 0x8000 : 0x80, nid_mask = range - 1;
    for (unsigned i = 0; i < entries; i += per) {
        uint32_t got = verb(g, w->nid, VERB12(GET_CONNECTIONS, i));
        for (unsigned k = 0; k < per && i + k < entries; k++) raw[i + k] = (uint16_t)(got >> (k * bits) & mask);
    }
    for (unsigned i = 0; i < entries; i++) {
        if (!(raw[i] & range) || !i) {
            spell++;
            continue;
        }
        uint16_t from = raw[i - 1] & nid_mask, to = raw[i] & nid_mask;
        if (to > from) spell += (uint32_t)(to - from);
    }
    w->inputs = hda_alloc(spell * sizeof *w->inputs);
    if (!w->inputs) return false;
    for (unsigned i = 0; i < entries; i++) {
        uint16_t nid = raw[i] & nid_mask;
        if (!(raw[i] & range) || !i) {
            w->inputs[w->ninputs++] = nid;
            continue;
        }
        for (uint16_t from = raw[i - 1] & nid_mask; from < nid; from++) w->inputs[w->ninputs++] = (uint16_t)(from + 1);
    }
    return true;
}

static struct hda_group *read_group(struct hda_codec *codec, unsigned nid) {
    struct hda_group *g = hda_alloc(sizeof *g);
    if (!g) return NULL;
    g->codec = codec;
    g->nid = (uint16_t)nid;
    uint32_t nodes = parameter(g, nid, PARAM_NODES);
    unsigned first = nodes >> 16 & 0xFF;
    g->nwidgets = nodes & 0xFF;
    g->output_delay = (uint8_t)(parameter(g, nid, PARAM_GROUP_CAPS) & 0xF);
    if (!g->nwidgets) return g;
    g->widgets = hda_alloc(g->nwidgets * sizeof *g->widgets);
    if (!g->widgets) return NULL;
    uint32_t amp_in = parameter(g, nid, PARAM_AMP_IN), amp_out = parameter(g, nid, PARAM_AMP_OUT);
    uint32_t pcm = parameter(g, nid, PARAM_PCM), formats = parameter(g, nid, PARAM_FORMATS);
    for (unsigned i = 0; i < g->nwidgets; i++) {
        struct hda_widget *w = &g->widgets[i];
        w->nid = (uint16_t)(first + i);
        w->caps = parameter(g, w->nid, PARAM_WIDGET_CAPS);
        w->type = (uint8_t)CAPS_TYPE(w->caps);
        bool amp = w->caps & CAPS_AMP_OVERRIDE, format = w->caps & CAPS_FORMAT_OVERRIDE;
        w->amp_in = amp ? parameter(g, w->nid, PARAM_AMP_IN) : amp_in;
        w->amp_out = amp ? parameter(g, w->nid, PARAM_AMP_OUT) : amp_out;
        if (w->type == HDA_WIDGET_OUTPUT) {
            w->pcm = format ? parameter(g, w->nid, PARAM_PCM) : pcm;
            w->formats = format ? parameter(g, w->nid, PARAM_FORMATS) : formats;
        }
        if (w->type == HDA_WIDGET_PIN) {
            w->pin_caps = parameter(g, w->nid, PARAM_PIN_CAPS);
            w->config = verb(g, w->nid, VERB12(GET_CONFIG, 0));
        }
        if ((w->caps & CAPS_CONNECTIONS) && !read_inputs(g, w)) return NULL;
    }
    return g;
}

struct hda_codec *hda_codec_read(struct hda_link *link, unsigned address) {
    struct hda_codec *codec = hda_alloc(sizeof *codec);
    if (!codec) return NULL;
    codec->link = link;
    codec->address = address;
    struct hda_group root = {.codec = codec};
    uint32_t nodes = parameter(&root, 0, PARAM_NODES);
    unsigned first = nodes >> 16 & 0xFF, count = nodes & 0xFF;
    struct hda_group **tail = &codec->groups;
    for (unsigned i = 0; i < count; i++) {
        if ((parameter(&root, first + i, PARAM_GROUP_TYPE) & 0xFF) != GROUP_AUDIO) continue;
        struct hda_group *g = read_group(codec, first + i);
        if (!g) return NULL;
        *tail = g;
        tail = &g->next;
    }
    return codec->groups ? codec : NULL;
}

/* ---- the outputs ---- */

static struct hda_widget *widget_of(const struct hda_group *g, unsigned nid) {
    unsigned first = g->nwidgets ? g->widgets[0].nid : 0;
    return nid >= first && nid - first < g->nwidgets ? &g->widgets[nid - first] : NULL;
}

/* A converter's format: the rate every codec has, in the mix's 16 bits where it has them, else in the
   narrowest wider size it has. 32-bit samples on a digital converter would be raw S/PDIF frames
   (7.3.3.9), so never those. 0 when it plays no PCM at that rate. */
static unsigned converter_bits(const struct hda_widget *w) {
    if (!(w->formats & FORMATS_PCM) || !(w->pcm & PCM_48K)) return 0;
    if (w->pcm & PCM_B16) return 16;
    if (w->pcm & PCM_B20) return 20;
    if (w->pcm & PCM_B24) return 24;
    if ((w->pcm & PCM_B32) && !(w->caps & CAPS_DIGITAL)) return 32;
    return 0;
}

static bool converter_fits(const struct hda_widget *converter, const struct hda_widget *pin) {
    return !(converter->caps & CAPS_DIGITAL) == !(pin->caps & CAPS_DIGITAL) && converter_bits(converter);
}

/* The shortest way from the pin back to a converter that fits, breadth first: the graph may have
   cycles (7.1.3 forbids them only in what is enabled at once), and each widget is seen once. */
static bool find_path(struct hda_output *o) {
    struct hda_group *g = o->group;
    unsigned n = g->nwidgets, head = 0, tail = 0;
    if (!g->search_queue) { /* one output's search at a time, each for a whole group's widgets */
        g->search_queue = hda_alloc(n * sizeof *g->search_queue);
        g->search_from = hda_alloc(n * sizeof *g->search_from);
        g->search_input = hda_alloc(n * sizeof *g->search_input);
    }
    unsigned *queue = g->search_queue, *input = g->search_input;
    int *from = g->search_from;
    if (!queue || !from || !input) return false;
    for (unsigned i = 0; i < n; i++) from[i] = -1;
    unsigned start = (unsigned)(o->pin - g->widgets);
    from[start] = (int)start;
    queue[tail++] = start;
    while (head < tail) {
        unsigned at = queue[head++];
        const struct hda_widget *w = &g->widgets[at];
        if (w->type == HDA_WIDGET_OUTPUT) {
            if (!converter_fits(w, o->pin)) continue;
            unsigned length = 1;
            for (unsigned i = at; i != start; i = (unsigned)from[i]) length++;
            o->path = hda_alloc(length * sizeof *o->path);
            o->via = hda_alloc(length * sizeof *o->via);
            if (!o->path || !o->via) return false;
            o->length = length;
            unsigned k = length - 1, i = at;
            for (;;) {
                o->path[k] = &g->widgets[i];
                if (i == start) break;
                k--;
                o->via[k] = input[i];
                i = (unsigned)from[i];
            }
            o->converter = &g->widgets[at];
            return true;
        }
        for (unsigned k = 0; k < w->ninputs; k++) {
            const struct hda_widget *next = widget_of(g, w->inputs[k]);
            if (!next) continue;
            if (next->type != HDA_WIDGET_OUTPUT && next->type != HDA_WIDGET_MIXER && next->type != HDA_WIDGET_SELECTOR) continue;
            unsigned index = (unsigned)(next - g->widgets);
            if (from[index] >= 0) continue;
            from[index] = (int)at;
            input[index] = k;
            queue[tail++] = index;
        }
    }
    return false;
}

static bool output_kind(const struct hda_widget *pin, enum hda_output_kind *kind) {
    if (pin->pin_caps & (PIN_HDMI | PIN_DP)) {
        *kind = HDA_OUTPUT_DISPLAY;
        return true;
    }
    switch (CONFIG_DEVICE(pin->config)) {
    case DEVICE_LINE: *kind = HDA_OUTPUT_LINE; return true;
    case DEVICE_SPEAKER: *kind = HDA_OUTPUT_SPEAKER; return true;
    case DEVICE_HEADPHONE: *kind = HDA_OUTPUT_HEADPHONE; return true;
    case DEVICE_SPDIF:
    case DEVICE_DIGITAL: *kind = HDA_OUTPUT_DIGITAL; return true;
    default: return false; /* the pin's use is an input's, or none named */
    }
}

/* Of the pins of one association and kind only the lowest sequence plays the front pair. */
static bool first_of_association(const struct hda_group *g, const struct hda_widget *pin, enum hda_output_kind kind) {
    unsigned assoc = CONFIG_ASSOC(pin->config);
    if (!assoc || assoc == 15) return true;
    for (unsigned i = 0; i < g->nwidgets; i++) {
        const struct hda_widget *w = &g->widgets[i];
        enum hda_output_kind other;
        if (w == pin || w->type != HDA_WIDGET_PIN || !(w->pin_caps & PIN_OUTPUT)) continue;
        if (CONFIG_PORT(w->config) == PORT_NONE || CONFIG_ASSOC(w->config) != assoc) continue;
        if (!output_kind(w, &other) || other != kind) continue;
        if (CONFIG_SEQUENCE(w->config) < CONFIG_SEQUENCE(pin->config)) return false;
        if (CONFIG_SEQUENCE(w->config) == CONFIG_SEQUENCE(pin->config) && w < pin) return false;
    }
    return true;
}

static void put(char *name, const char *text) {
    size_t at = 0;
    while (at < 31 && name[at]) at++;
    while (at < 31 && *text) name[at++] = *text++;
    name[at] = 0;
}

/* A name for people, from what the pin says of itself: where (7.3.3.31, table 110) and what. */
static void name_of(struct hda_output *o, const char *monitor) {
    unsigned location = CONFIG_LOCATION(o->pin->config);
    o->name[0] = 0;
    if (o->kind == HDA_OUTPUT_DISPLAY) {
        put(o->name, !monitor ? "Display audio" : o->displayport ? "DisplayPort: " : "HDMI: ");
        if (monitor) put(o->name, monitor);
        return;
    }
    if (o->kind == HDA_OUTPUT_SPEAKER) put(o->name, "Speakers");
    else {
        if ((location >> 4) == 0 && (location & 15) == 1) put(o->name, "Rear ");
        if ((location >> 4) == 0 && (location & 15) == 2) put(o->name, "Front ");
        put(o->name, o->kind == HDA_OUTPUT_HEADPHONE ? "headphones" : o->kind == HDA_OUTPUT_LINE ? "line out" :
                     o->kind == HDA_OUTPUT_DIGITAL ? "digital out" : "output");
    }
}

static bool output_of(struct hda_group *g, struct hda_widget *pin, struct hda_output **out) {
    enum hda_output_kind kind;
    *out = NULL;
    if (pin->type != HDA_WIDGET_PIN || !(pin->pin_caps & PIN_OUTPUT)) return true;
    if (CONFIG_PORT(pin->config) == PORT_NONE || !output_kind(pin, &kind)) return true;
    if (kind != HDA_OUTPUT_HEADPHONE && kind != HDA_OUTPUT_DISPLAY && !first_of_association(g, pin, kind)) return true;
    struct hda_output *o = hda_alloc(sizeof *o);
    if (!o) return false;
    o->group = g;
    o->pin = pin;
    o->kind = kind;
    o->senses = CONFIG_PORT(pin->config) == PORT_JACK && (pin->pin_caps & PIN_PRESENCE) && !CONFIG_NO_DETECT(pin->config);
    o->announces = o->senses && (pin->caps & CAPS_UNSOLICITED);
    if (!find_path(o)) return true; /* no converter reaches it: not an output */
    o->bits = (uint8_t)converter_bits(o->converter);
    o->channels = CAPS_CHANNELS(o->converter->caps) >= 2 ? 2 : 1;
    o->delay = g->output_delay;
    for (unsigned i = 0; !g->output_delay && i < o->length; i++) o->delay = (uint8_t)(o->delay + CAPS_DELAY(o->path[i]->caps));
    name_of(o, NULL);
    *out = o;
    return true;
}

struct hda_output *hda_codec_outputs(struct hda_codec *codec) {
    struct hda_output *first = NULL, **tail = &first;
    for (struct hda_group *g = codec->groups; g; g = g->next)
        for (unsigned i = 0; i < g->nwidgets; i++) {
            struct hda_output *o;
            if (!output_of(g, &g->widgets[i], &o)) return first;
            if (!o) continue;
            *tail = o;
            tail = &o->next;
        }
    return first;
}

void hda_codec_power(struct hda_codec *codec, const struct hda_output *outputs, unsigned tag) {
    for (struct hda_group *g = codec->groups; g; g = g->next) verb(g, g->nid, VERB12(SET_POWER, 0));
    for (const struct hda_output *o = outputs; o; o = o->next)
        if (o->announces) verb(o->group, o->pin->nid, VERB12(SET_UNSOLICITED, 0x80 | (tag & 0x3F)));
}

/* ---- sensing ---- */

/* ELD (7.3.3.34): the connection type and the monitor's name, when the sink's is valid. */
static void read_eld(struct hda_output *o, char *monitor, size_t room) {
    const struct hda_group *g = o->group;
    unsigned nid = o->pin->nid;
#define ELD_BYTE(at) verb(g, nid, VERB12(GET_ELD, at))
    uint32_t first = ELD_BYTE(4), type = ELD_BYTE(7);
    if (!(first >> 31) || !(type >> 31)) return;
    o->displayport = (type >> 2 & 3) == 1; /* table 121 */
    unsigned length = first & 0x1F;
    if (length > 16 || length >= room) return; /* table 119 */
    for (unsigned i = 0; i < length; i++) {
        uint32_t byte = ELD_BYTE(20 + i);
        if (!(byte >> 31)) {
            monitor[0] = 0;
            return;
        }
        monitor[i] = (char)byte;
        monitor[i + 1] = 0;
    }
#undef ELD_BYTE
}

bool hda_output_sense(struct hda_output *o) {
    if (!o->senses) return false;
    uint32_t sense = verb(o->group, o->pin->nid, VERB12(GET_SENSE, 0));
    bool display = o->kind == HDA_OUTPUT_DISPLAY, plugged = sense >> 31, valid = sense >> 30 & 1;
    if (display) plugged = plugged && valid; /* figure 72: streaming is allowed then */
    char before[32], monitor[17] = "";
    unsigned same = 1;
    for (unsigned i = 0; i < sizeof before; i++) before[i] = o->name[i];
    if (display) {
        if (plugged) read_eld(o, monitor, sizeof monitor);
        name_of(o, monitor[0] ? monitor : NULL);
    }
    for (unsigned i = 0; i < sizeof before; i++) same &= before[i] == o->name[i];
    bool changed = plugged != o->plugged || !same;
    o->plugged = plugged;
    return changed;
}

/* ---- playing ---- */

uint16_t hda_output_format(const struct hda_output *o) {
    unsigned bits = o->bits == 16 ? 1 : o->bits == 20 ? 2 : o->bits == 24 ? 3 : 4;
    return (uint16_t)(bits << 4 | (unsigned)(o->channels - 1));
}

/* An amplifier's word at 0 dB and open, the offset within the steps it has (7.3.4.10). */
static uint32_t open_amp(uint32_t caps) {
    unsigned steps = AMP_STEPS(caps), offset = AMP_OFFSET(caps);
    return offset <= steps ? offset : steps;
}

/* The widgets on the way: what selects among inputs selects path[i + 1]; a mixer sums the one on the
   way and has the rest shut, as they may be loops of the analog inputs (7.2.3.4). */
static void open_widget(const struct hda_output *o, unsigned at) {
    const struct hda_widget *w = o->path[at];
    const struct hda_group *g = o->group;
    if (w->caps & CAPS_POWER) verb(g, w->nid, VERB12(SET_POWER, 0));
    bool last = at + 1 == o->length;
    if (!last && w->ninputs > 1 && w->type != HDA_WIDGET_MIXER) verb(g, w->nid, VERB12(SET_SELECT, o->via[at]));
    if ((w->caps & CAPS_AMP_IN) && w->type != HDA_WIDGET_PIN)
        for (unsigned k = 0; k < w->ninputs; k++) {
            bool on_way = !last && k == o->via[at];
            uint32_t state = on_way ? open_amp(w->amp_in) : AMP_MUTE | open_amp(w->amp_in);
            verb(g, w->nid, VERB4(SET_AMP, AMP_SET_IN | AMP_SET_LR | AMP_INDEX(k) | state));
        }
    if (w->caps & CAPS_AMP_OUT) verb(g, w->nid, VERB4(SET_AMP, AMP_SET_OUT | AMP_SET_LR | open_amp(w->amp_out)));
}

/* A display's sample packets: the stream's two channels in the first two slots, the rest unallocated
   (7.3.3.41), and the Audio InfoFrame that tells the sink so, sent as a Data Island Packet (7.3.3.36
   to 39): CEA-861's header for HDMI, the secondary data packet's for DisplayPort, then the first
   data byte, 2 channels, all else "refer to the stream". */
static void open_display(const struct hda_output *o) {
    const struct hda_group *g = o->group;
    unsigned nid = o->pin->nid;
    for (unsigned slot = 0; slot < 8; slot++) verb(g, nid, VERB12(SET_ASP_MAP, (slot < 2 ? slot : 15u) << 4 | slot));
    static const uint8_t hdmi[] = {0x84, 0x01, 0x0A, 0x70, 0x01}, displayport[] = {0x00, 0x84, 0x1B, 0x44, 0x01};
    const uint8_t *frame = o->displayport ? displayport : hdmi;
    unsigned size = (verb(g, nid, VERB12(GET_DIP_SIZE, 0)) & 0xFF) + 1;
    verb(g, nid, VERB12(SET_DIP_INDEX, 0));
    for (unsigned i = 0; i < size; i++) verb(g, nid, VERB12(SET_DIP_DATA, i < sizeof hdmi ? frame[i] : 0));
    verb(g, nid, VERB12(SET_DIP_INDEX, 0));
    verb(g, nid, VERB12(SET_DIP_TRANSMIT, 0xC0));
}

void hda_output_enable(struct hda_output *o, unsigned tag) {
    const struct hda_group *g = o->group;
    const struct hda_widget *c = o->converter, *pin = o->pin;
    verb(g, g->nid, VERB12(SET_POWER, 0));
    for (unsigned i = o->length; i-- > 0;) open_widget(o, i);
    verb(g, c->nid, VERB4(SET_FORMAT, hda_output_format(o)));
    if (CAPS_CHANNELS(c->caps) > 2) verb(g, c->nid, VERB12(SET_CHANNELS, o->channels - 1));
    verb(g, c->nid, VERB12(SET_STREAM, tag << 4));
    if (c->caps & CAPS_DIGITAL) verb(g, c->nid, VERB12(SET_DIGITAL, 1));
    if (pin->pin_caps & PIN_EAPD) verb(g, pin->nid, VERB12(SET_EAPD, verb(g, pin->nid, VERB12(GET_EAPD, 0)) | 2));
    if (o->kind == HDA_OUTPUT_DISPLAY) open_display(o);
    bool headphone = o->kind == HDA_OUTPUT_HEADPHONE && (pin->pin_caps & PIN_HEADPHONE);
    verb(g, pin->nid, VERB12(SET_PIN_CONTROL, CONTROL_OUT | (headphone ? CONTROL_HEADPHONE : 0)));
}

void hda_output_disable(struct hda_output *o) {
    const struct hda_group *g = o->group;
    verb(g, o->pin->nid, VERB12(SET_PIN_CONTROL, 0));
    if (o->kind == HDA_OUTPUT_DISPLAY) {
        verb(g, o->pin->nid, VERB12(SET_DIP_INDEX, 0));
        verb(g, o->pin->nid, VERB12(SET_DIP_TRANSMIT, 0));
    }
    if (o->converter->caps & CAPS_DIGITAL) verb(g, o->converter->nid, VERB12(SET_DIGITAL, 0));
    verb(g, o->converter->nid, VERB12(SET_STREAM, 0));
}
