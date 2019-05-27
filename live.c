#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>

#include <glib.h>
#include <jansson.h>

#include "rtp.h"
#include "live.h"
#include "debug.h"
#include "utils.h"

static int post_reset_trigger = 200;


static void janus_live_event_loop_init(janus_live_pub *pub, janus_live_el **el, int id, char* name);
static void *janus_live_event_loop_thread(void *data);
static gboolean janus_live_send_handle(gpointer user_data);
static gboolean janus_rtp_jb_handle(gpointer user_data);

static int janus_live_ffmpeg_init(janus_live_pub *pub);
static int janus_live_ffmpeg_free(janus_live_pub *pub);
static void janus_rtp_jb_free(janus_live_pub *pub);
static janus_frame_packet *janus_packet_alloc(int data_len);
static void janus_packet_free(janus_frame_packet *pkt);
static void janus_live_pub_free(const janus_refcount *pub_ref);

static int janus_live_rtp_header_extension_parse_audio_level(char *buf,int len, int id, int *level);
static int janus_live_rtp_header_extension_parse_video_orientation(char *buf, int len, int id, int *rotation);
static void janus_live_h264_parse_sps(char *buffer, int *width, int *height);
static void janus_live_rtp_unpack(janus_rtp_jb *jb, janus_frame_packet *packet, gboolean video);
static int janus_live_packet_insert(janus_live_pub *pub, janus_frame_packet *p);

static janus_adecoder_opus *janus_live_opus_decoder_create(uint32_t samplerate, int channels, gboolean fec);
static void janus_live_opus_decoder_destory(janus_adecoder_opus *dc);
static void janus_live_opus_decoder_decode(janus_adecoder_opus *dc, char *buf, int len);
static janus_aencoder_fdkaac *janus_live_fdkaac_encoder_create(int sample_rate, int channels, int bitrate);
static void janus_live_fdkaac_encoder_destory(janus_aencoder_fdkaac *ec);
static void janus_live_fdkaac_encoder_encode(janus_aencoder_fdkaac *ec, char *data, int len, uint32_t pts);
static void janus_live_fdkaac_encoder_encode_inernal(janus_aencoder_fdkaac *ec, char *data, int len, uint32_t pts);


janus_live_pub *
janus_live_pub_create(const char *url, const char *acodec, const char *vcodec)
{
	if(url == NULL) {
		JANUS_LOG(LOG_ERR, "Missing live url information\n");
		return NULL;
	}
    if(acodec == NULL && vcodec == NULL) {
        JANUS_LOG(LOG_ERR, "Audio Video must have one\n");
		return NULL;
    }
    /* Create the live pub */
	janus_live_pub *pub = g_malloc0(sizeof(janus_live_pub));
	pub->url = g_strdup(url);
    JANUS_LOG(LOG_INFO, "rtmp url:%s\n", pub->url);
    if(acodec)
	    pub->acodec = g_strdup(acodec);
    if(vcodec)
        pub->vcodec = g_strdup(vcodec);
	pub->created = janus_get_real_time();
    pub->init_flag = FALSE;
    pub->closed = FALSE;

    if(pub->acodec) {
        pub->audio_jb = g_malloc0(sizeof(janus_rtp_jb));
        pub->audio_jb->tb = 48000;
        pub->audio_jb->pub = pub;
        pub->audio_jb->adecoder = janus_live_opus_decoder_create(48000, 2, TRUE);
        pub->audio_jb->adecoder->jb = pub->audio_jb;
        pub->audio_jb->aencoder = janus_live_fdkaac_encoder_create(48000, 2, 128);
        pub->audio_jb->aencoder->jb = pub->audio_jb;
    }
    if(pub->vcodec) {
        pub->video_jb = g_malloc0(sizeof(janus_rtp_jb));
        pub->video_jb->tb = 90000;
        pub->video_jb->pub = pub;
        pub->video_jb->buflen = JANUS_LIVE_BUFFER_MAX;
        pub->video_jb->received_frame = g_malloc0(pub->video_jb->buflen);
    }

    /* thread start */
    int     id;
    char    tname[32];
    GError *error = NULL;

    id = 1;
    memset(tname, 0, 32);
    g_snprintf(tname, sizeof(tname), "jitter event loop, id:%d", id);
    janus_live_event_loop_init(pub, &pub->jb_loop, id, tname);
    if(pub->jb_loop){
        pub->jb_src = g_timeout_source_new(50);
        g_source_set_priority(pub->jb_src, G_PRIORITY_DEFAULT);
        g_source_set_callback(pub->jb_src, janus_rtp_jb_handle, pub, NULL);
        g_source_attach(pub->jb_src, pub->jb_loop->mainctx);
    }

    id = 2;
    memset(tname, 0, 32);
    g_snprintf(tname, sizeof(tname), "rtmp send event loop, id:%d", id);
    janus_live_event_loop_init(pub, &pub->pub_loop, id, tname);
    if(pub->pub_loop){
        pub->pub_src = g_timeout_source_new(50);
        g_source_set_priority(pub->pub_src, G_PRIORITY_DEFAULT);
        g_source_set_callback(pub->pub_src, janus_live_send_handle, pub, NULL);
        g_source_attach(pub->pub_src, pub->pub_loop->mainctx);
    }

    janus_mutex_init(&pub->mutex);
    janus_mutex_init(&pub->mutex_live);
    /* Done */
	g_atomic_int_set(&pub->destroyed, 0);
	janus_refcount_init(&pub->ref, janus_live_pub_free);
    return pub;
}


void
janus_live_event_loop_init(janus_live_pub *pub, janus_live_el **el, int id, char* name)
{
    GError *error       = NULL;
    janus_live_el *loop = NULL;

    loop = g_malloc0(sizeof(janus_live_el));
    loop->id = id;
    loop->mainctx = g_main_context_new();
    loop->mainloop = g_main_loop_new(loop->mainctx, FALSE);
    loop->name = g_strdup(name);
    loop->pub = pub;
    loop->thread = g_thread_try_new(loop->name, 
                    &janus_live_event_loop_thread,loop, &error);
    if(error != NULL) {
        g_free(loop->name);
	    loop->name = NULL;
        loop->pub = NULL;
        g_main_loop_unref(loop->mainloop);
        g_main_context_unref(loop->mainctx);
        g_free(loop);
        JANUS_LOG(LOG_ERR, "Got error %d (%s) trying to launch a new event loop thread,id:%d,name:%s\n",
                  error->code, error->message ? error->message : "??", id, name);
        *el = NULL;
        return;
    }

    *el = loop;
    return;
}


