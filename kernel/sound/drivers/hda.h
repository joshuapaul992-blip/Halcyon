/* Halcyon OS - Intel High Definition Audio: codec definitions shared by the
 * controller driver (hda.c) and the codec layer (hda_codec.c).
 *
 * The codec layer knows nothing about the controller: it sends verbs through
 * the hda_codec_t's cmd() callback and keeps everything it learns in the
 * hda_codec_t, which the caller allocates. That keeps it free of kernel
 * services other than kprintf and the string functions, so tests/host/hdatest.c
 * runs it against synthetic codecs on the host.
 *
 * Reference: Intel "High Definition Audio Specification", Revision 1.0a
 * (2010): section 7.3 (codec parameters and controls), 7.3.3 (verbs),
 * 7.3.4 (parameters); and Microsoft's "Pin Configuration Guidelines for High
 * Definition Audio Devices" for the meaning of the default association and
 * sequence fields. */
#ifndef SOUND_HDA_H
#define SOUND_HDA_H
#include "core/types.h"

/* ---------------- verbs (7.3.3) ---------------- */
/* 12-bit verbs with an 8-bit payload */
#define HDA_GET_PARAM           0xF00
#define HDA_GET_CONN_SELECT     0xF01
#define HDA_SET_CONN_SELECT     0x701
#define HDA_GET_CONN_LIST       0xF02
#define HDA_GET_POWER_STATE     0xF05
#define HDA_SET_POWER_STATE     0x705
#define HDA_GET_STREAM_CHANNEL  0xF06
#define HDA_SET_STREAM_CHANNEL  0x706
#define HDA_GET_PIN_CTL         0xF07
#define HDA_SET_PIN_CTL         0x707
#define HDA_GET_UNSOL           0xF08
#define HDA_SET_UNSOL           0x708
#define HDA_GET_PIN_SENSE       0xF09
#define HDA_EXEC_PIN_SENSE      0x709
#define HDA_GET_EAPD_BTL        0xF0C
#define HDA_SET_EAPD_BTL        0x70C
#define HDA_GET_DIGI_CONV       0xF0D
#define HDA_SET_DIGI_CONV1      0x70D
#define HDA_GET_CONFIG_DEFAULT  0xF1C
#define HDA_GET_SUBSYSTEM_ID    0xF20
#define HDA_SET_CVT_CHAN_COUNT  0x72D
#define HDA_GET_HDMI_ELDD       0xF2F
#define HDA_SET_HDMI_DIP_INDEX  0x730
#define HDA_SET_HDMI_DIP_DATA   0x731
#define HDA_SET_HDMI_DIP_XMIT   0x732
#define HDA_FUNCTION_RESET      0x7FF
/* 4-bit verbs with a 16-bit payload (the verb in bits 19:16) */
#define HDA_SET_FORMAT          0x2
#define HDA_SET_AMP             0x3
#define HDA_GET_FORMAT          0xA
#define HDA_GET_AMP             0xB

/* ---------------- parameters (7.3.4) ---------------- */
#define HDA_PAR_VENDOR_ID       0x00
#define HDA_PAR_REVISION_ID     0x02
#define HDA_PAR_NODE_COUNT      0x04    /* 23:16 first node, 7:0 count */
#define HDA_PAR_FG_TYPE         0x05    /* 7:0 type, bit 8 unsolicited capable */
#define HDA_PAR_AFG_CAPS        0x08
#define HDA_PAR_WIDGET_CAPS     0x09
#define HDA_PAR_PCM             0x0A    /* rates 11:0, sizes 20:16 */
#define HDA_PAR_STREAM_FORMATS  0x0B
#define HDA_PAR_PIN_CAPS        0x0C
#define HDA_PAR_AMP_IN_CAPS     0x0D
#define HDA_PAR_CONN_LEN        0x0E    /* bit 7 long form, 6:0 length */
#define HDA_PAR_POWER_STATES    0x0F
#define HDA_PAR_GPIO_COUNT      0x11
#define HDA_PAR_AMP_OUT_CAPS    0x12

#define HDA_FG_AUDIO            0x01
#define HDA_FG_MODEM            0x02
#define HDA_FG_UNSOL            (1u << 8)

/* audio widget capabilities */
#define HDA_WCAP_TYPE(c)        (((c) >> 20) & 0xF)
#define HDA_WCAP_STEREO         (1u << 0)
#define HDA_WCAP_IN_AMP         (1u << 1)
#define HDA_WCAP_OUT_AMP        (1u << 2)
#define HDA_WCAP_AMP_OVRD       (1u << 3)
#define HDA_WCAP_FORMAT_OVRD    (1u << 4)
#define HDA_WCAP_UNSOL          (1u << 7)
#define HDA_WCAP_CONN_LIST      (1u << 8)
#define HDA_WCAP_DIGITAL        (1u << 9)
#define HDA_WCAP_POWER          (1u << 10)
enum { HDA_W_OUTPUT = 0, HDA_W_INPUT = 1, HDA_W_MIXER = 2, HDA_W_SELECTOR = 3, HDA_W_PIN = 4,
       HDA_W_POWER = 5, HDA_W_VOLKNOB = 6, HDA_W_BEEP = 7, HDA_W_VENDOR = 0xF };

