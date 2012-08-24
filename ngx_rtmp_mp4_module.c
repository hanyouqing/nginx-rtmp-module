/*
 * Copyright (c) 2012 Roman Arutyunyan
 */


#include "ngx_rtmp_cmd_module.h"
#include "ngx_rtmp_live_module.h"
#include "ngx_rtmp_codec_module.h"


static ngx_rtmp_play_pt                 next_play;
static ngx_rtmp_close_stream_pt         next_close_stream;
static ngx_rtmp_seek_pt                 next_seek;
static ngx_rtmp_pause_pt                next_pause;


static ngx_int_t ngx_rtmp_mp4_postconfiguration(ngx_conf_t *cf);
static void * ngx_rtmp_mp4_create_app_conf(ngx_conf_t *cf);
static char * ngx_rtmp_mp4_merge_app_conf(ngx_conf_t *cf, 
       void *parent, void *child);
static void ngx_rtmp_mp4_send(ngx_event_t *e);
static ngx_int_t ngx_rtmp_mp4_start(ngx_rtmp_session_t *s, ngx_int_t offset);
static ngx_int_t ngx_rtmp_mp4_stop(ngx_rtmp_session_t *s);


typedef struct {
    ngx_str_t                           root;
} ngx_rtmp_mp4_app_conf_t;


#pragma pack(push,4)


typedef struct {
    uint32_t                            first_chunk;
    uint32_t                            samples_per_chunk;
    uint32_t                            sample_descrption_index;
} ngx_rtmp_mp4_chunk_entry_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    ngx_rtmp_mp4_chunk_entry_t          entries[0];
} ngx_rtmp_mp4_chunks_t;


typedef struct {
    uint32_t                            sample_count;
    uint32_t                            sample_delta;
} ngx_rtmp_mp4_time_entry_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    ngx_rtmp_mp4_time_entry_t           entries[0];
} ngx_rtmp_mp4_times_t;


typedef struct {
    uint32_t                            sample_count;
    uint32_t                            sample_offset;
} ngx_rtmp_mp4_delay_entry_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    ngx_rtmp_mp4_delay_entry_t          entries[0];
} ngx_rtmp_mp4_delays_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    uint32_t                            entries[0];
} ngx_rtmp_mp4_keys_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            sample_size;
    uint32_t                            sample_count;
    uint32_t                            entries[0];
} ngx_rtmp_mp4_sizes_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            field_size;
    uint32_t                            sample_count;
    uint32_t                            entries[0];
} ngx_rtmp_mp4_sizes2_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    uint32_t                            entries[0];
} ngx_rtmp_mp4_offsets_t;


typedef struct {
    uint32_t                            version_flags;
    uint32_t                            entry_count;
    uint64_t                            entries[0];
} ngx_rtmp_mp4_offsets64_t;

#pragma pack(pop)


typedef struct {
    uint32_t                            timestamp;
    uint32_t                            duration;
    off_t                               offset;
    size_t                              size;
    ngx_int_t                           key;
    uint32_t                            delay;

    ngx_uint_t                          pos;

    ngx_uint_t                          key_pos;

    ngx_uint_t                          chunk;
    ngx_uint_t                          chunk_pos;
    ngx_uint_t                          chunk_count;

    ngx_uint_t                          time_pos;
    ngx_uint_t                          time_count;

    ngx_uint_t                          delay_pos;
    ngx_uint_t                          delay_count;

    ngx_uint_t                          size_pos;
} ngx_rtmp_mp4_cursor_t;


typedef struct {
    ngx_uint_t                          id;

    ngx_int_t                           type;
    ngx_int_t                           codec;
    uint32_t                            csid;
    u_char                              fhdr;
    ngx_int_t                           time_scale;
    uint64_t                            duration;

    u_char                             *header;
    size_t                              header_size;
    ngx_int_t                           header_sent;

    ngx_rtmp_mp4_times_t               *times;
    ngx_rtmp_mp4_delays_t              *delays;
    ngx_rtmp_mp4_keys_t                *keys;
    ngx_rtmp_mp4_chunks_t              *chunks;
    ngx_rtmp_mp4_sizes_t               *sizes;
    ngx_rtmp_mp4_sizes2_t              *sizes2;
    ngx_rtmp_mp4_offsets_t             *offsets;
    ngx_rtmp_mp4_offsets64_t           *offsets64;
    ngx_rtmp_mp4_cursor_t               cursor;
} ngx_rtmp_mp4_track_t;


typedef struct {
    ngx_file_t                          file;
    
    void                               *mmaped;
    size_t                              mmaped_size;

    ngx_rtmp_mp4_track_t                tracks[2];
    ngx_rtmp_mp4_track_t               *track;
    ngx_uint_t                          ntracks;

    ngx_uint_t                          width;
    ngx_uint_t                          height;
    ngx_uint_t                          nchannels;
    ngx_uint_t                          sample_size;
    ngx_uint_t                          sample_rate;

    uint32_t                            start_timestamp, epoch;

    ngx_event_t                         write_evt;
} ngx_rtmp_mp4_ctx_t;


/* system stuff for mmapping; 4K pages assumed */
/* TODO: more portable code */
#define NGX_RTMP_PAGE_SHIFT             12
#define NGX_RTMP_PAGE_SIZE              (1 << NGX_RTMP_PAGE_SHIFT)
#define NGX_RTMP_PAGE_MASK              (NGX_RTMP_PAGE_SIZE - 1)


#define ngx_rtmp_mp4_make_tag(a, b, c, d) ((uint32_t) d << 24       | \
                                           (uint32_t) c << 16       | \
                                           (uint32_t) b << 8        | \
                                           (uint32_t) a)


/*
#define ngx_rtmp_r32(n)                (((n) & 0x000000ffull) << 24 | \
                                        ((n) & 0x0000ff00ull) << 8  | \
                                        ((n) & 0x00ff0000ull) >> 8  | \
                                        ((n) & 0xff000000ull) >> 24)
*/

static inline uint16_t
ngx_rtmp_r16(uint16_t n)
{
    uint16_t    ret;

    /*TODO: optimize */
    ngx_rtmp_rmemcpy(&ret, &n, 2);
    return ret;
}


static inline uint32_t
ngx_rtmp_r32(uint32_t n)
{
    uint32_t    ret;

    /*TODO: optimize */
    ngx_rtmp_rmemcpy(&ret, &n, 4);
    return ret;
}


static inline uint64_t
ngx_rtmp_r64(uint64_t n)
{
    uint64_t    ret;

    ngx_rtmp_rmemcpy(&ret, &n, 8);
    return ret;
}


#define NGX_RTMP_MP4_DEFAULT_BUFLEN     1000


static u_char                           ngx_rtmp_mp4_buffer[1024*1024];