int
janus_live_pub_save_frame(janus_live_pub *pub, char *buffer, uint length, gboolean video, int slot)
{
	if(!pub)
		return -1;
	if(!buffer || length < 1) {
		return -2;
	}
	if(!pub->url) {
		return -3;
	}

    janus_rtp_jb *jb = video ? pub->video_jb : pub->audio_jb;
    if(!jb){
        return -4;
    }
	/* Write frame header */
    uint64_t max32 = UINT32_MAX;
    int skip = 0;
    int audiolevel = 0, rotation = 0, last_rotation = -1, rotated = -1;
    janus_rtp_header *rtp = (janus_rtp_header *)buffer;
    JANUS_LOG(LOG_INFO, "%s RTP packet, ssrc:%u, type:%d, sequence:%d, timestamp:%u, ext:%d\n",
            video ? "Video" : "Audio", ntohl(rtp->ssrc), rtp->type, ntohs(rtp->seq_number), ntohl(rtp->timestamp)*1000/jb->tb, rtp->extension);
    
    if(rtp->csrccount) {
        JANUS_LOG(LOG_VERB, "  -- -- Skipping CSRC list\n");
        skip += rtp->csrccount*4;
    }
    audiolevel = -1;
    rotation = -1;
    if(rtp->extension) {
        janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buffer+12+skip);
        JANUS_LOG(LOG_VERB, "  -- -- RTP extension (type=0x%"PRIX16", length=%"SCNu16")\n",
            ntohs(ext->type), ntohs(ext->length));
        skip += 4 + ntohs(ext->length)*4;
        if(pub->audio_level_extmap_id > 0)
            janus_live_rtp_header_extension_parse_audio_level(buffer, length, pub->audio_level_extmap_id, &audiolevel);
        if(pub->video_orient_extmap_id > 0) {
            janus_live_rtp_header_extension_parse_video_orientation(buffer, length, pub->video_orient_extmap_id, &rotation);
            if(rotation != -1 && rotation != last_rotation) {
                last_rotation = rotation;
                rotated++;
            }
        }
    }
    if(jb->ssrc == 0) {
        jb->ssrc = ntohl(rtp->ssrc);
        JANUS_LOG(LOG_INFO, "SSRC detected: %"SCNu32"\n", jb->ssrc);
    }
    if(jb->ssrc != ntohl(rtp->ssrc)) {
        JANUS_LOG(LOG_WARN, "Dropping packet with unexpected SSRC: %"SCNu32" != %"SCNu32"\n",
            ntohl(rtp->ssrc), jb->ssrc);
        return -5;
    }

    /* Generate frame packet and insert in the ordered list */
    janus_frame_packet *p = janus_packet_alloc(length);
    p->created = janus_get_real_time();
    memcpy(p->data, buffer, length);
    p->video = video;
    p->ssrc = ntohl(rtp->ssrc);
    p->seq = ntohs(rtp->seq_number);
    p->pt = rtp->type;
    //p->len = length;
    p->drop = 0;
    /* Due to resets, we need to mess a bit with the original timestamps */
    if(jb->last_ts == 0 && jb->start_ts == 0 && jb->start_sys == 0) {
        /* Simple enough... */
        p->ts = ntohl(rtp->timestamp);
        jb->start_ts = ntohl(rtp->timestamp);
        jb->start_sys = janus_get_real_time();
    } else {
        /* Is the new timestamp smaller than the next one, and if so, is it a timestamp reset or simply out of order? */ 
        gboolean late_pkt = FALSE;
        if(ntohl(rtp->timestamp) < jb->last_ts && (jb->last_ts-ntohl(rtp->timestamp) > 2*1000*1000*1000)) {
            if(jb->post_reset_pkts > post_reset_trigger) {
                jb->reset = ntohl(rtp->timestamp);
                JANUS_LOG(LOG_WARN, "Timestamp reset: %"SCNu32"\n", jb->reset);
                jb->times_resetted++;
                jb->post_reset_pkts = 0;
            }
        } else if(ntohl(rtp->timestamp) > jb->reset && ntohl(rtp->timestamp) > jb->last_ts &&
                (ntohl(rtp->timestamp)-jb->last_ts > 2*1000*1000*1000)) {
            if(jb->post_reset_pkts < post_reset_trigger) {
                JANUS_LOG(LOG_WARN, "Late pre-reset packet after a timestamp reset: %"SCNu32"\n", ntohl(rtp->timestamp));
                late_pkt = TRUE;
                jb->times_resetted--;
            }
        } else if(ntohl(rtp->timestamp) < jb->reset) {
            if(jb->post_reset_pkts < post_reset_trigger) {
                JANUS_LOG(LOG_WARN, "Updating latest timestamp reset: %"SCNu32" (was %"SCNu32")\n", ntohl(rtp->timestamp), jb->reset);
                jb->reset = ntohl(rtp->timestamp);
            } else {
                jb->reset = ntohl(rtp->timestamp);
                JANUS_LOG(LOG_WARN, "Timestamp reset: %"SCNu32"\n", jb->reset);
                jb->times_resetted++;
                jb->post_reset_pkts = 0;
            }
        }
        /* Take into account the number of resets when setting the internal, 64-bit, timestamp */
        p->ts = (jb->times_resetted*max32)+ntohl(rtp->timestamp);
        if(late_pkt)
            jb->times_resetted++;
    }
    if(rtp->padding) {
        /* There's padding data, let's check the last byte to see how much data we should skip */
        uint8_t padlen = (uint8_t)buffer[length - 1];
        JANUS_LOG(LOG_VERB, "Padding at sequence number %hu: %d/%d\n",
            ntohs(rtp->seq_number), padlen, length);
        p->len -= padlen;
        if((p->len - skip - 12) <= 0) {
            /* Only padding, take note that we should drop the packet later */
            p->drop = 1;
            JANUS_LOG(LOG_VERB, "  -- All padding, marking packet as dropped\n");
        }
    }
    if(p->len <= 12) {
        /* Only header? take note that we should drop the packet later */
        p->drop = 1;
        JANUS_LOG(LOG_VERB, "  -- Only RTP header, marking packet as dropped\n");
    }
    jb->last_ts = ntohl(rtp->timestamp);
    if(ntohs(rtp->seq_number) != jb->last_seq + 1){
        JANUS_LOG(LOG_VERB, "input %d sequence unorder, last:%d, curr:%d\n", video ? "Video" : "Audio", jb->last_seq, ntohs(rtp->seq_number));
    }
    jb->last_seq = ntohs(rtp->seq_number);
    jb->post_reset_pkts++;
    /* Fill in the rest of the details */
    p->skip = skip;
    p->audiolevel = audiolevel;
    p->rotation = rotation;
    p->next = NULL;
    p->prev = NULL;

    if(video)
        JANUS_LOG(LOG_VERB, "janus_live_pub_save_frame video ts: %"SCNu64"\n", p->ts);

    janus_mutex_lock_nodebug(&pub->mutex);
    if(jb->list == NULL) {
        /* First element becomes the list itself (and the last item), at least for now */
        jb->list = p;
        jb->last = p;
    } else if(!p->drop) {
        /* Check where we should insert this, starting from the end */
        int added = 0;
        janus_frame_packet *tmp = jb->last;
        while(tmp) {
            if(tmp->ts < p->ts) {
                /* The new timestamp is greater than the last one we have, append */
                added = 1;
                if(tmp->next != NULL) {
                    /* We're inserting */
                    tmp->next->prev = p;
                    p->next = tmp->next;
                } else {
                    /* Update the last packet */
                    jb->last = p;
                }
                tmp->next = p;
                p->prev = tmp;
                break;
            } else if(tmp->ts == p->ts) {
                /* Same timestamp, check the sequence number */
                if(tmp->seq < p->seq && (abs(tmp->seq - p->seq) < 10000)) {
                    /* The new sequence number is greater than the last one we have, append */
                    added = 1;
                    if(tmp->next != NULL) {
                        /* We're inserting */
                        tmp->next->prev = p;
                        p->next = tmp->next;
                    } else {
                        /* Update the last packet */
                        jb->last = p;
                    }
                    tmp->next = p;
                    p->prev = tmp;
                    break;
                } else if(tmp->seq > p->seq && (abs(tmp->seq - p->seq) > 10000)) {
                    /* The new sequence number (resetted) is greater than the last one we have, append */
                    added = 1;
                    if(tmp->next != NULL) {
                        /* We're inserting */
                        tmp->next->prev = p;
                        p->next = tmp->next;
                    } else {
                        /* Update the last packet */
                        jb->last = p;
                    }
                    tmp->next = p;
                    p->prev = tmp;
                    break;
                } else if(tmp->seq == p->seq) {
                    /* Maybe a retransmission? Skip */
                    JANUS_LOG(LOG_WARN, "Skipping duplicate packet (seq=%"SCNu16")\n", p->seq);
                    p->drop = 1;
                    break;
                }
            }
            /* If either the timestamp ot the sequence number we just got is smaller, keep going back */
            tmp = tmp->prev;
        }
        if(p->drop) {
            /* We don't need this */
            janus_packet_free(p);
        } else if(!added) {
            /* We reached the start */
            p->next = jb->list;
            jb->list->prev = p;
            jb->list = p;
        }
    }
    /* list size add */
    if(!p->drop)
        jb->size++;
	/* Done */
	janus_mutex_unlock_nodebug(&pub->mutex);
	return 0;
}


