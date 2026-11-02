#pragma once
/* The codecs of Intel High Definition Audio (High Definition Audio Specification 1.0a, chapter 7):
   what a codec says it is, read through its verbs alone. It knows no controller: dev/hda.c gives it
   a link to send verbs over, and tests/check_hda_codec.py runs it against codecs made up on the
   host. No product's ID decides anything.

   Each function group's widgets are read once (hda_codec_read). An output is a pin the configuration
   default names as one, with the shortest way back through mixers and selectors to an output
   converter that takes 48 kHz PCM (the rate 7.3.4.7 says every codec has). Pins grouped by a default
   association play the channels of one stream: of those of one kind only the lowest sequence is an
   output, as an output here is at most a front pair; association 15 (and 0, a default never filled in, 7.3.3.31)
   groups nothing, and neither headphones nor the pins to separate displays are channels of another. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hda_link {
    /* One verb (3.7): the 20 bits of command and payload for a node of the codec at this address, and
       the response. 0 when no response came, as a node answers what it has no use for (7.3.1). */
    uint32_t (*verb)(struct hda_link *link, unsigned codec, unsigned nid, uint32_t command);
};

/* Zeroed memory, NULL when there is none; never given back: a codec's description lives as long
   as its controller. The caller's to provide. */
void *hda_alloc(size_t bytes);

enum hda_widget_type {
    HDA_WIDGET_OUTPUT = 0,
    HDA_WIDGET_INPUT = 1,
    HDA_WIDGET_MIXER = 2,
    HDA_WIDGET_SELECTOR = 3,
    HDA_WIDGET_PIN = 4,
};

struct hda_widget {
    uint16_t nid;
    uint8_t type;
    uint32_t caps;     /* Audio Widget Capabilities (7.3.4.6) */
    uint32_t pin_caps; /* a pin's (7.3.4.9) */
    uint32_t config;   /* a pin's Configuration Default (7.3.3.31) */
    uint32_t amp_in, amp_out; /* Amplifier Capabilities (7.3.4.10), the group's unless overridden */
    uint32_t pcm, formats;    /* a converter's Supported PCM Size, Rates and Stream Formats, likewise */
    uint16_t *inputs;  /* its Connection List with its ranges spelled out (7.1.2) */
    unsigned ninputs;
};

struct hda_group {
    struct hda_codec *codec;
    struct hda_group *next;
    uint16_t nid;
    uint8_t output_delay; /* for the whole group, 0 when each widget says its own (7.3.4.5) */
    unsigned nwidgets;
    struct hda_widget *widgets;
    unsigned *search_queue, *search_input; /* find_path's */
    int *search_from;
};

struct hda_codec {
    struct hda_link *link;
    unsigned address;
    struct hda_group *groups;
};

enum hda_output_kind {
    HDA_OUTPUT_SPEAKER,
    HDA_OUTPUT_HEADPHONE,
    HDA_OUTPUT_LINE,
    HDA_OUTPUT_DISPLAY, /* a pin to HDMI or DisplayPort */
    HDA_OUTPUT_DIGITAL, /* S/PDIF and the other digital ones */
    HDA_OUTPUT_OTHER,
};

struct hda_output {
    struct hda_output *next;
    struct hda_group *group;
    struct hda_widget *pin, *converter;
    /* From the pin to the converter; via[i]: the input of path[i] that path[i + 1] is. */
    struct hda_widget **path;
    unsigned *via;
    unsigned length;
    enum hda_output_kind kind;
    bool senses;       /* something plugged in can be told (presence detect that nothing overrides) */
    bool announces;    /* ... and the pin says so by itself (an unsolicited response) */
    uint16_t format;   /* the stream format's word it plays (3.7.1): at first 48 kHz and its narrowest size */
    uint8_t channels;  /* 2, or 1 for a converter of one */
    uint8_t delay;     /* samples from the link to the pin: the group's, else its way's (7.3.4.5, 7.3.4.6) */
    bool plugged;      /* as last sensed, with a display's ELD valid too (figure 72) */
    bool displayport;  /* a display's ELD says so */
    char name[32];
};

/* The codec's function groups and widgets; NULL without memory, or when it has no audio group. */
struct hda_codec *hda_codec_read(struct hda_link *link, unsigned address);
/* Its outputs, in the order of its pins; NULL when it has none, or without memory. */
struct hda_output *hda_codec_outputs(struct hda_codec *codec);
/* After a reset of the link, and at first: the audio groups powered (D0), and the pins that sense
   asked to announce what is plugged in, with this tag in their unsolicited responses. */
void hda_codec_power(struct hda_codec *codec, const struct hda_output *outputs, unsigned tag);
/* What is plugged in now, kept in out->plugged, and its name; true when either changed. */
bool hda_output_sense(struct hda_output *out);
/* What its converter declares (7.3.4.7) that a stream format's word can say: the rates in Hz, in rising
   order, of R1 to R11 (R12, 384 kHz, has no MULT in table 53), and the sizes in bits, in rising order, of 16,
   20, 24 and 32 (8 bits share no container rule with them, and 32 on a digital converter would be raw
   S/PDIF frames, 7.3.3.9). Each returns how many. */
unsigned hda_output_rates(const struct hda_output *out, uint32_t *rates);
unsigned hda_output_sizes(const struct hda_output *out, uint8_t *bits);
/* The stream format's word (3.7.1) of PCM at this rate, of these bits and channels; 0 with no such word.
   No word it can give is 0: every size's code is. */
uint16_t hda_format_word(uint32_t rate, unsigned bits, unsigned channels);
uint16_t hda_output_format(const struct hda_output *out);
/* The format's word the next hda_output_enable gives the converter. */
void hda_output_select(struct hda_output *out, uint16_t format);
/* The stream with this tag plays on it: the converter takes the format and the tag (channel 0), the
   way is selected, its amplifiers at 0 dB and open, the other inputs of its mixers shut, the pin on. */
void hda_output_enable(struct hda_output *out, unsigned tag);
/* No more of any stream comes through: the pin off, the converter on no stream. */
void hda_output_disable(struct hda_output *out);
