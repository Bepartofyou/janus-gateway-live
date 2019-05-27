#ifndef _JANUS_LIVE_H
#define _JANUS_LIVE_H

#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <opus/opus.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "mutex.h"
#include "refcount.h"

#define LIBAVCODEC_VER_AT_LEAST(major, minor) \
		(LIBAVCODEC_VERSION_MAJOR > major || \
	 	(LIBAVCODEC_VERSION_MAJOR == major && \
	  	LIBAVCODEC_VERSION_MINOR >= minor))

#if LIBAVCODEC_VER_AT_LEAST(51, 42)
	#define PIX_FMT_YUV420P AV_PIX_FMT_YUV420P
#endif

#if LIBAVCODEC_VER_AT_LEAST(56, 56)
	#ifndef CODEC_FLAG_GLOBAL_HEADER
	#define CODEC_FLAG_GLOBAL_HEADER AV_CODEC_FLAG_GLOBAL_HEADER
	#endif
	#ifndef FF_INPUT_BUFFER_PADDING_SIZE
	#define FF_INPUT_BUFFER_PADDING_SIZE AV_INPUT_BUFFER_PADDING_SIZE
	#endif
#endif

#if LIBAVCODEC_VER_AT_LEAST(57, 14)
	#define USE_CODECPAR
#endif

#define AUDIO_MIN_SEQUENTIAL 		2
#define AUDIO_MAX_MISORDER			50
/* Mixer settings */
#define AUDIO_DEFAULT_PREBUFFERING	6
/* Opus settings */
#define	AUDIO_BUFFER_SAMPLES		8000
#define	AUDIO_OPUS_SAMPLES			960
#define AUDIO_DEFAULT_COMPLEXITY	4

#define JANUS_LIVE_BUFFER_MAX  2 * 1024 * 1024
#define htonll(x) ((1==htonl(1)) ? (x) : ((gint64)htonl((x) & 0xFFFFFFFF) << 32) | htonl((x) >> 32))
#define ntohll(x) ((1==ntohl(1)) ? (x) : ((gint64)ntohl((x) & 0xFFFFFFFF) << 32) | ntohl((x) >> 32))

typedef struct janus_rtp_jb			janus_rtp_jb;
typedef struct janus_live_pub 		janus_live_pub;
typedef struct janus_live_el 		janus_live_el;


typedef struct janus_adecoder_opus {
	uint8_t 			channels;
	uint32_t 			sampling_rate;		/* Sampling rate (e.g., 16000 for wideband; can be 8, 12, 16, 24 or 48kHz) */
	OpusDecoder 		*decoder;			/* Opus decoder instance */

	gboolean 			fec;				/* Opus FEC status */
	uint16_t 			expected_seq;		/* Expected sequence number */
	uint16_t 			probation; 			/* Used to determine new ssrc validity */
	uint32_t 			last_timestamp;		/* Last in seq timestamp */
	
	janus_rtp_jb		*jb;
} janus_adecoder_opus;


typedef struct janus_aencoder_fdkaac {
	int 				sample_rate;
	int 				channels;
	int 				bitrate;
	AVFrame				*aframe;
	AVPacket			*apacket;
	AVCodec				*acodec;
    AVCodecContext		*actx;

	int					nb_samples;
	int					buflen;
	char				*buffer;
	janus_rtp_jb		*jb;
} janus_aencoder_fdkaac;


typedef struct janus_frame_packet {
	char 							*data;			/* data */
	uint16_t 						len;			/* Length of the data */
	uint16_t 						seq;			/* RTP Sequence number */
	uint64_t 						ts;				/* RTP Timestamp */
	
	gboolean 						video;
	uint32_t 						ssrc;
	gint64 							created;
	int 							keyFrame;

	int 							pt;				/* Payload type of the data */
	int 							skip;			/* Bytes to skip, besides the RTP header */
	int 							audiolevel;		/* Value of audio level in RTP extension, if parsed */
	int 							rotation;		/* Value of rotation in RTP extension, if parsed */
	uint8_t 						drop;			/* Whether this packet can be dropped (e.g., padding)*/
	struct janus_frame_packet 		*next;
	struct janus_frame_packet 		*prev;
} janus_frame_packet;


typedef struct janus_rtp_jb {
	uint32_t 						last_ts;
	uint32_t 						reset;
	uint32_t 						ssrc;
	uint16_t 						last_seq;
	uint16_t 						last_seq_out;
	int 							times_resetted;
	int			 					post_reset_pkts;

	uint32_t 						tb;
	uint64_t 						start_ts;
	gint64 							start_sys;

	gboolean 						keyframe_found;
	int 							keyFrame;
	int 							frameLen;
	int								buflen;
	uint8_t 						*received_frame;
	uint64_t 						ts;
	janus_live_pub 					*pub;
	janus_adecoder_opus				*adecoder;
	janus_aencoder_fdkaac			*aencoder;
	uint32_t						lastts;
	uint32_t						offset;
	
	uint32_t 						size;
	janus_frame_packet 				*list;
	janus_frame_packet 				*last;
} janus_rtp_jb;


typedef struct janus_live_pub {
	char 							*url;
	char 							*acodec;
	char 							*vcodec;
	gint64 							created;
	volatile gboolean				closed;

	janus_rtp_jb 					*audio_jb;
	janus_rtp_jb 					*video_jb;
    
	GSource 						*jb_src;
	GSource 						*pub_src;
	janus_live_el 					*jb_loop;
	janus_live_el 					*pub_loop;

	int 							audio_level_extmap_id;
	int 							video_orient_extmap_id;
	
	uint32_t 						size;
	janus_frame_packet 				*list;
	janus_frame_packet 				*last;
	uint32_t 						start_ts;
	gint64 							start_sys;

	int 							max_width;
	int 							max_height;
	gboolean 						init_flag;
	AVFormatContext 				*fctx;
	AVStream 						*vStream;
	AVStream 						*aStream;
#ifdef USE_CODECPAR
	AVCodecContext 					*vEncoder;
	AVCodecContext 					*aEncoder;
#endif
	AVBitStreamFilterContext		*aacbsf;
	uint32_t 						lastts;

	janus_mutex 					mutex;
	janus_mutex 					mutex_live;
	volatile gint 					destroyed;
	janus_refcount 					ref;
} janus_live_pub;


typedef struct janus_live_el {
	int 							id;
	char 							*name;
	GThread 						*thread;
	GMainLoop 						*mainloop;
	GMainContext 					*mainctx;
	janus_live_pub 					*pub;
} janus_live_el;


janus_live_pub *janus_live_pub_create(const char *url, const char *acodec, const char *vcodec);
int janus_live_pub_save_frame(janus_live_pub *pub, char *buffer, uint length, gboolean video, int slot);
int janus_live_pub_close(janus_live_pub *pub);
void janus_live_pub_destroy(janus_live_pub *pub);


#endif /* _JANUS_LIVE_H */