void *
janus_live_event_loop_thread(void *data)
{
	janus_live_el *loop = data;
    janus_live_pub *pub = (janus_live_pub *)loop->pub;

	JANUS_LOG(LOG_VERB, "[loop#%d] Event loop [%s] thread started\n", loop->id, loop->name);
	if(loop->mainloop == NULL) {
		JANUS_LOG(LOG_ERR, "[loop#%d] Invalid loop...\n", loop->id);
		g_thread_unref(g_thread_self());
		return NULL;
	}
	JANUS_LOG(LOG_DBG, "[loop#%d] Looping...\n", loop->id);
	g_main_loop_run(loop->mainloop);
	/* When the loop quits, we can unref it */
	g_main_loop_unref(loop->mainloop);
	g_main_context_unref(loop->mainctx);
	JANUS_LOG(LOG_VERB, "[loop#%d] Event loop [%s] thread ended!\n", loop->id, loop->name);
	return NULL;
}


gboolean
janus_rtp_jb_handle(gpointer user_data)
{
	janus_live_pub *pub = (janus_live_pub *)user_data;
    gint64 now = janus_get_real_time();
    janus_frame_packet *tmp = NULL, *head = NULL;
    janus_rtp_jb *jb = NULL;
   
    /*audio */
    janus_mutex_lock_nodebug(&pub->mutex);
    head = pub->audio_jb->list;
    while(head){
        jb = pub->audio_jb;
        gint64 gap = (now - jb->start_sys) - (1000 * (head->ts - jb->start_ts)*1000/jb->tb);
        gint64 timetout = now - head->created;
        if(now - head->created > G_USEC_PER_SEC){
            tmp = head->next;

            if(tmp){
                tmp->prev = NULL;
            }
            head->next = NULL;
            pub->audio_jb->size--;

            JANUS_LOG(LOG_INFO, "janus_rtp_jb_handle, audio sequence:%d, gap:%"SCNu64", timeout:%"SCNu64"\n", head->seq, gap/1000, timetout/1000);
            if(head->seq != pub->audio_jb->last_seq_out + 1){
                JANUS_LOG(LOG_WARN, "output %d sequence unorder, last:%d, curr:%d\n", "Audio", pub->audio_jb->last_seq_out, head->seq);
            }
            pub->audio_jb->last_seq_out = head->seq;

            /* opus unpack */
            int len = 0;
            char *buffer = janus_rtp_payload(head->data, head->len, &len);
            JANUS_LOG(LOG_INFO, "audio frame len: %d\n", len);
            if(jb->adecoder){
                janus_live_opus_decoder_decode(jb->adecoder, head->data, head->len);
            }

            janus_packet_free(head);
            head = tmp;
        }else{
            break;
        }
    }
    pub->audio_jb->list = head;
    janus_mutex_unlock_nodebug(&pub->mutex);
    
    /*video */
    janus_mutex_lock_nodebug(&pub->mutex);
    head = pub->video_jb->list;
    while(head){
        jb = pub->video_jb;
        gint64 gap = (now - jb->start_sys) - (1000 * (head->ts - jb->start_ts)*1000/jb->tb);
        gint64 timetout = now - head->created;
        if(now - head->created > G_USEC_PER_SEC){
            tmp = head->next;

            if(tmp){
                tmp->prev = NULL;
            }
            head->next = NULL;
            pub->video_jb->size--;

            JANUS_LOG(LOG_INFO, "janus_rtp_jb_handle, video sequence:%d, gap:%"SCNu64", timeout:%"SCNu64"\n", head->seq, gap/1000, timetout/1000);
            if(head->seq != pub->video_jb->last_seq_out + 1){
                JANUS_LOG(LOG_WARN, "output %d sequence unorder, last:%d, curr:%d\n", "Video", pub->video_jb->last_seq_out, head->seq);
            }
            pub->video_jb->last_seq_out = head->seq;

            /* h264 unpack */
            if(!head->drop){
                if(pub->video_jb->ts != head->ts && pub->video_jb->frameLen) {
                    uint8_t type = *(pub->video_jb->received_frame + 3) & 0x1F;

                    janus_frame_packet *p = janus_packet_alloc(pub->video_jb->frameLen + FF_INPUT_BUFFER_PADDING_SIZE);
                    p->created = now;
                    memcpy(p->data, pub->video_jb->received_frame, pub->video_jb->frameLen);
                    p->video = TRUE;
                    p->keyFrame = pub->video_jb->keyFrame;
                    p->ts = pub->video_jb->ts *1000/jb->tb;
                    JANUS_LOG(LOG_INFO, "video frame len: %d, nalu type:%d, rtpts:%"SCNu64", ts:%"SCNu64"\n",
                        pub->video_jb->frameLen, type, pub->video_jb->ts, p->ts);

                    janus_live_packet_insert(pub, p);
                    pub->video_jb->frameLen = 0;
                    pub->video_jb->keyFrame = 0;
                }

                janus_live_rtp_unpack(pub->video_jb, head, TRUE);
                pub->video_jb->ts = head->ts;
            }
            janus_packet_free(head);
            head = tmp;
        }else{
            break;
        }
    }
    pub->video_jb->list = head;
    janus_mutex_unlock_nodebug(&pub->mutex);

    JANUS_LOG(LOG_INFO, "janus_rtp_jb_handle, ajb:%d, vjb:%d\n", pub->audio_jb->size, pub->video_jb->size);
	return G_SOURCE_CONTINUE;
}


int
janus_live_packet_insert(janus_live_pub *pub, janus_frame_packet *p)
{
    janus_mutex_lock_nodebug(&pub->mutex_live);
    if(pub->start_ts == 0 && pub->start_sys == 0) {
        pub->start_ts = p->ts;
        pub->start_sys = janus_get_real_time();
    } 
    JANUS_LOG(LOG_INFO, "janus_live_packet_insert, ts:%"SCNu64", len:%d, size:%d\n",  p->ts, p->len, pub->size);
    if(pub->list == NULL) {
        /* First element becomes the list itself (and the last item), at least for now */
        pub->list = p;
        pub->last = p;
    } else if(!p->drop) {
        /* Check where we should insert this, starting from the end */
        int added = 0;
        janus_frame_packet *tmp = pub->last;
        while(tmp) {
            if(tmp->ts <= p->ts) {
                /* The new timestamp is greater than the last one we have, append */
                added = 1;
                if(tmp->next != NULL) {
                    /* We're inserting */
                    tmp->next->prev = p;
                    p->next = tmp->next;
                } else {
                    /* Update the last packet */
                    pub->last = p;
                }
                tmp->next = p;
                p->prev = tmp;
                break;
            }
            /* If either the timestamp ot the sequence number we just got is smaller, keep going back */
            tmp = tmp->prev;
        }
        if(!added) {
            /* We reached the start */
            p->next = pub->list;
            pub->list->prev = p;
            pub->list = p;
        }
    }
    /* list size add */
    pub->size++;
	janus_mutex_unlock_nodebug(&pub->mutex_live);
}