/* pin capabilities */
#define HDA_PINCAP_IMP_SENSE    (1u << 0)
#define HDA_PINCAP_TRIGGER      (1u << 1)
#define HDA_PINCAP_PRESENCE     (1u << 2)
#define HDA_PINCAP_HP_DRIVE     (1u << 3)
#define HDA_PINCAP_OUT          (1u << 4)
#define HDA_PINCAP_IN           (1u << 5)
#define HDA_PINCAP_HDMI         (1u << 7)
#define HDA_PINCAP_EAPD         (1u << 16)
#define HDA_PINCAP_DP           (1u << 24)

/* pin widget control */
#define HDA_PINCTL_HP           0x80
#define HDA_PINCTL_OUT          0x40
#define HDA_PINCTL_IN           0x20

/* EAPD/BTL enable */
#define HDA_EAPD                0x02

/* pin sense */
#define HDA_SENSE_PRESENCE      (1u << 31)
#define HDA_SENSE_ELDV          (1u << 30)

/* amplifier capabilities */
#define HDA_AMPCAP_MUTE         (1u << 31)
#define HDA_AMPCAP_STEP_QDB(c)  ((((c) >> 16) & 0x7F) + 1)   /* step size in 0.25 dB */
#define HDA_AMPCAP_STEPS(c)     (((c) >> 8) & 0x7F)          /* highest gain value */
#define HDA_AMPCAP_OFFSET(c)    ((c) & 0x7F)                 /* gain value of 0 dB */

/* set amplifier gain/mute payload */
#define HDA_AMP_OUT             (1u << 15)
#define HDA_AMP_IN              (1u << 14)
#define HDA_AMP_LEFT            (1u << 13)
#define HDA_AMP_RIGHT           (1u << 12)
#define HDA_AMP_INDEX(i)        ((u32)(i) << 8)
#define HDA_AMP_MUTE            (1u << 7)

/* configuration default (7.3.3.31) */
#define HDA_CFG_CONN(c)         (((c) >> 30) & 3)    /* 0 jack, 1 none, 2 fixed, 3 both */
#define HDA_CFG_LOCATION(c)     (((c) >> 24) & 0x3F)
#define HDA_CFG_DEVICE(c)       (((c) >> 20) & 0xF)
#define HDA_CFG_CONNTYPE(c)     (((c) >> 16) & 0xF)
#define HDA_CFG_COLOR(c)        (((c) >> 12) & 0xF)
#define HDA_CFG_MISC(c)         (((c) >> 8) & 0xF)
#define HDA_CFG_ASSOC(c)        (((c) >> 4) & 0xF)
#define HDA_CFG_SEQ(c)          ((c) & 0xF)
#define HDA_CFG_NO_DETECT       0x1              /* misc bit 0: jack detect override */
enum { HDA_CONN_JACK = 0, HDA_CONN_NONE = 1, HDA_CONN_FIXED = 2, HDA_CONN_BOTH = 3 };
enum { HDA_DEV_LINE_OUT = 0, HDA_DEV_SPEAKER = 1, HDA_DEV_HP = 2, HDA_DEV_CD = 3, HDA_DEV_SPDIF_OUT = 4,
       HDA_DEV_DIG_OUT = 5, HDA_DEV_LINE_IN = 8, HDA_DEV_AUX = 9, HDA_DEV_MIC = 0xA, HDA_DEV_SPDIF_IN = 0xC,
       HDA_DEV_DIG_IN = 0xD, HDA_DEV_OTHER = 0xF };

/* stream format: 48 kHz, 16 bits, 2 channels (PCM, base 48 kHz, x1 /1) */
#define HDA_FMT_48K_16_2        0x0011

/* ---------------- what the codec layer keeps ---------------- */
#define HDA_MAX_WIDGETS         160
#define HDA_MAX_CONN            32
#define HDA_PATH_MAX            8       /* widgets in a path, DAC to pin */
#define HDA_MAX_PATHS           8

/* what a path drives */
enum { HDA_OUT_SPEAKER = 0, HDA_OUT_LINE, HDA_OUT_HP, HDA_OUT_HDMI, HDA_OUT_DP, HDA_OUT_SPDIF };