static ngx_int_t ngx_rtmp_mp4_parse(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_trak(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_mdhd(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_hdlr(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stsd(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stsc(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stts(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_ctts(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stss(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stsz(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stz2(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_stco(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_co64(ngx_rtmp_session_t *s, u_char *pos,
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_avc1(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_avcC(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_mp4a(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_esds(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_mp3(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_nmos(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);
static ngx_int_t ngx_rtmp_mp4_parse_spex(ngx_rtmp_session_t *s, u_char *pos, 
       u_char *last);


typedef ngx_int_t (*ngx_rtmp_mp4_box_pt)(ngx_rtmp_session_t *s, u_char *pos,
                                         u_char *last);

typedef struct {
    uint32_t                            tag;
    ngx_rtmp_mp4_box_pt                 handler;
} ngx_rtmp_mp4_box_t;


static ngx_rtmp_mp4_box_t                       ngx_rtmp_mp4_boxes[] = {
    { ngx_rtmp_mp4_make_tag('t','r','a','k'),   ngx_rtmp_mp4_parse_trak   },
    { ngx_rtmp_mp4_make_tag('m','d','i','a'),   ngx_rtmp_mp4_parse        },
    { ngx_rtmp_mp4_make_tag('m','d','h','d'),   ngx_rtmp_mp4_parse_mdhd   },
    { ngx_rtmp_mp4_make_tag('h','d','l','r'),   ngx_rtmp_mp4_parse_hdlr   },
    { ngx_rtmp_mp4_make_tag('m','i','n','f'),   ngx_rtmp_mp4_parse        },
    { ngx_rtmp_mp4_make_tag('s','t','b','l'),   ngx_rtmp_mp4_parse        }, 
    { ngx_rtmp_mp4_make_tag('s','t','s','d'),   ngx_rtmp_mp4_parse_stsd   },
    { ngx_rtmp_mp4_make_tag('s','t','s','c'),   ngx_rtmp_mp4_parse_stsc   },
    { ngx_rtmp_mp4_make_tag('s','t','t','s'),   ngx_rtmp_mp4_parse_stts   },
    { ngx_rtmp_mp4_make_tag('c','t','t','s'),   ngx_rtmp_mp4_parse_ctts   },
    { ngx_rtmp_mp4_make_tag('s','t','s','s'),   ngx_rtmp_mp4_parse_stss   },
    { ngx_rtmp_mp4_make_tag('s','t','s','z'),   ngx_rtmp_mp4_parse_stsz   },
    { ngx_rtmp_mp4_make_tag('s','t','z','2'),   ngx_rtmp_mp4_parse_stz2   },
    { ngx_rtmp_mp4_make_tag('s','t','c','o'),   ngx_rtmp_mp4_parse_stco   },
    { ngx_rtmp_mp4_make_tag('c','o','6','4'),   ngx_rtmp_mp4_parse_co64   },
    { ngx_rtmp_mp4_make_tag('a','v','c','1'),   ngx_rtmp_mp4_parse_avc1   }, 
    { ngx_rtmp_mp4_make_tag('a','v','c','C'),   ngx_rtmp_mp4_parse_avcC   }, 
    { ngx_rtmp_mp4_make_tag('m','p','4','a'),   ngx_rtmp_mp4_parse_mp4a   }, 
    { ngx_rtmp_mp4_make_tag('e','s','d','s'),   ngx_rtmp_mp4_parse_esds   }, 
    { ngx_rtmp_mp4_make_tag('.','m','p','3'),   ngx_rtmp_mp4_parse_mp3    }, 
    { ngx_rtmp_mp4_make_tag('n','m','o','s'),   ngx_rtmp_mp4_parse_nmos   }, 
    { ngx_rtmp_mp4_make_tag('s','p','e','x'),   ngx_rtmp_mp4_parse_spex   }
};


static ngx_command_t  ngx_rtmp_mp4_commands[] = {

    { ngx_string("play_mp4"),
      NGX_RTMP_MAIN_CONF|NGX_RTMP_SRV_CONF|NGX_RTMP_APP_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_RTMP_APP_CONF_OFFSET,
      offsetof(ngx_rtmp_mp4_app_conf_t, root),
      NULL },

      ngx_null_command
};


static ngx_rtmp_module_t  ngx_rtmp_mp4_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_rtmp_mp4_postconfiguration,         /* postconfiguration */
    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */
    NULL,                                   /* create server configuration */
    NULL,                                   /* merge server configuration */
    ngx_rtmp_mp4_create_app_conf,           /* create app configuration */
    ngx_rtmp_mp4_merge_app_conf             /* merge app configuration */
};


ngx_module_t  ngx_rtmp_mp4_module = {
    NGX_MODULE_V1,
    &ngx_rtmp_mp4_module_ctx,               /* module context */
    ngx_rtmp_mp4_commands,                  /* module directives */
    NGX_RTMP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_rtmp_mp4_create_app_conf(ngx_conf_t *cf)
{
    ngx_rtmp_mp4_app_conf_t    *pacf;

    pacf = ngx_pcalloc(cf->pool, sizeof(ngx_rtmp_mp4_app_conf_t));

    if (pacf == NULL) {
        return NULL;
    }

    return pacf;
}


static char *
ngx_rtmp_mp4_merge_app_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_rtmp_mp4_app_conf_t *prev = parent;
    ngx_rtmp_mp4_app_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->root, prev->root, "");

    return NGX_CONF_OK;
} 


static ngx_int_t
ngx_rtmp_mp4_parse_trak(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track) {
        return NGX_OK;
    }

    ctx->track = (ctx->ntracks == sizeof(ctx->tracks) / sizeof(ctx->tracks[0]))
                 ? NULL : &ctx->tracks[ctx->ntracks];

    if (ctx->track) {
        ngx_memzero(ctx->track, sizeof(ctx->track));
        ctx->track->id = ctx->ntracks;

        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: trying track %ui", ctx->ntracks);
    }

    if (ngx_rtmp_mp4_parse(s, pos, last) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ctx->track && ctx->track->type &&
        (ctx->ntracks == 0 || 
         ctx->tracks[0].type != ctx->tracks[ctx->ntracks].type))
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: adding track %ui", ctx->ntracks);
        ++ctx->ntracks;

    } else {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: ignoring track %ui", ctx->ntracks);
    }

    ctx->track = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_mdhd(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;
    uint8_t                     version;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL) {
        return NGX_OK;
    }

    t = ctx->track;

    if (pos + 1 > last) {
        return NGX_ERROR;
    }

    version = *(uint8_t *) pos;

    switch (version) {
        case 0:
            if (pos + 20 > last) {
                return NGX_ERROR;
            }

            pos += 12;
            t->time_scale = ngx_rtmp_r32(*(uint32_t *) pos);
            pos += 4;
            t->duration = ngx_rtmp_r32(*(uint32_t *) pos);
            break;

        case 1:
            if (pos + 28 > last) {
                return NGX_ERROR;
            }

            pos += 20;
            t->time_scale = ngx_rtmp_r32(*(uint32_t *) pos);
            pos += 4;
            t->duration = ngx_rtmp_r64(*(uint64_t *) pos);
            break;

        default:
            return NGX_ERROR;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: duration time_scale=%ui duration=%uL",
                   t->time_scale, t->duration);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_hdlr(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    uint32_t                    type;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL) {
        return NGX_OK;
    }

    if (pos + 12 > last) {
        return NGX_ERROR;
    }

    type = *(uint32_t *)(pos + 8);

    if (type == ngx_rtmp_mp4_make_tag('v','i','d','e')) {
        ctx->track->type = NGX_RTMP_MSG_VIDEO;
        ctx->track->csid = NGX_RTMP_LIVE_CSID_VIDEO;

        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: video track");

    } else if (type == ngx_rtmp_mp4_make_tag('s','o','u','n')) {
        ctx->track->type = NGX_RTMP_MSG_AUDIO;
        ctx->track->csid = NGX_RTMP_LIVE_CSID_AUDIO;

        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: audio track");
    } else {
        ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: unknown track");
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_video(ngx_rtmp_session_t *s, u_char *pos, u_char *last, 
                         ngx_int_t codec)
{
    ngx_rtmp_mp4_ctx_t         *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL) {
        return NGX_OK;
    }

    ctx->track->codec = codec;

    if (pos + 78 > last) {
        return NGX_ERROR;
    }

    pos += 24;

    ctx->width = ngx_rtmp_r16(*(uint16_t *) pos);

    pos += 2;

    ctx->height = ngx_rtmp_r16(*(uint16_t *) pos);

    pos += 52;

    ctx->track->fhdr = codec;

    ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: video settings codec=%i, width=%ui, height=%ui",
                   codec, ctx->width, ctx->height);

    return ngx_rtmp_mp4_parse(s, pos, last);
}


static ngx_int_t
ngx_rtmp_mp4_parse_audio(ngx_rtmp_session_t *s, u_char *pos, u_char *last,
                         ngx_int_t codec)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    u_char                     *p;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL) {
        return NGX_OK;
    }

    ctx->track->codec = codec;

    if (pos + 28 > last) {
        return NGX_ERROR;
    }

    pos += 16;

    ctx->nchannels = ngx_rtmp_r16(*(uint16_t *) pos);

    pos += 2;

    ctx->sample_size = ngx_rtmp_r16(*(uint16_t *) pos);

    pos += 6;

    ctx->sample_rate = ngx_rtmp_r16(*(uint16_t *) pos);

    pos += 4;

    p = &ctx->track->fhdr;

    *p = 0;

    if (ctx->nchannels) {
        *p |= 0x01;
    }

    if (ctx->sample_size == 16) {
        *p |= 0x02;
    }

    switch (ctx->sample_rate) {
        case 11025:
            *p |= 0x04;
            break;

        case 22050:
            *p |= 0x08;
            break;

        case 44100:
            *p |= 0x0c;
            break;
    }

    *p |= (codec << 4);

    ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: audio settings codec=%i, nchannels==%ui, "
                   "sample_size=%ui, sample_rate=%ui",
                   codec, ctx->nchannels, ctx->sample_size, ctx->sample_rate);

    return ngx_rtmp_mp4_parse(s, pos, last);
}


static ngx_int_t
ngx_rtmp_mp4_parse_avc1(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    return ngx_rtmp_mp4_parse_video(s, pos, last, NGX_RTMP_VIDEO_H264);
}


static ngx_int_t
ngx_rtmp_mp4_parse_avcC(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;

    if (pos == last) {
        return NGX_OK;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL || ctx->track->codec != NGX_RTMP_VIDEO_H264) {
        return NGX_OK;
    }

    ctx->track->header = pos;
    ctx->track->header_size = (size_t) (last - pos);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: video h264 header size=%uz",
                   ctx->track->header_size);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_mp4a(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    return ngx_rtmp_mp4_parse_audio(s, pos, last, NGX_RTMP_AUDIO_MP3);
}


static ngx_int_t
ngx_rtmp_mp4_parse_esds(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;

    if (pos == last) {
        return NGX_OK;
    }

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx->track == NULL || ctx->track->codec != NGX_RTMP_AUDIO_AAC) {
        return NGX_OK;
    }

    /*TODO: parse AAC header? */

    ctx->track->header = pos;
    ctx->track->header_size = (size_t) (last - pos);

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: audio AAC header size=%uz",
                   ctx->track->header_size);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_mp3(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    return ngx_rtmp_mp4_parse_audio(s, pos, last, NGX_RTMP_AUDIO_MP3);
}


static ngx_int_t
ngx_rtmp_mp4_parse_nmos(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    return ngx_rtmp_mp4_parse_audio(s, pos, last, NGX_RTMP_AUDIO_NELLY);
}


static ngx_int_t
ngx_rtmp_mp4_parse_spex(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    return ngx_rtmp_mp4_parse_audio(s, pos, last, NGX_RTMP_AUDIO_SPEEX);
}


static ngx_int_t
ngx_rtmp_mp4_parse_stsd(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    if (pos + 8 > last) {
        return NGX_ERROR;
    }

    pos += 8;

    ngx_rtmp_mp4_parse(s, pos, last);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stsc(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->chunks = (ngx_rtmp_mp4_chunks_t *) pos;

    if (pos + sizeof(*t->chunks) + ngx_rtmp_r32(t->times->entry_count) * 
                                   sizeof(t->chunks->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: chunks entries=%uD", 
                       ngx_rtmp_r32(t->chunks->entry_count));
        return NGX_OK;
    }

    t->chunks = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stts(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->times = (ngx_rtmp_mp4_times_t *) pos;

    if (pos + sizeof(*t->times) + ngx_rtmp_r32(t->times->entry_count) * 
                                  sizeof(t->times->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: times entries=%uD", 
                       ngx_rtmp_r32(t->times->entry_count));
        return NGX_OK;
    }

    t->times = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_ctts(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->delays = (ngx_rtmp_mp4_delays_t *) pos;

    if (pos + sizeof(*t->delays) + ngx_rtmp_r32(t->delays->entry_count) * 
                                   sizeof(t->delays->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: delays entries=%uD", 
                       ngx_rtmp_r32(t->delays->entry_count));
        return NGX_OK;
    }

    t->delays = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stss(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->keys = (ngx_rtmp_mp4_keys_t *) pos;

    if (pos + sizeof(*t->keys) + ngx_rtmp_r32(t->keys->entry_count) * 
                                  sizeof(t->keys->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: keys entries=%uD", 
                       ngx_rtmp_r32(t->keys->entry_count));
        return NGX_OK;
    }

    t->keys = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stsz(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->sizes = (ngx_rtmp_mp4_sizes_t *) pos;

    if (pos + sizeof(*t->sizes) <= last && t->sizes->sample_size) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: sizes size=%uD", 
                       ngx_rtmp_r32(t->sizes->sample_size));
        return NGX_OK;
    }

    if (pos + sizeof(*t->sizes) + ngx_rtmp_r32(t->sizes->sample_count) *
                                  sizeof(t->sizes->entries[0])
        <= last)

    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: sizes entries=%uD", 
                       ngx_rtmp_r32(t->sizes->sample_count));
        return NGX_OK;
    }

    t->sizes = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stz2(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->sizes2 = (ngx_rtmp_mp4_sizes2_t *) pos;

    if (pos + sizeof(*t->sizes) + ngx_rtmp_r32(t->sizes2->sample_count) *
                                  ngx_rtmp_r32(t->sizes2->field_size) / 8
        <= last)
    {
        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: sizes2 field_size=%uD entries=%uD", 
                       ngx_rtmp_r32(t->sizes2->field_size), 
                       ngx_rtmp_r32(t->sizes2->sample_count));
        return NGX_OK;
    }

    t->sizes2 = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_stco(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->offsets = (ngx_rtmp_mp4_offsets_t *) pos;

    if (pos + sizeof(*t->offsets) + ngx_rtmp_r32(t->offsets->entry_count) *
                                    sizeof(t->offsets->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: offsets entries=%uD", 
                       ngx_rtmp_r32(t->offsets->entry_count));
        return NGX_OK;
    }

    t->offsets = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse_co64(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    ngx_rtmp_mp4_track_t       *t;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    t = ctx->track;

    if (t == NULL) {
        return NGX_OK;
    }

    t->offsets64 = (ngx_rtmp_mp4_offsets64_t *) pos;

    if (pos + sizeof(*t->offsets64) + ngx_rtmp_r32(t->offsets64->entry_count) *
                                      sizeof(t->offsets64->entries[0])
        <= last)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: offsets64 entries=%uD", 
                       ngx_rtmp_r32(t->offsets64->entry_count));
        return NGX_OK;
    }

    t->offsets64 = NULL;
    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_parse(ngx_rtmp_session_t *s, u_char *pos, u_char *last)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    uint32_t                   *hdr, tag;
    size_t                      size, nboxes;
    ngx_uint_t                  n;
    ngx_rtmp_mp4_box_t         *b;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    while (pos != last) {
        if (pos + 8 > last) {
            ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: too small box: size=%i", last - pos);
            return NGX_ERROR;
        }

        hdr = (uint32_t *) pos;
        size = ngx_rtmp_r32(hdr[0]);
        tag  = hdr[1];

        if (pos + size > last) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                          "mp4: too big box '%*s': size=%uz", 
                          4, &tag, size);
            return NGX_ERROR;
        }

        b = ngx_rtmp_mp4_boxes;
        nboxes = sizeof(ngx_rtmp_mp4_boxes) / sizeof(ngx_rtmp_mp4_boxes[0]);

        for (n = 0; n < nboxes && b->tag != tag; ++n, ++b);
        
        if (n == nboxes) {
            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: box unhandled '%*s'", 4, &tag);
        } else {
            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: box '%*s'", 4, &tag);
            b->handler(s, pos + 8, pos + size);
        }

        pos += size;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_init(ngx_rtmp_session_t *s)
{
    ngx_rtmp_mp4_ctx_t         *ctx;
    uint32_t                    hdr[2];
    ssize_t                     n;
    size_t                      offset, page_offset, size;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL || ctx->mmaped || ctx->file.fd == NGX_INVALID_FILE) {
        return NGX_OK;
    }

    offset = 0;
    size   = 0;

    /* find moov box */
    for ( ;; ) {
        n = ngx_read_file(&ctx->file, (u_char *) &hdr, sizeof(hdr), offset);

        if (n != sizeof(hdr)) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                          "mp4: error reading file at offset=%uz "
                          "while searching for moov box", offset);
            return NGX_ERROR;
        }

        /*TODO: implement 64-bit boxes */

        size = ngx_rtmp_r32(hdr[0]);

        if (hdr[1] == ngx_rtmp_mp4_make_tag('m','o','o','v')) {
            ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: found moov box");
            break;
        }

        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: skipping box '%*s'", 4, hdr + 1);

        offset += size;
    }

    if (size < 8) {
        return NGX_ERROR;
    }

    size   -= 8;
    offset += 8;

    /* mmap moov box */
    page_offset = (offset & NGX_RTMP_PAGE_MASK);
    ctx->mmaped_size = page_offset + size;

    ctx->mmaped = mmap(NULL, ctx->mmaped_size, PROT_READ, MAP_SHARED,
                       ctx->file.fd, offset - page_offset);

    if (ctx->mmaped == MAP_FAILED) {
        ctx->mmaped = NULL;
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "mp4: mmap failed at offset=%ui, size=%uz",
                      offset, size);
        return NGX_ERROR;
    }

    /* locate all required data within mapped area */
    return ngx_rtmp_mp4_parse(s, (u_char *) ctx->mmaped + page_offset, 
                                 (u_char *) ctx->mmaped + page_offset + size);
}


static ngx_int_t
ngx_rtmp_mp4_done(ngx_rtmp_session_t *s)
{
    ngx_rtmp_mp4_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL || ctx->mmaped == NULL) {
        return NGX_OK;
    }

    if (munmap(ctx->mmaped, ctx->mmaped_size)) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, ngx_errno,
                      "mp4: munmap failed");
        return NGX_ERROR;
    }

    ctx->mmaped = NULL;
    ctx->mmaped_size = 0;

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_next_time(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t      *cr;
    ngx_rtmp_mp4_time_entry_t  *te;

    if (t->times == NULL) {
        return NGX_ERROR;
    }

    cr = &t->cursor;

    if (cr->time_pos >= ngx_rtmp_r32(t->times->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui time[%ui/%uD] overflow",
                       t->id, cr->time_pos,
                       ngx_rtmp_r32(t->times->entry_count));

        return NGX_ERROR;
    }

    te = &t->times->entries[cr->time_pos];

    cr->duration = ngx_rtmp_r32(te->sample_delta);
    cr->timestamp += cr->duration;

    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui time[%ui] [%ui/%uD][%ui/%uD]=%uD t=%uD",
                   t->id, cr->pos, cr->time_pos, 
                   ngx_rtmp_r32(t->times->entry_count),
                   cr->time_count, ngx_rtmp_r32(te->sample_count),
                   ngx_rtmp_r32(te->sample_delta),
                   cr->timestamp);

    cr->time_count++;
    cr->pos++;

    if (cr->time_count >= ngx_rtmp_r32(te->sample_count)) {
        cr->time_pos++;
        cr->time_count = 0;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_seek_time(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t, 
                       ngx_int_t timestamp)
{
    ngx_rtmp_mp4_cursor_t      *cr;
    ngx_rtmp_mp4_time_entry_t  *te;
    uint32_t                    dt;
    ngx_uint_t                  dn;

    if (t->times == NULL) {
        return NGX_ERROR;
    }

    cr = &t->cursor;

    te = t->times->entries;

    while (cr->time_pos < ngx_rtmp_r32(t->times->entry_count)) {
        dt = ngx_rtmp_r32(te->sample_delta) * ngx_rtmp_r32(te->sample_count);
        
        if (cr->timestamp + dt > timestamp) {
            if (te->sample_delta == 0) {
                return NGX_ERROR;
            }

            dn = (timestamp - cr->timestamp) / ngx_rtmp_r32(te->sample_delta);
            cr->timestamp = ngx_rtmp_r32(te->sample_delta) * dn;
            cr->pos += dn;
            break;
        }

        cr->timestamp += dt;
        cr->pos += ngx_rtmp_r32(te->sample_count);
        cr->time_pos++;
        te++;
    }

    if (cr->time_pos >= ngx_rtmp_r32(t->times->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui seek time[%ui/%uD] overflow",
                       t->id, cr->time_pos,
                       ngx_rtmp_r32(t->times->entry_count));

        return  NGX_ERROR;
    }

    ngx_log_debug8(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui seek time[%ui] [%ui/%uD][%ui/%uD]=%uD "
                   "t=%uD",
                   t->id, cr->pos, cr->time_pos, 
                   ngx_rtmp_r32(t->times->entry_count),
                   cr->time_count,
                   ngx_rtmp_r32(te->sample_count),
                   ngx_rtmp_r32(te->sample_delta),
                   cr->timestamp);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_update_offset(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;
    ngx_uint_t                      chunk;

    cr = &t->cursor;

    if (cr->chunk < 1) {
        ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui offset[%ui] underflow",
                       t->id, cr->chunk);
        return NGX_ERROR;
    }

    chunk = cr->chunk - 1;

    if (t->offsets) {
        if (chunk >= ngx_rtmp_r32(t->offsets->entry_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#[%ui] offset[%ui/%uD] overflow",
                           t->id, cr->chunk,
                           ngx_rtmp_r32(t->offsets->entry_count));

            return NGX_ERROR;
        }

        cr->offset = ngx_rtmp_r32(t->offsets->entries[chunk]);
        cr->size = 0;

        ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui offset[%ui/%uD]=%O",
                       t->id, cr->chunk, 
                       ngx_rtmp_r32(t->offsets->entry_count),
                       cr->offset);

        return NGX_OK;
    }

    if (t->offsets64) {
        if (chunk >= ngx_rtmp_r32(t->offsets64->entry_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui offset64[%ui/%uD] overflow",
                           t->id, cr->chunk,
                           ngx_rtmp_r32(t->offsets->entry_count));

            return NGX_ERROR;
        }

        cr->offset = ngx_rtmp_r32(t->offsets64->entries[chunk]);
        cr->size = 0;

        ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui offset64[%ui/%uD]=%O",
                       t->id, cr->chunk,
                       ngx_rtmp_r32(t->offsets->entry_count),
                       cr->offset);

        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_next_chunk(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;
    ngx_rtmp_mp4_chunk_entry_t     *ce, *nce;
    ngx_int_t                       new_chunk;

    if (t->chunks == NULL) {
        return NGX_OK;
    }

    cr = &t->cursor;

    if (cr->chunk_pos >= ngx_rtmp_r32(t->chunks->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui chunk[%ui/%uD] overflow",
                       t->id, cr->chunk_pos,
                       ngx_rtmp_r32(t->chunks->entry_count));

        return NGX_ERROR;
    }
    
    ce = &t->chunks->entries[cr->chunk_pos];

    cr->chunk_count++;

    if (cr->chunk_count >= ngx_rtmp_r32(ce->samples_per_chunk)) {
        cr->chunk_count = 0;
        cr->chunk++;

        if (cr->chunk_pos + 1 < ngx_rtmp_r32(t->chunks->entry_count)) {
            nce = ce + 1;
            if (cr->chunk >= ngx_rtmp_r32(nce->first_chunk)) {
                cr->chunk_pos++;
                ce = nce;
            }
        }

        new_chunk = 1;

    } else {
        new_chunk = 0;
    }

    ngx_log_debug7(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui chunk[%ui/%uD][%uD..%ui][%ui/%uD]",
                   t->id, cr->chunk_pos,
                   ngx_rtmp_r32(t->chunks->entry_count),
                   ngx_rtmp_r32(ce->first_chunk),
                   cr->chunk, cr->chunk_count,
                   ngx_rtmp_r32(ce->samples_per_chunk));


    if (new_chunk) {
        return ngx_rtmp_mp4_update_offset(s, t);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_seek_chunk(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;
    ngx_rtmp_mp4_chunk_entry_t     *ce, *nce;
    ngx_uint_t                      pos, dpos, dchunk;

    cr = &t->cursor;

    if (t->chunks == NULL || t->chunks->entry_count == 0) {
        cr->chunk = 1;
        return NGX_OK;
    }

    ce = t->chunks->entries;
    pos = 0;

    while (cr->chunk_pos + 1 < ngx_rtmp_r32(t->chunks->entry_count)) {
        nce = ce + 1;
        
        dpos = (ngx_rtmp_r32(nce->first_chunk) - 
                ngx_rtmp_r32(ce->first_chunk)) *
                ngx_rtmp_r32(ce->samples_per_chunk);

        if (pos + dpos > cr->pos) {
            break;
        }

        pos += dpos;
        ce++;
        cr->chunk_pos++;
    }

    if (ce->samples_per_chunk == 0) {
        return NGX_ERROR;
    }

    dchunk = (cr->pos - pos) / ngx_rtmp_r32(ce->samples_per_chunk);

    cr->chunk = ngx_rtmp_r32(ce->first_chunk) + dchunk;
    cr->chunk_pos = (ngx_uint_t) (ce - t->chunks->entries);
    cr->chunk_count = (ngx_uint_t) (cr->pos - dchunk * 
                                    ngx_rtmp_r32(ce->samples_per_chunk));

    ngx_log_debug7(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui seek chunk[%ui/%uD][%uD..%ui][%ui/%uD]",
                   t->id, cr->chunk_pos,
                   ngx_rtmp_r32(t->chunks->entry_count),
                   ngx_rtmp_r32(ce->first_chunk),
                   cr->chunk, cr->chunk_count,
                   ngx_rtmp_r32(ce->samples_per_chunk));

    return ngx_rtmp_mp4_update_offset(s, t);
}


static ngx_int_t
ngx_rtmp_mp4_next_size(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;

    cr = &t->cursor;

    cr->offset += cr->size;

    if (t->sizes) {
        if (t->sizes->sample_size) {
            cr->size = ngx_rtmp_r32(t->sizes->sample_size);

            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui size fix=%uz",
                           t->id, cr->size);

            return NGX_OK;
        }

        cr->size_pos++;

        if (cr->size_pos >= ngx_rtmp_r32(t->sizes->sample_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui size[%ui/%uD] overflow",
                           t->id, cr->size_pos,
                           ngx_rtmp_r32(t->sizes->sample_count));

            return NGX_ERROR;
        }

        cr->size = ngx_rtmp_r32(t->sizes->entries[cr->size_pos]);

        ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui size[%ui/%uD]=%uz",
                       t->id, cr->size_pos, 
                       ngx_rtmp_r32(t->sizes->sample_count),
                       cr->size);

        return NGX_OK;
    }

    if (t->sizes2) {
        if (cr->size_pos >= ngx_rtmp_r32(t->sizes2->sample_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui size[%ui/%uD] overflow",
                           t->id, cr->size_pos,
                           ngx_rtmp_r32(t->sizes2->sample_count));

            return NGX_ERROR;
        }

        /*TODO*/

        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_seek_size(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t      *cr;

    cr = &t->cursor;

    if (t->sizes) {
        if (t->sizes->sample_size) {
            cr->size = ngx_rtmp_r32(t->sizes->sample_size);

            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui seek size fix=%uz",
                           t->id, cr->size);

            return NGX_OK;
        }

        if (cr->pos >= ngx_rtmp_r32(t->sizes->sample_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui seek size[%ui/%uD] overflow",
                           t->id, cr->pos,
                           ngx_rtmp_r32(t->sizes->sample_count));

            return NGX_ERROR;
        }

        cr->size_pos = cr->pos;
        cr->size = ngx_rtmp_r32(t->sizes->entries[cr->size_pos]);

        ngx_log_debug4(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui seek size[%ui/%uD]=%uz",
                       t->id, cr->size_pos, 
                       ngx_rtmp_r32(t->sizes->sample_count),
                       cr->size);

        return NGX_OK;
    }

    if (t->sizes2) {
        if (cr->size_pos >= ngx_rtmp_r32(t->sizes2->sample_count)) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui seek size2[%ui/%uD] overflow",
                           t->id, cr->size_pos,
                           ngx_rtmp_r32(t->sizes->sample_count));

            return NGX_ERROR;
        }

        cr->size_pos = cr->pos;

        /* TODO */
        return NGX_OK;
    }

    return NGX_ERROR;
}


static ngx_int_t
ngx_rtmp_mp4_next_key(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;
    uint32_t                       *ke;

    cr = &t->cursor;

    if (t->keys == NULL) {
        return NGX_OK;
    }

    if (cr->key) {
        cr->key_pos++;
    }

    if (cr->key_pos >= ngx_rtmp_r32(t->keys->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "mp4: track#%ui key[%ui/%uD] overflow",
                t->id, cr->key_pos,
                ngx_rtmp_r32(t->keys->entry_count));

        cr->key = 0;

        return NGX_OK;
    }
    
    ke = &t->keys->entries[cr->key_pos];
    cr->key = (cr->pos + 1 == ngx_rtmp_r32(*ke));

    ngx_log_debug6(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui key[%ui/%uD][%ui/%uD]=%s",
                   t->id, cr->key_pos,
                   ngx_rtmp_r32(t->keys->entry_count),
                   cr->pos, ngx_rtmp_r32(*ke),
                   cr->key ? "match" : "miss");

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_seek_key(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t      *cr;
    uint32_t                   *ke;

    cr = &t->cursor;

    if (t->keys == NULL) {
        return NGX_OK;
    }

    while (cr->key_pos < ngx_rtmp_r32(t->keys->entry_count)) {
        if (ngx_rtmp_r32(t->keys->entries[cr->key_pos]) >= cr->pos) {
            break;
        }

        cr->key_pos++;
    }

    if (cr->key_pos >= ngx_rtmp_r32(t->keys->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "mp4: track#%ui seek key[%ui/%uD] overflow",
                t->id, cr->key_pos,
                ngx_rtmp_r32(t->keys->entry_count));
        return NGX_OK;
    }

    ke = &t->keys->entries[cr->key_pos];
    cr->key = (cr->pos + 1 == ngx_rtmp_r32(*ke));

    ngx_log_debug6(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui seek key[%ui/%uD][%ui/%uD]=%s",
                   t->id, cr->key_pos,
                   ngx_rtmp_r32(t->keys->entry_count),
                   cr->pos, ngx_rtmp_r32(*ke),
                   cr->key ? "match" : "miss");

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_next_delay(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t          *cr;
    ngx_rtmp_mp4_delay_entry_t     *de;

    cr = &t->cursor;

    if (t->delays == NULL) {
        return NGX_OK;
    }

    if (cr->delay_pos >= ngx_rtmp_r32(t->delays->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "mp4: track#%ui delay[%ui/%uD] overflow",
                t->id, cr->delay_pos,
                ngx_rtmp_r32(t->delays->entry_count));

        return NGX_OK;
    }

    cr->delay_count++;
    de = &t->delays->entries[cr->delay_pos];

    if (cr->delay_count >= ngx_rtmp_r32(de->sample_count)) {
        cr->delay_pos++;
        de++;
        cr->delay_count = 0;
    }

    if (cr->delay_pos >= ngx_rtmp_r32(t->delays->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "mp4: track#%ui delay[%ui/%uD] overflow",
                t->id, cr->delay_pos,
                ngx_rtmp_r32(t->delays->entry_count));

        return NGX_OK;
    }

    cr->delay = ngx_rtmp_r32(de->sample_offset);

    ngx_log_debug6(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui delay[%ui/%uD][%ui/%uD]=%ui",
                   t->id, cr->delay_pos,
                   ngx_rtmp_r32(t->delays->entry_count),
                   cr->delay_count,
                   ngx_rtmp_r32(de->sample_count), cr->delay);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_seek_delay(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    ngx_rtmp_mp4_cursor_t      *cr;
    ngx_rtmp_mp4_delay_entry_t *de;
    uint32_t                    pos, dpos;

    cr = &t->cursor;

    if (t->delays == NULL) {
        return NGX_OK;
    }

    pos = 0;
    de = t->delays->entries;

    while (cr->delay_pos < ngx_rtmp_r32(t->delays->entry_count)) {
        dpos = ngx_rtmp_r32(de->sample_count);
        
        if (pos + dpos > cr->pos) {
            cr->delay_count = cr->pos - pos;
            cr->delay = ngx_rtmp_r32(de->sample_offset);
            break;
        }

        cr->delay_pos++;
        pos += dpos;
        de++;
    }

    if (cr->delay_pos >= ngx_rtmp_r32(t->delays->entry_count)) {
        ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                "mp4: track#%ui seek delay[%ui/%uD] overflow",
                t->id, cr->delay_pos,
                ngx_rtmp_r32(t->delays->entry_count));

        return NGX_OK;
    }

    ngx_log_debug6(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: track#%ui seek delay[%ui/%uD][%ui/%uD]=%ui",
                   t->id, cr->delay_pos,
                   ngx_rtmp_r32(t->delays->entry_count),
                   cr->delay_count,
                   ngx_rtmp_r32(de->sample_count), cr->delay);

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_next(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t)
{
    return ngx_rtmp_mp4_next_time(s, t)  != NGX_OK ||
           ngx_rtmp_mp4_next_key(s, t)   != NGX_OK ||
           ngx_rtmp_mp4_next_chunk(s, t) != NGX_OK ||
           ngx_rtmp_mp4_next_size(s, t)  != NGX_OK ||
           ngx_rtmp_mp4_next_delay(s, t) != NGX_OK
           ? NGX_ERROR : NGX_OK;
}


static uint32_t
ngx_rtmp_mp4_to_rtmp_timestamp(ngx_rtmp_mp4_track_t *t, uint32_t ts)
{
    return (uint64_t) ts * 1000 / t->time_scale;
}


static void
ngx_rtmp_mp4_send(ngx_event_t *e)
{
    ngx_rtmp_session_t             *s;
    ngx_rtmp_mp4_ctx_t             *ctx;
    ngx_buf_t                       in_buf;
    ngx_rtmp_header_t               h, lh;
    ngx_rtmp_core_srv_conf_t       *cscf;
    ngx_chain_t                    *out, in;
    ngx_rtmp_mp4_track_t           *t;
    ngx_rtmp_mp4_cursor_t          *cr;
    uint32_t                        buflen, end_timestamp, sched,
                                    timestamp, duration;
    ssize_t                         ret;
    u_char                          fhdr[5];
    size_t                          fhdr_size;
    ngx_uint_t                      n, abs_frame, active;

    s = e->data;

    cscf = ngx_rtmp_get_module_srv_conf(s, ngx_rtmp_core_module);

    ctx  = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL) {
        return;
    }

    buflen = (s->buflen ? s->buflen : NGX_RTMP_MP4_DEFAULT_BUFLEN);

    t = ctx->tracks;

    sched  = 0;
    active = 0;

    end_timestamp = ctx->start_timestamp + 
                    (ngx_current_msec - ctx->epoch) + buflen;

    for (n = 0; n < ctx->ntracks; ++n, ++t) {
        cr = &t->cursor;

        if (cr->size == 0) {
            continue;
        }

        timestamp = ngx_rtmp_mp4_to_rtmp_timestamp(t, cr->timestamp);

        if (timestamp > end_timestamp) {
            ngx_log_debug3(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui ahead %uD > %uD",
                           t->id, timestamp, end_timestamp);
            goto next;
        }

        duration = ngx_rtmp_mp4_to_rtmp_timestamp(t, cr->duration);
        abs_frame = (duration == 0);

        ngx_memzero(&h, sizeof(h));

        h.msid = NGX_RTMP_LIVE_MSID;
        h.type = t->type;
        h.csid = t->csid;

        lh = h;

        h.timestamp = (abs_frame ? timestamp : duration);

        ngx_memzero(&in, sizeof(in));
        ngx_memzero(&in_buf, sizeof(in_buf));

        if (t->header && !t->header_sent) {
            ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                           "mp4: track#%ui sending header of size=%uz",
                           t->id, t->header_size);

            fhdr[0] = t->fhdr | 0x10;
            fhdr[1] = fhdr[2] = fhdr[3] = fhdr[4] = 0;

            in.buf = &in_buf;
            in_buf.pos  = fhdr;
            in_buf.last = fhdr + 5;

            out = ngx_rtmp_append_shared_bufs(cscf, NULL, &in);

            in.buf = &in_buf;
            in_buf.pos  = t->header;
            in_buf.last = t->header + t->header_size;

            ngx_rtmp_append_shared_bufs(cscf, out, &in);

            ngx_rtmp_prepare_message(s, &h, NULL, out);
            ngx_rtmp_send_message(s, out, 0);
            ngx_rtmp_free_shared_chain(cscf, out);

            t->header_sent = 1;

            goto next;
        }

        ngx_log_debug5(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui read frame "
                       "offset=%O, size=%uz, timestamp=%uD, duration=%uD", 
                       t->id, cr->offset, cr->size, timestamp,
                       cr->duration);

        fhdr_size = (t->header ? 5 : 1);

        if (cr->size + fhdr_size > sizeof(ngx_rtmp_mp4_buffer)) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "mp4: track#%ui too big frame: %D>%uz", 
                          t->id, cr->size, sizeof(ngx_rtmp_mp4_buffer));
            continue;
        }

        ret = ngx_read_file(&ctx->file, ngx_rtmp_mp4_buffer + fhdr_size, 
                            cr->size, cr->offset);

        if (ret != (ssize_t) cr->size) {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "mp4: track#%ui could not read frame", t->id);
            continue;
        }

        ngx_rtmp_mp4_buffer[0] = t->fhdr;
        
        if (cr->key) {
            ngx_rtmp_mp4_buffer[0] |= 0x10;
        }

        if (fhdr_size > 1) {
            ngx_rtmp_mp4_buffer[1] = 1;
            ngx_rtmp_mp4_buffer[2] = cr->delay & 0xf00;
            ngx_rtmp_mp4_buffer[3] = cr->delay & 0x0f0;
            ngx_rtmp_mp4_buffer[4] = cr->delay & 0x00f;
        }

        in.buf = &in_buf;
        in_buf.pos  = ngx_rtmp_mp4_buffer;
        in_buf.last = ngx_rtmp_mp4_buffer + cr->size + fhdr_size;

        out = ngx_rtmp_append_shared_bufs(cscf, NULL, &in);
        
        ngx_rtmp_prepare_message(s, &h, abs_frame ? NULL : &lh, out);
        ngx_rtmp_send_message(s, out, 0);
        ngx_rtmp_free_shared_chain(cscf, out);

        if (ngx_rtmp_mp4_next(s, t) != NGX_OK) {
            continue;
        }

next:
        active = 1;

        if (timestamp > end_timestamp &&
            (sched == 0 || timestamp < end_timestamp + sched))
        {
            sched = (uint32_t) (timestamp - end_timestamp);
        }
    }

    if (sched) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: scheduling %uD", sched);
        ngx_add_timer(e, sched);
        return;
    }

    if (active) {
        ngx_post_event(e, &ngx_posted_events);
    }
}


static ngx_int_t
ngx_rtmp_mp4_seek_track(ngx_rtmp_session_t *s, ngx_rtmp_mp4_track_t *t,
                        ngx_int_t timestamp)
{
    ngx_rtmp_mp4_cursor_t          *cr;

    cr = &t->cursor;
    ngx_memzero(cr, sizeof(cr));

    return ngx_rtmp_mp4_seek_time(s, t, timestamp * 
                                        t->time_scale / 1000) != NGX_OK ||
           ngx_rtmp_mp4_seek_key(s, t)   != NGX_OK ||
           ngx_rtmp_mp4_seek_chunk(s, t) != NGX_OK ||
           ngx_rtmp_mp4_seek_size(s, t)  != NGX_OK ||
           ngx_rtmp_mp4_seek_delay(s, t) != NGX_OK
           ? NGX_ERROR : NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_start(ngx_rtmp_session_t *s, ngx_int_t timestamp)
{
    ngx_rtmp_mp4_ctx_t     *ctx;
    ngx_uint_t              n;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL) {
        return NGX_OK;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: start t=%i", timestamp);

    ngx_rtmp_mp4_stop(s);

    for (n = 0; n < ctx->ntracks; ++n) {
        ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                       "mp4: track#%ui seek", n);

        ngx_rtmp_mp4_seek_track(s, &ctx->tracks[n], timestamp);
    }

    ctx->epoch = ngx_current_msec;
    ctx->start_timestamp = timestamp;

    ngx_post_event((&ctx->write_evt), &ngx_posted_events)

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_stop(ngx_rtmp_session_t *s)
{
    ngx_rtmp_mp4_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL) {
        return NGX_OK;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: stop");

    if (ctx->write_evt.timer_set) {
        ngx_del_timer(&ctx->write_evt);
    }

    if (ctx->write_evt.prev) {
        ngx_delete_posted_event((&ctx->write_evt));
    }

    return NGX_OK;
}


static ngx_int_t
ngx_rtmp_mp4_close_stream(ngx_rtmp_session_t *s, ngx_rtmp_close_stream_t *v)
{
    ngx_rtmp_mp4_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL) {
        goto next;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: close_stream");

    ngx_rtmp_mp4_stop(s);

    ngx_rtmp_mp4_done(s);

    if (ctx->file.fd != NGX_INVALID_FILE) {
        ngx_close_file(ctx->file.fd);
        ctx->file.fd = NGX_INVALID_FILE;
    }

next:
    return next_close_stream(s, v);
}


static ngx_int_t
ngx_rtmp_mp4_seek(ngx_rtmp_session_t *s, ngx_rtmp_seek_t *v)
{
    ngx_rtmp_mp4_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL || ctx->file.fd == NGX_INVALID_FILE) {
        goto next;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: seek timestamp=%i", (ngx_int_t) v->offset);

    ngx_rtmp_mp4_start(s, v->offset);

next:
    return next_seek(s, v);
}


static ngx_int_t
ngx_rtmp_mp4_pause(ngx_rtmp_session_t *s, ngx_rtmp_pause_t *v)
{
    ngx_rtmp_mp4_ctx_t            *ctx;

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx == NULL || ctx->file.fd == NGX_INVALID_FILE) {
        goto next;
    }

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: pause=%i timestamp=%i",
                   (ngx_int_t) v->pause, (ngx_int_t) v->position);

    if (v->pause) {
        ngx_rtmp_mp4_stop(s);
    } else {
        ngx_rtmp_mp4_start(s, v->position);
    }

next:
    return next_pause(s, v);
}


static ngx_int_t
ngx_rtmp_mp4_play(ngx_rtmp_session_t *s, ngx_rtmp_play_t *v)
{
    ngx_rtmp_mp4_app_conf_t        *pacf;
    ngx_rtmp_mp4_ctx_t             *ctx;
    u_char                         *p;
    ngx_event_t                    *e;
    size_t                          len;
    static u_char                   path[NGX_MAX_PATH];
    u_char                         *name;

    pacf = ngx_rtmp_get_module_app_conf(s, ngx_rtmp_mp4_module);

    if (pacf == NULL || pacf->root.len == 0) {
        goto next;
    }

    if (ngx_strncasecmp(v->name, (u_char *) "mp4:", sizeof("mp4:") - 1) == 0) {
        name = v->name + sizeof("mp4:") - 1;
        goto ok;
    }

    len = ngx_strlen(v->name);

    if (len >= sizeof(".mp4") && 
        ngx_strncasecmp(v->name + len - sizeof(".mp4") + 1, (u_char *) ".mp4",
                        sizeof(".mp4") - 1))
    {
        name = v->name;
        goto ok;
    }

    goto next;
ok:

    ngx_log_debug2(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: play name='%s' timestamp=%i",
                    name, (ngx_int_t) v->start);

    ctx = ngx_rtmp_get_module_ctx(s, ngx_rtmp_mp4_module);

    if (ctx && ctx->file.fd != NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "mp4: already playing");
        goto next;
    }

    /* check for double-dot in name;
     * we should not move out of play directory */
    for (p = name; *p; ++p) {
        if (ngx_path_separator(p[0]) &&
            p[1] == '.' && p[2] == '.' && 
            ngx_path_separator(p[3])) 
        {
            ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                          "mp4: bad name '%s'", name);
            return NGX_ERROR;
        }
    }

    if (ctx == NULL) {
        ctx = ngx_palloc(s->connection->pool, sizeof(ngx_rtmp_mp4_ctx_t));
        ngx_rtmp_set_ctx(s, ctx, ngx_rtmp_mp4_module);
    }
    ngx_memzero(ctx, sizeof(*ctx));

    ctx->file.log = s->connection->log;

    p = ngx_snprintf(path, sizeof(path), "%V/%s", &pacf->root, name);
    *p = 0;

    ctx->file.fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 
                                 NGX_FILE_DEFAULT_ACCESS);
    if (ctx->file.fd == NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, s->connection->log, 0,
                      "mp4: error opening file %s", path);
        goto next;
    }

    ngx_log_debug1(NGX_LOG_DEBUG_RTMP, s->connection->log, 0,
                   "mp4: opened file '%s'", path);

    e = &ctx->write_evt;
    e->data = s;
    e->handler = ngx_rtmp_mp4_send;
    e->log = s->connection->log;

    ngx_rtmp_send_user_recorded(s, 1);

    ngx_rtmp_mp4_init(s);

    ngx_rtmp_mp4_start(s, v->start);

next:
    return next_play(s, v);
}


static ngx_int_t
ngx_rtmp_mp4_postconfiguration(ngx_conf_t *cf)
{
    next_play = ngx_rtmp_play;
    ngx_rtmp_play = ngx_rtmp_mp4_play;

    next_close_stream = ngx_rtmp_close_stream;
    ngx_rtmp_close_stream = ngx_rtmp_mp4_close_stream;

    next_seek = ngx_rtmp_seek;
    ngx_rtmp_seek = ngx_rtmp_mp4_seek;

    next_pause = ngx_rtmp_pause;
    ngx_rtmp_pause = ngx_rtmp_mp4_pause;

    return NGX_OK;
}