void
janus_live_rtp_unpack(janus_rtp_jb *jb, janus_frame_packet *packet, gboolean video)
{
    janus_live_pub *pub = jb->pub;
    int len = 0;
    char *buffer = janus_rtp_payload(packet->data, packet->len, &len);
    if(len < 1) {
        return;
    }

    if((buffer[0] & 0x1F) == 7) {
        /* SPS, see if we can extract the width/height as well */
        int width = 0, height = 0;
        janus_live_h264_parse_sps(buffer, &width, &height);
        JANUS_LOG(LOG_VERB, "Parsing width/height: %dx%d\n", width, height);
        if(width > pub->max_width)
            pub->max_width = width;
        if(height > pub->max_height)
            pub->max_height = height;
    } else if((buffer[0] & 0x1F) == 24) {
        /* May we find an SPS in this STAP-A? */
        JANUS_LOG(LOG_HUGE, "Parsing STAP-A...\n");
        char *buf = buffer;
        buf++;
        int tot = len-1;
        uint16_t psize = 0;
        while(tot > 0) {
            memcpy(&psize, buf, 2);
            psize = ntohs(psize);
            buf += 2;
            tot -= 2;
            int nal = *buf & 0x1F;
            JANUS_LOG(LOG_HUGE, "  -- NALU of size %u: %d\n", psize, nal);
            if(nal == 7) {
                int width = 0, height = 0;
                janus_live_h264_parse_sps(buf, &width, &height);
                JANUS_LOG(LOG_VERB, "Parsing width/height: %dx%d\n", width, height);
                if(width > pub->max_width)
                    pub->max_width = width;
                if(height > pub->max_height)
                    pub->max_height = height;
            }
            buf += psize;
            tot -= psize;
        }
    }

    if(!pub->init_flag && pub->max_width && pub->max_height) {
        int rc = janus_live_ffmpeg_init(pub);
        if(rc == 0)
            pub->init_flag = TRUE;
    }

    /* H.264 depay */
    int jump = 0;
    uint8_t fragment = *buffer & 0x1F;
    uint8_t nal = *(buffer+1) & 0x1F;
    uint8_t start_bit = *(buffer+1) & 0x80;
    uint8_t end_bit = *(buffer+1) & 0x40;
    if(fragment == 28 || fragment == 29)
        JANUS_LOG(LOG_HUGE, "%s Fragment=%d, NAL=%d, Start=%d End=%d (len=%d, frameLen=%d)\n", 
                video ? "video" : "audio", fragment, nal, start_bit, end_bit, len, jb->frameLen);
    else
        JANUS_LOG(LOG_HUGE, "%s Fragment=%d (len=%d, frameLen=%d)\n", video ? "video" : "audio", fragment, len, jb->frameLen);
    if(fragment == 5 ||
            ((fragment == 28 || fragment == 29) && nal == 5 && start_bit == 128)) {
        JANUS_LOG(LOG_VERB, "(seq=%"SCNu16", ts=%"SCNu64") Key frame\n", packet->seq, packet->ts);
        jb->keyFrame = 1;
        /* Is this the first keyframe we find? */
        if(!jb->keyframe_found) {
            jb->keyframe_found = TRUE;
            JANUS_LOG(LOG_INFO, "First keyframe: %"SCNu64"\n", packet->ts - jb->start_ts);
        }
    }    
    /* Frame manipulation */
    if((fragment > 0) && (fragment < 24)) {	/* Add a start code */
        uint8_t *temp = jb->received_frame + jb->frameLen;
        memset(temp, 0x00, 1);
        memset(temp + 1, 0x00, 1);
        memset(temp + 2, 0x01, 1);
        jb->frameLen += 3;
    } else if(fragment == 24) {	/* STAP-A */
        /* De-aggregate the NALs and write each of them separately */
        buffer++;
        int tot = len-1;
        uint16_t psize = 0;
        while(tot > 0) {
            memcpy(&psize, buffer, 2);
            psize = ntohs(psize);
            buffer += 2;
            tot -= 2;
            /* Now we have a single NAL */
            uint8_t *temp = jb->received_frame + jb->frameLen;
            memset(temp, 0x00, 1);
            memset(temp + 1, 0x00, 1);
            memset(temp + 2, 0x01, 1);
            // memset(temp + 3, 0x01, 1);
            jb->frameLen += 3;
            memcpy(jb->received_frame + jb->frameLen, buffer, psize);
            jb->frameLen += psize;
            /* Go on */
            buffer += psize;
            tot -= psize;
        }
        return;

    } else if((fragment == 28) || (fragment == 29)) {	/* FIXME true fr FU-A, not FU-B */
        uint8_t indicator = *buffer;
        uint8_t header = *(buffer+1);
        jump = 2;
        len -= 2;
        if(header & 0x80) {
            /* First part of fragmented packet (S bit set) */
            uint8_t *temp = jb->received_frame + jb->frameLen;
            memset(temp, 0x00, 1);
            memset(temp + 1, 0x00, 1);
            memset(temp + 2, 0x01, 1);
            memset(temp + 3, (indicator & 0xE0) | (header & 0x1F), 1);
            jb->frameLen += 4;
        } else if (header & 0x40) {
            /* Last part of fragmented packet (E bit set) */
        }
    }
    memcpy(jb->received_frame + jb->frameLen, buffer+jump, len);
    jb->frameLen += len;
    if(len == 0){
        JANUS_LOG(LOG_ERR, "nalu is null\n");
    }
}


gboolean
janus_live_send_handle(gpointer user_data)
{
	janus_live_pub *pub = (janus_live_pub *)user_data;
    gint64 now = janus_get_real_time();
    janus_frame_packet *tmp = NULL, *head = NULL;
    /*audio */
    janus_mutex_lock_nodebug(&pub->mutex_live);
    head = pub->list;
    while(head){
        gint64 gap = (now - pub->start_sys) - (1000 * (head->ts - pub->start_ts));
        gint64 timetout = now - head->created;
        if(now - head->created > G_USEC_PER_SEC) {
            tmp = head->next;

            if(tmp){
                tmp->prev = NULL;
            }
            head->next = NULL;
            pub->size--;

            JANUS_LOG(LOG_INFO, "janus_live_send_handle, %s packet len:%d,"
                    " ts:%"SCNu64", gap:%"SCNu64", timeout:%"SCNu64", list:%d\n", 
                    head->video ? "video":"audio",head->len,head->ts,
                    gap/1000, timetout/1000, pub->size);
            
            if(head->len > 0) {
                if(head->video){
                    /* Save the frame */
                    AVPacket *packet = av_packet_alloc();
                    av_init_packet(packet);
                    packet->stream_index = 0;
                    packet->data = head->data;
                    packet->size = head->len;
                    if(head->keyFrame)
                        packet->flags |= AV_PKT_FLAG_KEY;

                    packet->dts = (uint32_t)head->ts;
                    packet->pts = (uint32_t)head->ts;
                    if(pub->fctx) {
                        int res = av_interleaved_write_frame(pub->fctx, packet);
                        if(res < 0) {
                            JANUS_LOG(LOG_ERR, "Error writing video frame to file... (error %d)\n", res);
                        }
                    }
                    av_packet_free(&packet);
                    JANUS_LOG(LOG_INFO, "rtmp video packet (len:%d) tb:%d pts:%d listsize:%d\n", head->len, pub->vStream->time_base.den, head->ts, pub->size);
                }else{
                    AVPacket *packet = av_packet_alloc();
                    av_init_packet(packet);
                    packet->stream_index = 1;
                    packet->data = head->data;
                    packet->size = head->len;
                    packet->dts = (uint32_t)head->ts;
                    packet->pts = (uint32_t)head->ts;
                    av_bitstream_filter_filter(pub->aacbsf, pub->aStream->codec, NULL, &packet->data, &packet->size, packet->data, packet->size, 0);
                    if(pub->fctx) {
                        int res = av_write_frame(pub->fctx, packet);
                        if(res < 0) {
                            JANUS_LOG(LOG_ERR, "Error writing audio frame to file... (error %d)\n", res);
                        }
                    }
                    av_free(packet->data); /* https://blog.csdn.net/bikeytang/article/details/60883987# */
                    av_packet_free(&packet);
                    JANUS_LOG(LOG_INFO, "rtmp audio packet (len:%d) tb:%d pts:%d listsize:%d\n", head->len, pub->aStream->time_base.den, head->ts, pub->size);
                }
            }
        
            janus_packet_free(head);
            head = tmp;
        }else{
            break;
        }
    }
    pub->list = head;
    janus_mutex_unlock_nodebug(&pub->mutex_live);
	
	return G_SOURCE_CONTINUE;
}

int
janus_live_ffmpeg_free(janus_live_pub *pub)
{
    if(pub->fctx != NULL)
		av_write_trailer(pub->fctx);

#ifdef USE_CODECPAR
	if(pub->vEncoder != NULL)
		avcodec_close(pub->vEncoder);
    if(pub->aEncoder != NULL)
		avcodec_close(pub->aEncoder);
#else
	if(pub->vStream != NULL && pub->vStream->codec != NULL)
		avcodec_close(pub->vStream->codec);
#endif

	if(pub->fctx != NULL && pub->fctx->streams[0] != NULL) {
#ifndef USE_CODECPAR
		av_free(pub->fctx->streams[0]->codec);
#endif
		av_free(pub->fctx->streams[0]);
        av_free(pub->fctx->streams[1]);
	}

	if(pub->fctx != NULL) {
		avio_close(pub->fctx->pb);
        av_free(pub->fctx);
	}
    if(pub->aacbsf != NULL) {
        av_bitstream_filter_close(pub->aacbsf);
        pub->aacbsf = NULL;
    }
    return 0;
}