typedef struct hda_widget {
    u16 nid;
    u8 type;                    /* HDA_W_* */
    u8 nconn;
    u32 caps;                   /* audio widget capabilities */
    u32 pincap, cfg;            /* pins: capabilities, configuration default */
    u32 ampin, ampout;          /* amplifier capabilities (the function group's unless overridden) */
    u32 pcm;                    /* converters: supported rates and sizes */
    u16 conn[HDA_MAX_CONN];     /* connection list (inputs) */
    u8 used;                    /* on a path */
    u8 sel_claimed;             /* selector / pin: a path set its connection select */
    u8 sel;                     /* ...to this index */
} hda_widget_t;

typedef struct hda_path {
    int kind;                   /* HDA_OUT_* */
    int len;
    u16 nid[HDA_PATH_MAX];      /* nid[0] the converter ... nid[len-1] the pin */
    u8 idx[HDA_PATH_MAX];       /* idx[i]: index of nid[i-1] in nid[i]'s connection list */
    u16 vol_nid;                /* amp used for the volume (0: none) */
    u8 vol_out;                 /* ...its output amp (else input amp vol_idx) */
    u8 vol_idx;
    u32 vol_caps;
    u16 mute_nid;               /* amp used to mute (0: none) */
    u8 mute_out, mute_idx;
    u8 detect;                  /* the pin reports presence */
    u8 unsol;                   /* ...through unsolicited responses (tag = path index + 1) */
    u8 present;                 /* last presence state (1 when it cannot detect) */
    u8 jack_muted;              /* muted because headphones are plugged in */
    u8 assoc, seq;
} hda_path_t;

typedef struct hda_codec hda_codec_t;
struct hda_codec {
    /* filled in by the caller */
    int cad;                    /* codec address 0..14 */
    int (*cmd)(void *ctx, u32 verb, u32 *resp);     /* 0 or E_*; one verb, its response */
    void *ctx;
    void (*delay)(int ms);      /* may be NULL */
    const char *log;            /* log prefix ("hda 00:1b.0") */
    /* learned by hda_codec_probe */
    u32 vendor_id, revision, subsystem;
    char name[40];              /* "Realtek ALC892" */
    u16 afg;                    /* audio function group node (0: none) */
    u8 afg_unsol;
    u8 modem;                   /* has a modem function group */
    u32 afg_ampin, afg_ampout;
    u16 wstart;
    int nw;
    hda_widget_t w[HDA_MAX_WIDGETS];
    /* found by hda_codec_find_paths */
    hda_path_t path[HDA_MAX_PATHS];
    int npaths;
    int digital;                /* the paths are digital (HDMI/DP/S/PDIF): no analog output */
    int hw_volume;              /* every path has an amplifier with enough range */
    int hp_plugged;
    int verbs_failed;
};

/* encode a verb */
static inline u32 hda_verb(int cad, int nid, u32 verb, u32 payload) {
    return (u32)cad << 28 | (u32)(nid & 0x7F) << 20 | verb << 8 | (payload & 0xFF);
}
static inline u32 hda_verb4(int cad, int nid, u32 verb, u32 payload) {
    return (u32)cad << 28 | (u32)(nid & 0x7F) << 20 | verb << 16 | (payload & 0xFFFF);
}

/* read the codec's function groups, widgets, connections and pin
 * configurations. 0, or E_NODEV when it has no audio function group. */
int  hda_codec_probe(hda_codec_t *c);
/* choose the output paths (analog; digital only when allow_digital and
 * there is no analog output). Returns the number of paths. */
int  hda_codec_find_paths(hda_codec_t *c, int allow_digital);
/* power the codec up and program the paths: amps, selectors, pin
 * controls, EAPD, converter format; jack detection. 0 or E_*. */
int  hda_codec_setup(hda_codec_t *c, u16 format);
/* bind the paths' converters to stream tag (0 = stop) channel 0 */
void hda_codec_stream(hda_codec_t *c, int tag, u16 format);
/* volume 0..100 and mute through the paths' amplifiers (c->hw_volume) */
void hda_codec_set_volume(hda_codec_t *c, int percent, int mute);
/* re-read the jacks (presence detect) and mute or unmute the speakers.
 * Returns 1 if anything changed. */
int  hda_codec_jacks(hda_codec_t *c);
/* an unsolicited response: 1 if it was one of ours (a jack event) */
int  hda_codec_unsol(hda_codec_t *c, u32 resp);
/* what plays now, e.g. "Speakers" or "Line out, Headphones" */
void hda_codec_describe(const hda_codec_t *c, char *buf, int n);
/* log the widget graph and the chosen paths */
void hda_codec_dump(const hda_codec_t *c);
const char *hda_out_name(int kind);
/* the gain value (0..steps) for percent of the volume on an amplifier */
int  hda_volume_gain(u32 ampcaps, int percent);

#endif