int
janus_live_ffmpeg_init(janus_live_pub *pub)
{
    /* Setup FFmpeg */
	av_register_all();
	/* Adjust logging to match the postprocessor's */
	av_log_set_level(janus_log_level <= LOG_NONE ? AV_LOG_QUIET :
		(janus_log_level == LOG_FATAL ? AV_LOG_FATAL :
			(janus_log_level == LOG_ERR ? AV_LOG_ERROR :
				(janus_log_level == LOG_WARN ? AV_LOG_WARNING :
					(janus_log_level == LOG_INFO ? AV_LOG_INFO :
						(janus_log_level == LOG_VERB ? AV_LOG_VERBOSE : AV_LOG_DEBUG))))));
    pub->aacbsf = av_bitstream_filter_init("aac_adtstoasc");
    if(pub->aacbsf == NULL) {
		JANUS_LOG(LOG_ERR, "Error allocating aac_adtstoasc\n");
		return -1;
	}
	/* rtmp output */
	pub->fctx = avformat_alloc_context();
	if(pub->fctx == NULL) {
		JANUS_LOG(LOG_ERR, "Error allocating context\n");
		return -1;
	}
    const char* metadata = "bepartfoyou (七曦)";
	/* We save the metadata part as a comment (see #1189) */
	if(metadata)
		av_dict_set(&pub->fctx->metadata, "comment", metadata, 0);
	pub->fctx->oformat = av_guess_format("flv", NULL, NULL);
	if(pub->fctx->oformat == NULL) {
		JANUS_LOG(LOG_ERR, "Error guessing format\n");
		return -1;
	}
	snprintf(pub->fctx->filename, sizeof(pub->fctx->filename), "%s", pub->url);
#ifdef USE_CODECPAR
    /*video */
	AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if(!codec) {
		/* Error opening video codec */
		JANUS_LOG(LOG_ERR, "Encoder not available\n");
		return -1;
	}
	pub->fctx->video_codec = codec;
	pub->fctx->oformat->video_codec = codec->id;
	pub->vStream = avformat_new_stream(pub->fctx, codec);
	// pub->vStream->id = pub->fctx->nb_streams - 1;
    pub->vStream->id = 0;
	pub->vEncoder = avcodec_alloc_context3(codec);
	pub->vEncoder->width = pub->max_width;
	pub->vEncoder->height = pub->max_height;
	pub->vEncoder->time_base = (AVRational){ 1, 25 };
	pub->vEncoder->pix_fmt = AV_PIX_FMT_YUV420P;
	pub->vEncoder->flags |= CODEC_FLAG_GLOBAL_HEADER;
	if(avcodec_open2(pub->vEncoder, codec, NULL) < 0) {
		/* Error opening video codec */
		JANUS_LOG(LOG_ERR, "Encoder error\n");
		return -1;
	}
	avcodec_parameters_from_context(pub->vStream->codecpar, pub->vEncoder);

    /*audio */
    AVCodec *acodec = avcodec_find_encoder_by_name("libfdk_aac");
	if(!acodec) {
		/* Error opening video codec */
		JANUS_LOG(LOG_ERR, "Encoder not available\n");
		return -1;
	}
    pub->fctx->audio_codec = acodec;
	pub->fctx->oformat->audio_codec = acodec->id;
	pub->aStream = avformat_new_stream(pub->fctx, acodec);
	//pub->vStream->id = pub->fctx->nb_streams-1;
    pub->aStream->id = 1;
	pub->aEncoder = avcodec_alloc_context3(acodec);
    pub->aEncoder->codec_type = AVMEDIA_TYPE_AUDIO;
    pub->aEncoder->sample_rate = 48000;
    pub->aEncoder->bit_rate = 128 * 1000;
    pub->aEncoder->bit_rate_tolerance = 128 * 1000 * 3 / 2;
    pub->aEncoder->channels = 2;
    pub->aEncoder->channel_layout = AV_CH_LAYOUT_STEREO;
    pub->aEncoder->time_base.num = 1;
    pub->aEncoder->time_base.den = pub->aEncoder->sample_rate;
    pub->aEncoder->sample_fmt = AV_SAMPLE_FMT_S16;
	pub->aEncoder->flags |= CODEC_FLAG_GLOBAL_HEADER;
	if(avcodec_open2(pub->aEncoder, acodec, NULL) < 0) {
		/* Error opening video codec */
		JANUS_LOG(LOG_ERR, "Encoder error\n");
		return -1;
	}
	avcodec_parameters_from_context(pub->aStream->codecpar, pub->aEncoder);
#else

	pub->vStream = avformat_new_stream(pub->fctx, 0);
	if(pub->vStream == NULL) {
		JANUS_LOG(LOG_ERR, "Error adding stream\n");
		return -1;
	}
#if LIBAVCODEC_VER_AT_LEAST(53, 21)
	avcodec_get_context_defaults3(pub->vStream->codec, AVMEDIA_TYPE_VIDEO);
#else
	avcodec_get_context_defaults2(pub->vStream->codec, AVMEDIA_TYPE_VIDEO);
#endif
#if LIBAVCODEC_VER_AT_LEAST(54, 25)
	pub->vStream->codec->codec_id = AV_CODEC_ID_H264;
#else
	pub->vStream->codec->codec_id = CODEC_ID_H264;
#endif
	pub->vStream->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	pub->vStream->codec->time_base = (AVRational){1, 25};
	pub->vStream->time_base = (AVRational){1, 90000};
	pub->vStream->codec->width = pub->max_width;
	pub->vStream->codec->height = pub->max_height;
	pub->vStream->codec->pix_fmt = PIX_FMT_YUV420P;
	//~ if (fctx->flags & AVFMT_GLOBALHEADER)
		pub->vStream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
#endif
	if(avio_open(&pub->fctx->pb, pub->fctx->filename, AVIO_FLAG_WRITE) < 0) {
		JANUS_LOG(LOG_ERR, "Error opening file for output\n");
		return -1;
	}
	if(avformat_write_header(pub->fctx, NULL) < 0) {
		JANUS_LOG(LOG_ERR, "Error writing header\n");
		return -1;
	}

    JANUS_LOG(LOG_ERR, "ffmpeg init success .....\n");
	return 0;
}


void
janus_rtp_jb_free(janus_live_pub *pub)
{
    janus_mutex_lock_nodebug(&pub->mutex);
    janus_frame_packet *tmp = NULL, *head = NULL;
    /*audio */
    head = pub->audio_jb->list;
    while(head){
        tmp = head->next;
        if(tmp){
            tmp->prev = NULL;
        }
        head->next = NULL;
        pub->audio_jb->size--;
        janus_packet_free(head);
        head = tmp;
    }
    pub->audio_jb->list = head;
    
    /*video */
    head = pub->video_jb->list;
    while(head){
        tmp = head->next;
        if(tmp){
            tmp->prev = NULL;
        }
        head->next = NULL;
        pub->video_jb->size--;
        janus_packet_free(head);
        head = tmp;
    }
    pub->video_jb->list = head;
    if(pub->video_jb->received_frame){
        g_free(pub->video_jb->received_frame);
        pub->video_jb->received_frame = NULL;
    }
	janus_mutex_unlock_nodebug(&pub->mutex);
}


janus_frame_packet *
janus_packet_alloc(int data_len)
{
    janus_frame_packet *pkt = g_malloc0(sizeof(janus_frame_packet));
    memset(pkt, 0, sizeof(janus_frame_packet));
    pkt->len = data_len;
    pkt->data = g_malloc0(pkt->len);

    return  pkt;
}


void
janus_packet_free(janus_frame_packet *pkt)
{
    if(!pkt)
        return;
    if(pkt->data){
        g_free(pkt->data);
        pkt->data = NULL;
    }
    g_free(pkt);
    pkt = NULL;
}


/* Static helper to quickly find the extension data */
static int
janus_live_rtp_header_extension_find(char *buf, int len, int id,
		uint8_t *byte, uint32_t *word, char **ref)
{
	if(!buf || len < 12)
		return -1;
	janus_rtp_header *rtp = (janus_rtp_header *)buf;
	int hlen = 12;
	if(rtp->csrccount)	/* Skip CSRC if needed */
		hlen += rtp->csrccount*4;
	if(rtp->extension) {
		janus_rtp_header_extension *ext = (janus_rtp_header_extension *)(buf+hlen);
		int extlen = ntohs(ext->length)*4;
		hlen += 4;
		if(len > (hlen + extlen)) {
			/* 1-Byte extension */
			if(ntohs(ext->type) == 0xBEDE) {
				const uint8_t padding = 0x00, reserved = 0xF;
				uint8_t extid = 0, idlen;
				int i = 0;
				while(i < extlen) {
					extid = buf[hlen+i] >> 4;
					if(extid == reserved) {
						break;
					} else if(extid == padding) {
						i++;
						continue;
					}
					idlen = (buf[hlen+i] & 0xF)+1;
					if(extid == id) {
						/* Found! */
						if(byte)
							*byte = buf[hlen+i+1];
						if(word)
							*word = ntohl(*(uint32_t *)(buf+hlen+i));
						if(ref)
							*ref = &buf[hlen+i];
						return 0;
					}
					i += 1 + idlen;
				}
			}
			hlen += extlen;
		}
	}
	return -1;
}

int
janus_live_rtp_header_extension_parse_audio_level(char *buf, int len, int id, int *level)
{
	uint8_t byte = 0;
	if(janus_live_rtp_header_extension_find(buf, len, id, &byte, NULL, NULL) < 0)
		return -1;
	/* a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level */
	int value = byte & 0x7F;
	if(level)
		*level = value;
	return 0;
}


int
janus_live_rtp_header_extension_parse_video_orientation(char *buf, int len, int id, int *rotation)
{
	uint8_t byte = 0;
	if(janus_live_rtp_header_extension_find(buf, len, id, &byte, NULL, NULL) < 0)
		return -1;
	/* a=extmap:4 urn:3gpp:video-orientation */
	gboolean r1bit = (byte & 0x02) >> 1;
	gboolean r0bit = byte & 0x01;
	if(rotation) {
		if(!r0bit && !r1bit)
			*rotation = 0;
		else if(r0bit && !r1bit)
			*rotation = 90;
		else if(!r0bit && r1bit)
			*rotation = 180;
		else if(r0bit && r1bit)
			*rotation = 270;
	}
	return 0;
}


void
janus_live_pub_free(const janus_refcount *pub_ref)
{
    janus_frame_packet *tmp = NULL, *head = NULL;
	janus_live_pub *pub = janus_refcount_containerof(pub_ref, janus_live_pub, ref);
	/* This pub can be destroyed, free all the resources */
    if(!pub->closed)
	    janus_live_pub_close(pub);

    g_free(pub->url);
	pub->url = NULL;
    if(pub->acodec){
        g_free(pub->acodec);
	    pub->acodec = NULL;
    }
    if(pub->vcodec){
        g_free(pub->vcodec);
	    pub->vcodec = NULL;
    }

    janus_rtp_jb_free(pub);
    janus_live_ffmpeg_free(pub);
    head = pub->list;
    while(head){
        tmp = head->next;
        if(tmp){
            tmp->prev = NULL;
        }
        head->next = NULL;
        pub->size--;
        janus_packet_free(head);
        head = tmp;
    }
    JANUS_LOG(LOG_WARN, "janus live pusblish release, list:%d\n", pub->size);
	g_free(pub);
}


int
janus_live_pub_close(janus_live_pub *pub)
{
	if(!pub)
		return -1;
	janus_mutex_lock_nodebug(&pub->mutex);
    // jb thead stop
    if(pub->jb_loop && pub->jb_loop->mainloop != NULL &&
        g_main_loop_is_running(pub->jb_loop->mainloop)){
        g_main_loop_quit(pub->jb_loop->mainloop);

        if(pub->jb_src) {
			g_source_destroy(pub->jb_src);
			g_source_unref(pub->jb_src);
			pub->jb_src = NULL;
		}
        g_free(pub->jb_loop->name);
	    pub->jb_loop->name = NULL;
        pub->jb_loop->pub = NULL;
        g_main_loop_unref(pub->jb_loop->mainloop);
        g_main_context_unref(pub->jb_loop->mainctx);
        
        pub->jb_loop->mainloop = NULL;
        g_thread_join(pub->jb_loop->thread);
    }
    g_free(pub->jb_loop);
    pub->jb_loop = NULL;
    janus_mutex_unlock_nodebug(&pub->mutex);

    janus_mutex_lock_nodebug(&pub->mutex_live);
    // live send thread stop
    if(pub->pub_loop && pub->pub_loop->mainloop != NULL &&
        g_main_loop_is_running(pub->pub_loop->mainloop)){
        g_main_loop_quit(pub->pub_loop->mainloop);

        if(pub->pub_src) {
			g_source_destroy(pub->pub_src);
			g_source_unref(pub->pub_src);
			pub->pub_src = NULL;
		}
        g_free(pub->pub_loop->name);
	    pub->pub_loop->name = NULL;
        pub->pub_loop->pub = NULL;
        g_main_loop_unref(pub->pub_loop->mainloop);
        g_main_context_unref(pub->pub_loop->mainctx);

        pub->pub_loop->mainloop = NULL;
        g_thread_join(pub->pub_loop->thread);
    }
    g_free(pub->pub_loop);
    pub->pub_loop = NULL;
    janus_mutex_unlock_nodebug(&pub->mutex_live);

    pub->closed = TRUE;
    JANUS_LOG(LOG_WARN, "janus live pusblish event thread closed\n");
	return 0;
}


void
janus_live_pub_destroy(janus_live_pub *pub)
{
	if(!pub || !g_atomic_int_compare_and_exchange(&pub->destroyed, 0, 1))
		return;
	janus_refcount_decrease(&pub->ref);
}


/* Helpers to decode Exp-Golomb */
static uint32_t
janus_pp_h264_eg_getbit(uint8_t *base, uint32_t offset)
{
	return ((*(base + (offset >> 0x3))) >> (0x7 - (offset & 0x7))) & 0x1;
}


static uint32_t
janus_pp_h264_eg_decode(uint8_t *base, uint32_t *offset)
{
	uint32_t zeros = 0;
	while(janus_pp_h264_eg_getbit(base, (*offset)++) == 0)
		zeros++;
	uint32_t res = 1 << zeros;
	int32_t i = 0;
	for(i=zeros-1; i>=0; i--) {
		res |= janus_pp_h264_eg_getbit(base, (*offset)++) << i;
	}
	return res-1;
}


/* Helper to parse a SPS (only to get the video resolution) */
void
janus_live_h264_parse_sps(char *buffer, int *width, int *height)
{
	/* Let's check if it's the right profile, first */
	int index = 1;
	int profile_idc = *(buffer+index);
	if(profile_idc != 66) {
		JANUS_LOG(LOG_WARN, "Profile is not baseline (%d != 66)\n", profile_idc);
	}
	/* Then let's skip 2 bytes and evaluate/skip the rest */
	index += 3;
	uint32_t offset = 0;
	uint8_t *base = (uint8_t *)(buffer+index);
	/* Skip seq_parameter_set_id */
	janus_pp_h264_eg_decode(base, &offset);
	if(profile_idc >= 100) {
		/* Skip chroma_format_idc */
		janus_pp_h264_eg_decode(base, &offset);
		/* Skip bit_depth_luma_minus8 */
		janus_pp_h264_eg_decode(base, &offset);
		/* Skip bit_depth_chroma_minus8 */
		janus_pp_h264_eg_decode(base, &offset);
		/* Skip qpprime_y_zero_transform_bypass_flag */
		janus_pp_h264_eg_getbit(base, offset++);
		/* Skip seq_scaling_matrix_present_flag */
		janus_pp_h264_eg_getbit(base, offset++);
	}
	/* Skip log2_max_frame_num_minus4 */
	janus_pp_h264_eg_decode(base, &offset);
	/* Evaluate pic_order_cnt_type */
	int pic_order_cnt_type = janus_pp_h264_eg_decode(base, &offset);
	if(pic_order_cnt_type == 0) {
		/* Skip log2_max_pic_order_cnt_lsb_minus4 */
		janus_pp_h264_eg_decode(base, &offset);
	} else if(pic_order_cnt_type == 1) {
		/* Skip delta_pic_order_always_zero_flag, offset_for_non_ref_pic,
		 * offset_for_top_to_bottom_field and num_ref_frames_in_pic_order_cnt_cycle */
		janus_pp_h264_eg_getbit(base, offset++);
		janus_pp_h264_eg_decode(base, &offset);
		janus_pp_h264_eg_decode(base, &offset);
		int num_ref_frames_in_pic_order_cnt_cycle = janus_pp_h264_eg_decode(base, &offset);
		int i = 0;
		for(i=0; i<num_ref_frames_in_pic_order_cnt_cycle; i++) {
			janus_pp_h264_eg_decode(base, &offset);
		}
	}
	/* Skip max_num_ref_frames and gaps_in_frame_num_value_allowed_flag */
	janus_pp_h264_eg_decode(base, &offset);
	janus_pp_h264_eg_getbit(base, offset++);
	/* We need the following three values */
	int pic_width_in_mbs_minus1 = janus_pp_h264_eg_decode(base, &offset);
	int pic_height_in_map_units_minus1 = janus_pp_h264_eg_decode(base, &offset);
	int frame_mbs_only_flag = janus_pp_h264_eg_getbit(base, offset++);
	if(!frame_mbs_only_flag) {
		/* Skip mb_adaptive_frame_field_flag */
		janus_pp_h264_eg_getbit(base, offset++);
	}
	/* Skip direct_8x8_inference_flag */
	janus_pp_h264_eg_getbit(base, offset++);
	/* We need the following value to evaluate offsets, if any */
	int frame_cropping_flag = janus_pp_h264_eg_getbit(base, offset++);
	int frame_crop_left_offset = 0, frame_crop_right_offset = 0,
		frame_crop_top_offset = 0, frame_crop_bottom_offset = 0;
	if(frame_cropping_flag) {
		frame_crop_left_offset = janus_pp_h264_eg_decode(base, &offset);
		frame_crop_right_offset = janus_pp_h264_eg_decode(base, &offset);
		frame_crop_top_offset = janus_pp_h264_eg_decode(base, &offset);
		frame_crop_bottom_offset = janus_pp_h264_eg_decode(base, &offset);
	}
	/* Skip vui_parameters_present_flag */
	janus_pp_h264_eg_getbit(base, offset++);

	/* We skipped what we didn't care about and got what we wanted, compute width/height */
	if(width)
		*width = ((pic_width_in_mbs_minus1 +1)*16) - frame_crop_left_offset*2 - frame_crop_right_offset*2;
	if(height)
		*height = ((2 - frame_mbs_only_flag)* (pic_height_in_map_units_minus1 +1) * 16) - (frame_crop_top_offset * 2) - (frame_crop_bottom_offset * 2);
}


janus_adecoder_opus *
janus_live_opus_decoder_create(uint32_t samplerate, int channels, gboolean fec)
{
    int error = 0;
    janus_adecoder_opus *dc = g_malloc0(sizeof(janus_adecoder_opus));
	dc->channels = channels;
    dc->sampling_rate = samplerate;
    dc->fec = FALSE;
    dc->expected_seq = 0;
    dc->probation = 0;
    dc->last_timestamp = 0;
    dc->decoder = opus_decoder_create(dc->sampling_rate, dc->channels, &error);
    if(error != OPUS_OK) {
	    JANUS_LOG(LOG_ERR, "Error create Opus decoder...\n");
        g_free(dc);
        return NULL;
    }

    if(fec){
        dc->fec = TRUE;
        dc->probation = AUDIO_MIN_SEQUENTIAL;
    }
    
    return dc;
}


void
janus_live_opus_decoder_destory(janus_adecoder_opus *dc)
{
    if(dc){
        opus_decoder_destroy(dc);
        g_free(dc);
    }
}


void
janus_live_opus_decoder_decode(janus_adecoder_opus *dc, char *buf, int len)
{
    janus_rtp_header *rtp = (janus_rtp_header *)buf;
    uint32_t ssrc = ntohl(rtp->ssrc);
	uint32_t timestamp = ntohl(rtp->timestamp);
	uint16_t seq_number = ntohs(rtp->seq_number);
    /* First check if probation period */
    if(dc->probation == AUDIO_MIN_SEQUENTIAL) {
        dc->probation--;
        dc->expected_seq = seq_number + 1;
        JANUS_LOG(LOG_INFO, "Probation started with ssrc = %"SCNu32", seq = %"SCNu16" \n", ssrc, seq_number);
        return;
    } else if(dc->probation != 0) {
        /* Decrease probation */
        dc->probation--;
        if(!dc->probation){
            /* Probation is ended */
            JANUS_LOG(LOG_INFO, "Probation ended with ssrc = %"SCNu32", seq = %"SCNu16" \n",ssrc, seq_number);
        }
        dc->expected_seq = seq_number + 1;
        return;
    }

    int plen = 0;
    const unsigned char *payload = (const unsigned char *)janus_rtp_payload(buf, len, &plen);
    if(!payload) {
        JANUS_LOG(LOG_ERR, "[Opus] Ops! got an error accessing the RTP payload\n");
        return;
    }

    gint length = 0;
    char data[AUDIO_BUFFER_SAMPLES*2];
    memset(data, 0, AUDIO_BUFFER_SAMPLES*2);
    /* Check sequence number received, verify if it's relevant to the expected one */
    if(seq_number == dc->expected_seq) {
        /* Regular decode */
        length = opus_decode(dc->decoder, payload, plen, (opus_int16 *)data, AUDIO_BUFFER_SAMPLES, 0);
        /* Update last_timestamp */
        dc->last_timestamp = timestamp;
        /* Increment according to previous seq_number */
        dc->expected_seq = seq_number + 1;        
    } else if(seq_number > dc->expected_seq) {
        /* Sequence(s) losts */
        uint16_t gap = seq_number - dc->expected_seq;
        JANUS_LOG(LOG_HUGE, "%"SCNu16" sequence(s) lost, sequence = %"SCNu16",  expected seq = %"SCNu16" \n",
            gap, seq_number, dc->expected_seq);

        /* Use FEC if sequence lost < DEFAULT_PREBUFFERING */
        uint16_t start_lost_seq = dc->expected_seq;
        if(dc->fec && gap < AUDIO_DEFAULT_PREBUFFERING) {
            uint8_t i=0;
            for(i=1; i<=gap ; i++) {
                int32_t output_samples;
                uint32_t timestamp_tmp = dc->last_timestamp + (i * AUDIO_OPUS_SAMPLES);
                uint16_t seq_number_tmp = start_lost_seq++;
                length = 0;
                if(i == gap) {
                    /* Attempt to decode with in-band FEC from next packet */
                    opus_decoder_ctl(dc->decoder, OPUS_GET_LAST_PACKET_DURATION(&output_samples));
                    length = opus_decode(dc->decoder, payload, plen, (opus_int16 *)data, output_samples, 1);
                } else {
                    opus_decoder_ctl(dc->decoder, OPUS_GET_LAST_PACKET_DURATION(&output_samples));
                    length = opus_decode(dc->decoder, NULL, plen, (opus_int16 *)data, output_samples, 1);
                }
                if(length < 0) {
                    JANUS_LOG(LOG_ERR, "[Opus] Ops! got an error decoding the Opus frame: %d (%s)\n", length, opus_strerror(length));
                    return;
                }
                JANUS_LOG(LOG_HUGE, "[Opus] decoding  Opus frame len: %d, fec\n", length*dc->channels);
                janus_live_fdkaac_encoder_encode(dc->jb->aencoder, data, length * dc->channels * sizeof(opus_int16), timestamp_tmp/(dc->jb->tb/1000));
            }
        }
        /* Then go with the regular decode (no FEC) */
        length = opus_decode(dc->decoder, payload, plen, (opus_int16 *)data, AUDIO_BUFFER_SAMPLES, 0);
        /* Increment according to previous seq_number */
        dc->expected_seq = seq_number + 1;
    } else {
        /* In late sequence or sequence wrapped */
        if((dc->expected_seq - seq_number) > AUDIO_MAX_MISORDER){
            JANUS_LOG(LOG_HUGE, "SN WRAPPED seq =  %"SCNu16", expected_seq =  %"SCNu16" \n", seq_number, dc->expected_seq);
            dc->expected_seq = seq_number + 1;
        } else {
            JANUS_LOG(LOG_HUGE, "IN LATE SN seq =  %"SCNu16", expected_seq =  %"SCNu16" \n", seq_number, dc->expected_seq);
        }
        return;
    }
    if(length < 0) {
        JANUS_LOG(LOG_ERR, "[Opus] Ops! got an error decoding the Opus frame: %d (%s)\n", length, opus_strerror(length));
        return;
    }
    JANUS_LOG(LOG_VERB, "[Opus] decoding  Opus frame len: %d\n", length*dc->channels);
    janus_live_fdkaac_encoder_encode(dc->jb->aencoder, data, length * dc->channels * sizeof(opus_int16), timestamp/(dc->jb->tb/1000));
}


janus_aencoder_fdkaac *
janus_live_fdkaac_encoder_create(int sample_rate, int channels, int bitrate)
{
    janus_aencoder_fdkaac *ec = g_malloc0(sizeof(janus_aencoder_fdkaac));
    ec->sample_rate = sample_rate;
    ec->channels = channels;
    ec->bitrate = bitrate;

    AVDictionary *audio_opt_p = NULL;
    ec->acodec = avcodec_find_encoder_by_name("libfdk_aac");
    if (ec->acodec == NULL) {
        JANUS_LOG(LOG_ERR, "init audio encoder avcodec_find_encoder_by_name aac error\n");
        return NULL;
    }
    ec->actx = avcodec_alloc_context3(ec->acodec);
    ec->actx->codec_type = AVMEDIA_TYPE_AUDIO;
    ec->actx->sample_rate = ec->sample_rate;
    ec->actx->bit_rate = ec->bitrate * 1000;
    ec->actx->bit_rate_tolerance = ec->bitrate * 1000 * 3 / 2;
    ec->actx->channels = ec->channels;
    ec->actx->channel_layout = AV_CH_LAYOUT_STEREO;

    if (ec->actx->channels == 2) {
        ec->actx->channel_layout = AV_CH_LAYOUT_STEREO;
    }
    if (ec->actx->channels == 6) {
        ec->actx->channel_layout = AV_CH_LAYOUT_5POINT1_BACK;
    }
    if (ec->actx->channels == 8) {
        ec->actx->channel_layout = AV_CH_LAYOUT_7POINT1;
    }
    ec->actx->time_base.num = 1;
    ec->actx->time_base.den = ec->sample_rate;
    ec->actx->sample_fmt = AV_SAMPLE_FMT_S16;

    av_dict_set(&audio_opt_p, "profile", "aac_low", 0);
    av_dict_set(&audio_opt_p, "threads", "1", 0);
    int ret = 0;
    if ((ret = avcodec_open2(ec->actx, ec->acodec, &audio_opt_p)) < 0) {
        av_dict_free(&audio_opt_p);
        JANUS_LOG(LOG_ERR, "init audio encoder open audio encoder failed. ret=%d\n", ret);
        return NULL;
    }
    av_dict_free(&audio_opt_p);

    ec->nb_samples = 1024;
    ec->aframe = av_frame_alloc();
    ec->aframe->nb_samples     = ec->nb_samples;
    ec->aframe->channel_layout = 3;
    ec->aframe->format         = AV_SAMPLE_FMT_S16;

    ret = av_frame_get_buffer(ec->aframe, 32);
    if (ret != 0) {
        JANUS_LOG(LOG_ERR, "init audio frame failed. nb_samples=%d, channel_layout=%d, ret=%d\n",
            ec->nb_samples,ec->aframe->channel_layout, ret);
    }
    JANUS_LOG(LOG_ERR, "init audio frame failed. nb_samples=%d, channels=%d, format=%d, linesize0=%d\n",
        ec->aframe->nb_samples, ec->aframe->channels, ec->aframe->format, ec->aframe->linesize[0]);

    ec->buflen = 0;
    JANUS_LOG(LOG_ERR, "fdkaac frame buffer len:%d\n", ec->nb_samples * ec->aframe->channels * 2);
    ec->buffer = g_malloc0(ec->nb_samples * ec->aframe->channels * 2);

    return ec;
}


void
janus_live_fdkaac_encoder_destory(janus_aencoder_fdkaac *ec)
{
    if (ec->aframe) {
        av_frame_free(&ec->aframe);
        ec->aframe = NULL;
    }
    if(ec->actx) {
        avcodec_close(ec->actx);
        av_free(ec->actx);
        ec->actx = NULL;
        ec->acodec = NULL;
    }
    if(ec->buffer){
        g_free(ec->buffer);
        ec->buffer = NULL;
    }
    g_free(ec);
}


void
janus_live_fdkaac_encoder_encode_inernal(janus_aencoder_fdkaac *ec, char *data, int len, uint32_t pts)
{
    if(!data || !len)
        return;
    gint64 now = janus_get_real_time();
    JANUS_LOG(LOG_HUGE, "fdkaac encode len:%d, pts:%d\n", len, pts);
    if(pts > 0 && ec->jb->lastts > 0 && pts < ec->jb->lastts && ec->jb->lastts - pts > 500){
        JANUS_LOG(LOG_WARN, "fdkaac offset reset, last:%d, now:%d\n", ec->jb->offset, ec->jb->lastts);
        ec->jb->offset = ec->jb->lastts;
    }
    ec->jb->lastts = pts;

    ec->aframe->pts = pts + ec->jb->offset;
    memcpy((unsigned char *)ec->aframe->data[0], data, len);

    int got_pic = 0;
    int ret_enc = 0;
    AVPacket* enc_pkt_p = av_packet_alloc();
    av_init_packet(enc_pkt_p);

    ret_enc = avcodec_encode_audio2(ec->actx, enc_pkt_p, ec->aframe, &got_pic);
    if (ret_enc < 0) {
        JANUS_LOG(LOG_ERR, "audio encode error ret:%d\n", ret_enc);
    }
    if ((ret_enc >= 0) && got_pic) {

        if(enc_pkt_p->pts != enc_pkt_p->dts){
            JANUS_LOG(LOG_WARN, "audio encode erroraudio pts:%d != dts:%d\n", enc_pkt_p->pts, enc_pkt_p->dts);
        }
        if(AV_NOPTS_VALUE == enc_pkt_p->pts || enc_pkt_p->pts < 0){
            JANUS_LOG(LOG_WARN, "audio pts unnormal dts:%d, pts:%d\n", enc_pkt_p->dts, enc_pkt_p->pts);
            enc_pkt_p->pts = 0;
            enc_pkt_p->dts = 0;
        }
        JANUS_LOG(LOG_HUGE, "audio pts  dts:%d, pts:%d src pts:%d\n", enc_pkt_p->dts, enc_pkt_p->pts, pts);

        //janus_frame_packet *p = janus_packet_alloc(enc_pkt_p->size + FF_INPUT_BUFFER_PADDING_SIZE);
        janus_frame_packet *p = janus_packet_alloc(enc_pkt_p->size);
        p->created = now;
        memcpy(p->data, enc_pkt_p->data, enc_pkt_p->size);
        p->video = FALSE;
        p->ts = enc_pkt_p->dts;
        janus_live_packet_insert(ec->jb->pub, p);
    }
    av_packet_free(&enc_pkt_p);
}


void
janus_live_fdkaac_encoder_encode(janus_aencoder_fdkaac *ec, char *data, int len, uint32_t pts)
{
    if(!data || !len)
        return;

    int total = ec->nb_samples * ec->aframe->channels * 2;
    int left = 0;
    if(ec->buflen + len < total){
        memcpy(ec->buffer + ec->buflen, data, len);
        ec->buflen += len;
    } else{
        left = len - (total - ec->buflen);
        memcpy(ec->buffer + ec->buflen, data, total - ec->buflen);
        janus_live_fdkaac_encoder_encode_inernal(ec, ec->buffer, total, pts);
        ec->buflen = 0;

        if(left > 0){
            memcpy(ec->buffer + ec->buflen, &data[len - left], left);
            ec->buflen += left;
        }
    }
    JANUS_LOG(LOG_HUGE, "fdkaac cache buflen:%d, len:%d, pts:%d, left:%d\n", ec->buflen, len, pts, left);
}
