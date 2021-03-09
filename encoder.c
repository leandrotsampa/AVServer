#include <AVServer.h>
#include <unistd.h>
#include <hi_adp_mpi.h>
#include <hi_adp_demux.h>
#include <hi_common.h>
#include <hi_unf_avplay.h>
#include <hi_unf_descrambler.h>
#include <hi_unf_demux.h>
#include <hi_unf_ecs.h>
#include <hi_unf_keyled.h>
#include <hi_unf_sound.h>
#include <hi_unf_vo.h>
#include <hi_unf_aenc.h>
#include <hi_unf_venc.h>
#include <hi_mpi_ao.h>
#include <hi_mpi_avplay.h>
#include <hi_mpi_sync.h>
#include <hi_mpi_vdec.h>
#include <hi_mpi_win.h>
#include <hi_video_codec.h>
#include <buffer.h>
#include <mpeg-ts.h>
#include <mpeg-ts-proto.h>

/* Audio Includes */
#include <HA.AUDIO.MP3.decode.h>
#include <HA.AUDIO.MP2.decode.h>
#include <HA.AUDIO.AAC.decode.h>
#include <HA.AUDIO.DRA.decode.h>
#include <HA.AUDIO.PCM.decode.h>
#include <HA.AUDIO.WMA9STD.decode.h>
#include <HA.AUDIO.TRUEHDPASSTHROUGH.decode.h>
#include <HA.AUDIO.DOLBYTRUEHD.decode.h>
#include <HA.AUDIO.DTSHD.decode.h>
#include <HA.AUDIO.DOLBYPLUS.decode.h>
#include <HA.AUDIO.AC3PASSTHROUGH.decode.h>
#include <HA.AUDIO.DTSM6.decode.h>
#include <HA.AUDIO.DTSPASSTHROUGH.decode.h>
#include <HA.AUDIO.FFMPEG_DECODE.decode.h>
#include "HA.AUDIO.AAC.encode.h"

#define PLAYER_DEMUX_PORT 5

struct s_encoder {
	int  id;
	bool IsStarted;
	int  AudioID;
	int  AudioPid;
	int  AudioType;
	int  VideoID;
	int  VideoPid;
	int  VideoType;
	int  PMTPid;

	unsigned int hPlayer;
	unsigned int hWindow;
	unsigned int hTrack;
	unsigned int hTsBuffer;
	unsigned int hVenc;
	unsigned int hAenc;

	struct buffer    tsBuffer;
	void            *tsEncoder;
	pthread_t        m_process;
	pthread_rwlock_t m_write;
};

static void encoder_read_config(int id, char *property, char *value, size_t size) {
	char path[256];
	FILE *file = NULL;

	memset(value, 0 , size);
	sprintf(path, "/proc/stb/encoder/%d/%s", id, property);
	if ((file = fopen(path, "r"))) {
		fgets(value, size, file);
		fclose(file);
	}
	printf("[CONFIG] %s = %s\n", property, value);
}

static void *encoder_ts_alloc(void *param, size_t bytes) {
	return malloc(bytes);
}

static void encoder_ts_free(void *param, void *packet) {
	free(packet);
}

static void encoder_ts_write(void *param, const void *packet, size_t bytes) {
	struct s_encoder *encoder = (struct s_encoder *)param;

	if (!encoder)
		return;

	pthread_rwlock_wrlock(&encoder->m_write);
	if (!buffer_put_data(&encoder->tsBuffer, packet, bytes))
		printf("[ERROR] %s -> Failed to Write TS Buffer.\n", __FUNCTION__);
	pthread_rwlock_unlock(&encoder->m_write);
}

void *encoder_process(struct encoder_ops *ops) {
	PMT_TB pmt_tb;
	bool IsPMTLoaded;
	uint32_t AudioPts;
	uint32_t VideoPts;
	struct buffer bAudio;
	struct buffer bVideo;
	DMX_DATA_FILTER_S stDataFilter;
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder)
		return NULL;

	memset(&pmt_tb, 0, sizeof(pmt_tb));
	memset(stDataFilter.u8Match,  0x00, DMX_FILTER_MAX_DEPTH * sizeof(HI_U8));
	memset(stDataFilter.u8Mask,   0xff, DMX_FILTER_MAX_DEPTH * sizeof(HI_U8));
	memset(stDataFilter.u8Negate, 0x00, DMX_FILTER_MAX_DEPTH * sizeof(HI_U8));

	stDataFilter.u32TSPID       = encoder->PMTPid;
	stDataFilter.u32TimeOut     = 1000;
	stDataFilter.u16FilterDepth = 1;
	stDataFilter.u8Crcflag      = 0;

	stDataFilter.u8Match[0] = 0x02; /* PMT Table ID */
	stDataFilter.u8Mask[0]  = 0x00;

	stDataFilter.funSectionFunCallback = &SRH_ParsePMT;
	stDataFilter.pSectionStruct        = (HI_U8 *)&pmt_tb;

	AudioPts = HI_INVALID_PTS;
	VideoPts = HI_INVALID_PTS;
	buffer_init(&bAudio, 0);
	buffer_init(&bVideo, 0);

	while (encoder->IsStarted) {
		if (!IsPMTLoaded) {
			if (DMX_SectionStartDataFilter(PLAYER_DEMUX_PORT + encoder->id, &stDataFilter) == HI_SUCCESS) {
				int i;

				for (i = 0; i < pmt_tb.u16VideoNum; i++) {
					if (encoder->VideoPid == pmt_tb.Videoinfo[i].u16VideoPid) {
						HI_UNF_AVPLAY_STOP_OPT_S stStop;
						stStop.u32TimeoutMs = 0;
						stStop.enMode = HI_UNF_AVPLAY_STOP_MODE_BLACK;
						HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stStop);
						ops->set_type(ops, DEV_VIDEO, pmt_tb.Videoinfo[i].u32VideoEncType);
						HI_UNF_AVPLAY_Start(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, NULL);
					}
				}

				for (i = 0; i < pmt_tb.u16AudoNum; i++) {
					if (encoder->AudioPid == pmt_tb.Audioinfo[i].u16AudioPid) {
						HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, NULL);
						ops->set_type(ops, DEV_AUDIO, pmt_tb.Audioinfo[i].u32AudioEncType);
						HI_UNF_AVPLAY_Start(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, NULL);
					}
				}

				IsPMTLoaded = true;
			}
		} else {
			HI_UNF_ES_BUF_S      stAencStream;
			HI_UNF_VENC_STREAM_S stVencStream;

			if (HI_UNF_AENC_AcquireStream(encoder->hAenc, &stAencStream, 0) == HI_SUCCESS) {
				if (stAencStream.u32PtsMs == HI_INVALID_PTS) {
					printf("[WARNING] %s -> Encoded Audio with Invalid PTS Detected.\n", __FUNCTION__);
				} else {
					if (AudioPts != stAencStream.u32PtsMs && buffer_length(&bAudio) > 0) {
						void *audio = malloc(buffer_length(&bAudio));
						if (!audio) {
							printf("[ERROR] %s -> Failed to alloc Audio Buffer.\n", __FUNCTION__);
							continue;
						}
						mpeg_ts_write(encoder->tsEncoder, encoder->AudioID, 0, AudioPts * 90, PTS_NO_VALUE, audio, buffer_pull(&bAudio, audio, buffer_length(&bAudio)));
						free(audio);
					}

					AudioPts = stAencStream.u32PtsMs;
					if (!buffer_put_data(&bAudio, stAencStream.pu8Buf, stAencStream.u32BufLen))
						printf("[ERROR] %s -> Failed to Write TS Buffer.\n", __FUNCTION__);
				}

				HI_UNF_AENC_ReleaseStream(encoder->hAenc, &stAencStream);
			}

			if (HI_UNF_VENC_AcquireStream(encoder->hVenc, &stVencStream, 0) == HI_SUCCESS) {
				if (stVencStream.u32PtsMs == HI_INVALID_PTS) {
					printf("[WARNING] %s -> Encoded Video with Invalid PTS Detected.\n", __FUNCTION__);
				} else {
					if (VideoPts != stVencStream.u32PtsMs && buffer_length(&bVideo) > 0) {
						void *video = malloc(buffer_length(&bVideo));
						if (!video) {
							printf("[ERROR] %s -> Failed to alloc Video Buffer.\n", __FUNCTION__);
							continue;
						}
						mpeg_ts_write(encoder->tsEncoder, encoder->VideoID, MPEG_FLAG_IDR_FRAME, VideoPts * 90, PTS_NO_VALUE, video, buffer_pull(&bVideo, video, buffer_length(&bVideo)));
						free(video);
					}

					VideoPts = stVencStream.u32PtsMs;
					if (!buffer_put_data(&bVideo, stVencStream.pu8Addr, stVencStream.u32SlcLen))
						printf("[ERROR] %s -> Failed to Write TS Buffer.\n", __FUNCTION__);
				}

				HI_UNF_VENC_ReleaseStream(encoder->hVenc, &stVencStream);
			}
		}
		usleep(1000);
	}

	buffer_free(&bAudio);
	buffer_free(&bVideo);
	return NULL;
}

bool encoder_create(struct encoder_ops *ops, int id) {
	printf("[INFO] %s -> %d called.\n", __FUNCTION__, id);

	if (!ops)
		return false;
	else if (ops->priv)
		return true;

	HI_UNF_VCODEC_ATTR_S     stVDecAttr;
	HI_UNF_ACODEC_ATTR_S     stACodecAttr;
	HI_UNF_SYNC_ATTR_S       stSyncAttr;
	HI_UNF_AVPLAY_ATTR_S     stAvplayAttr;
	HI_UNF_AVPLAY_OPEN_OPT_S OpenOpt;
	HI_UNF_AUDIOTRACK_ATTR_S stTrackAttr;
	HI_UNF_WINDOW_ATTR_S     stWinAttr;
	struct s_encoder *encoder = calloc(1, sizeof(struct s_encoder));

	if (HI_UNF_DMX_AttachTSPort(PLAYER_DEMUX_PORT + id, HI_UNF_DMX_PORT_RAM_1 + id) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to Attach TS Port.\n", __FUNCTION__);
		goto FAILED;
	}

	HI_UNF_AVPLAY_GetDefaultConfig(&stAvplayAttr, HI_UNF_AVPLAY_STREAM_TYPE_TS);
	stAvplayAttr.u32DemuxId = PLAYER_DEMUX_PORT + id;
	stAvplayAttr.stStreamAttr.u32AudBufSize = 4 * 1024 * 1024;  // Allocate 4MB
	stAvplayAttr.stStreamAttr.u32VidBufSize = 16 * 1024 * 1024; // Allocate 16MB
	if (HI_UNF_AVPLAY_Create(&stAvplayAttr, &encoder->hPlayer) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_AVPLAY_Create failed.\n", __FUNCTION__);
		goto TSPORT_DETACH;
	}

	if (HI_UNF_DMX_CreateTSBuffer(HI_UNF_DMX_PORT_RAM_1 + id, 0x1000000, &encoder->hTsBuffer) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to create TS buffer.\n", __FUNCTION__);
		goto AVPLAY_DESTROY;
	}

	OpenOpt.enDecType  = HI_UNF_VCODEC_DEC_TYPE_NORMAL;
	OpenOpt.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_4096x2160;
	OpenOpt.enProtocolLevel = HI_UNF_VCODEC_PRTCL_LEVEL_H264;
	if (HI_UNF_AVPLAY_ChnOpen(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &OpenOpt) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to Open Video Channel.\n", __FUNCTION__);
		goto TSBUF_FREE;
	}

	if (HI_UNF_AVPLAY_ChnOpen(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, HI_NULL) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to Open Audio Channel.\n", __FUNCTION__);
		goto VCHN_CLOSE;
	}

	/* create virtual window */
	stWinAttr.enDisp = HI_UNF_DISPLAY0;
	stWinAttr.bVirtual = HI_TRUE;
	stWinAttr.enVideoFormat = HI_UNF_FORMAT_YUV_SEMIPLANAR_420;
	stWinAttr.stWinAspectAttr.bUserDefAspectRatio = HI_FALSE;
	stWinAttr.stWinAspectAttr.enAspectCvrs = HI_UNF_VO_ASPECT_CVRS_IGNORE;
	stWinAttr.bUseCropRect = HI_FALSE;
	memset(&stWinAttr.stInputRect,0,sizeof(HI_RECT_S));
	memset(&stWinAttr.stOutputRect,0,sizeof(HI_RECT_S));
	if (HI_UNF_VO_CreateWindow(&stWinAttr, &encoder->hWindow) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_VO_CreateWindow failed.\n", __FUNCTION__);
		goto ACHN_CLOSE;
	}

	if (HI_UNF_VO_AttachWindow(encoder->hWindow, encoder->hPlayer) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_VO_AttachWindow failed.\n", __FUNCTION__);
		goto VO_DEINIT;
	}

	if (HI_UNF_VO_SetWindowEnable(encoder->hWindow, HI_TRUE) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_VO_SetWindowEnable failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_GetDefaultTrackAttr(HI_UNF_SND_TRACK_TYPE_VIRTUAL, &stTrackAttr) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_SND_GetDefaultTrackAttr failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_CreateTrack(HI_UNF_SND_0, &stTrackAttr, &encoder->hTrack) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_SND_CreateTrack failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_Attach(encoder->hTrack, encoder->hPlayer) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_SND_Attach failed.\n", __FUNCTION__);
		goto TRACK_DESTROY;
	}

	/* Set Audio Codec to AAC */
	HI_UNF_AVPLAY_GetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_ADEC, &stACodecAttr);
	stACodecAttr.enType = HA_AUDIO_ID_AAC;
	HA_AAC_DecGetDefalutOpenParam(&stACodecAttr.stDecodeParam);
	HI_UNF_AVPLAY_SetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_ADEC, &stACodecAttr);

	/* Set Video Codec to MPEG2 */
	HI_UNF_AVPLAY_GetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_VDEC, &stVDecAttr);
	stVDecAttr.enType      = HI_UNF_VCODEC_TYPE_MPEG2;
	stVDecAttr.enMode      = HI_UNF_VCODEC_MODE_NORMAL;
	stVDecAttr.u32ErrCover = 100;
	stVDecAttr.u32Priority = 3;
	HI_UNF_AVPLAY_SetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_VDEC, &stVDecAttr);

	HI_UNF_AVPLAY_GetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr);
	stSyncAttr.enSyncRef = HI_UNF_SYNC_REF_AUDIO;
	if (HI_UNF_AVPLAY_SetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_AVPLAY_SetAttr failed.\n", __FUNCTION__);
		goto SND_DETACH;
	}

	pthread_rwlock_init(&encoder->m_write, 0);
	buffer_init(&encoder->tsBuffer, 0);

	encoder->id          = id;
	encoder->IsStarted   = false;
	encoder->AudioPid    = 0;
	encoder->AudioType   = HA_AUDIO_ID_AAC;
	encoder->VideoPid    = 0;
	encoder->VideoType   = HI_UNF_VCODEC_TYPE_MPEG2;
	encoder->PMTPid      = 0;
	ops->priv            = encoder;
	return true;

SND_DETACH:
	HI_UNF_SND_Detach(encoder->hTrack, encoder->hPlayer);
TRACK_DESTROY:
	HI_UNF_SND_DestroyTrack(encoder->hTrack);
WIN_DETACH:
	HI_UNF_VO_SetWindowEnable(encoder->hWindow, HI_FALSE);
	HI_UNF_VO_DetachWindow(encoder->hWindow, encoder->hPlayer);
VO_DEINIT:
	HI_UNF_VO_DestroyWindow(encoder->hWindow);
ACHN_CLOSE:
	HI_UNF_AVPLAY_ChnClose(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
VCHN_CLOSE:
	HI_UNF_AVPLAY_ChnClose(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);
TSBUF_FREE:
	HI_UNF_DMX_DestroyTSBuffer(encoder->hTsBuffer);
AVPLAY_DESTROY:
	HI_UNF_AVPLAY_Destroy(encoder->hPlayer);
TSPORT_DETACH:
	HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT + id);
FAILED:
	free(encoder);
	return false;
}

void encoder_destroy(struct encoder_ops *ops) {
	printf("[INFO] %s -> called.\n", __FUNCTION__);
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (encoder) {
		HI_UNF_AVPLAY_STOP_OPT_S stStop;
		stStop.u32TimeoutMs = 0;
		stStop.enMode = HI_UNF_AVPLAY_STOP_MODE_BLACK;

		encoder->IsStarted = false;
		pthread_join(encoder->m_process, NULL);

		HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, HI_NULL);
		HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stStop);

		HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT + encoder->id);
		HI_UNF_DMX_DestroyTSBuffer(encoder->hTsBuffer);

		HI_UNF_SND_Detach(encoder->hTrack, encoder->hPlayer);
		HI_UNF_SND_DestroyTrack(encoder->hTrack);

		HI_UNF_VO_SetWindowEnable(encoder->hWindow, HI_FALSE);
		HI_UNF_VO_DetachWindow(encoder->hWindow, encoder->hPlayer);

		if (encoder->hAenc) {
			HI_UNF_AENC_DetachInput(encoder->hAenc);
			HI_UNF_AENC_Destroy(encoder->hAenc);
		}

		if (encoder->hVenc) {
			HI_UNF_VENC_DetachInput(encoder->hVenc);
			HI_UNF_VENC_Destroy(encoder->hVenc);
		}

		HI_UNF_AVPLAY_ChnClose(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
		HI_UNF_AVPLAY_ChnClose(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);

		HI_UNF_AVPLAY_Destroy(encoder->hPlayer);

		HI_UNF_VO_DestroyWindow(encoder->hWindow);

		if (encoder->tsEncoder)
			mpeg_ts_destroy(encoder->tsEncoder);
		buffer_free(&encoder->tsBuffer);
		free(encoder);
	} else {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
	}
}

bool encoder_set_pid(struct encoder_ops *ops, int dev_type, int pid) {
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, pid);
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			if (HI_UNF_AVPLAY_SetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_AUD_PID, &pid) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set a new PID %d for Audio.\n", __FUNCTION__, pid);
				return false;
			}
			encoder->AudioPid = pid;
		break;
		case DEV_VIDEO:
			if (HI_UNF_AVPLAY_SetAttr(encoder->hPlayer, HI_UNF_AVPLAY_ATTR_ID_VID_PID, &pid) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set a new PID %d for Video.\n", __FUNCTION__, pid);
				return false;
			}
			encoder->VideoPid = pid;
		break;
		default:
			encoder->PMTPid = pid;
		break;
	}

	return true;
}

bool encoder_set_type(struct encoder_ops *ops, int dev_type, int type) {
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, type);
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type) {
		case DEV_AUDIO:
			if (encoder->AudioType == type)
				return true;
			else if (HIADP_AVPlay_SetAdecAttr(encoder->hPlayer, type, HD_DEC_MODE_RAWPCM, 1) != HI_SUCCESS) {
				printf("[ERROR] %s: Failed to set Audio Type %d.\n", __FUNCTION__, type);
				return false;
			}

			encoder->AudioType = type;
		break;
		case DEV_VIDEO:
			if (encoder->VideoType == type)
				return true;
			else if (HIADP_AVPlay_SetVdecAttr(encoder->hPlayer, type, HI_UNF_VCODEC_MODE_NORMAL) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to set Video Type %d.\n", __FUNCTION__, type);
				return false;
			}

			encoder->VideoType = type;
		break;
	}

	return true;
}

bool encoder_play(struct encoder_ops *ops) {
	printf("[INFO] %s -> called.\n", __FUNCTION__);
	char                     value[256];
	AAC_ENC_CONFIG           stPConfig;
	HI_UNF_AENC_ATTR_S       stAencAttr;
	HI_UNF_VENC_CHN_ATTR_S   stVencAttr;
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_AVPLAY_Start(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, NULL) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to play Audio.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_AVPLAY_Start(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, NULL) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to play Video.\n", __FUNCTION__);
		return false;
	}

	HI_UNF_VENC_Init();
	HI_UNF_VENC_GetDefaultAttr(&stVencAttr);

	stVencAttr.enVencType = HI_UNF_VCODEC_TYPE_H264;

	encoder_read_config(encoder->id, "profile", value, sizeof(value));
	if (startsWith(value, "main"))
		stVencAttr.enVencProfile = HI_UNF_H264_PROFILE_MAIN;
	else if (startsWith(value, "high"))
		stVencAttr.enVencProfile = HI_UNF_H264_PROFILE_HIGH;
	else
		stVencAttr.enVencProfile = HI_UNF_H264_PROFILE_BASELINE;

	encoder_read_config(encoder->id, "bitrate", value, sizeof(value));
	stVencAttr.u32TargetBitRate = strtoul(value, NULL, 10);

	encoder_read_config(encoder->id, "framerate", value, sizeof(value));
	stVencAttr.u32TargetFrmRate = strtoul(value, NULL, 10);
	if (stVencAttr.u32TargetFrmRate > 1000)
		stVencAttr.u32TargetFrmRate /= 1000;
	stVencAttr.u32InputFrmRate  = stVencAttr.u32TargetFrmRate;
	if (!(stVencAttr.u32TargetFrmRate > 0 && stVencAttr.u32TargetFrmRate <= 60)) {
		stVencAttr.u32TargetFrmRate = 25;
		stVencAttr.u32InputFrmRate  = 25;
	}

	encoder_read_config(encoder->id, "display_format", value, sizeof(value));
	if (startsWith(value, "1080p")) {
		stVencAttr.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_FULLHD;
		stVencAttr.u32Width   = 1920;
		stVencAttr.u32Height  = 1080;
		if (!stVencAttr.u32TargetBitRate)
			stVencAttr.u32TargetBitRate = 3 * 1024 * 1024;
	} else if (startsWith(value, "720p")) {
		stVencAttr.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_720P;
		stVencAttr.u32Width   = 1280;
		stVencAttr.u32Height  = 720;
		if (!stVencAttr.u32TargetBitRate)
			stVencAttr.u32TargetBitRate = 3 * 1024 * 1024;
	} else if (startsWith(value, "576p")) {
		stVencAttr.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_D1;
		stVencAttr.u32Width   = 720;
		stVencAttr.u32Height  = 576;
		if (!stVencAttr.u32TargetBitRate)
			stVencAttr.u32TargetBitRate = 3 / 2 * 1024 * 1024;
	} else if (startsWith(value, "480p")) {
		stVencAttr.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_D1;
		stVencAttr.u32Width   = 640;
		stVencAttr.u32Height  = 480;
		if (!stVencAttr.u32TargetBitRate)
			stVencAttr.u32TargetBitRate = 800 * 1024;
	} else {
		stVencAttr.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_720P;
		stVencAttr.u32Width = 1280;
		stVencAttr.u32Height = 720;
		stVencAttr.u32TargetBitRate = 3 * 1024 * 1024;
		stVencAttr.u32TargetFrmRate = 25;
		stVencAttr.u32InputFrmRate  = 25;
	}

	stVencAttr.bQuickEncode = HI_TRUE;
	stVencAttr.u32StrmBufSize = stVencAttr.u32Width * stVencAttr.u32Height * 2;
	if (stVencAttr.u32StrmBufSize < 829440)
		stVencAttr.u32StrmBufSize = 829440;
	else if (stVencAttr.u32StrmBufSize > 20971520)
		stVencAttr.u32StrmBufSize = 20971520;

	if (HI_UNF_VENC_Create(&encoder->hVenc, &stVencAttr) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_VENC_Create failed.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_VENC_AttachInput(encoder->hVenc, encoder->hWindow) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_VENC_AttachInput failed.\n", __FUNCTION__);
		HI_UNF_VENC_Destroy(encoder->hVenc);
		return false;
	}

	HI_UNF_AENC_Init();
	HI_UNF_AENC_RegisterEncoder("libHA.AUDIO.AAC.encode.so");

	stAencAttr.enAencType = HA_AUDIO_ID_AAC;
	HA_AAC_GetDefaultConfig(&stPConfig);
	HA_AAC_GetEncDefaultOpenParam(&(stAencAttr.sOpenParam), (HI_VOID *)&stPConfig);

	if (HI_UNF_AENC_Create(&stAencAttr, &encoder->hAenc) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_AENC_Create failed.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_AENC_AttachInput(encoder->hAenc, encoder->hTrack) != HI_SUCCESS) {
		printf("[ERROR] %s -> HI_UNF_AENC_AttachInput failed.\n", __FUNCTION__);
		HI_UNF_AENC_Destroy(encoder->hAenc);
		return false;
	}

	struct mpeg_ts_func_t tsHandler;
	tsHandler.alloc = encoder_ts_alloc;
	tsHandler.write = encoder_ts_write;
	tsHandler.free  = encoder_ts_free;
	if (!(encoder->tsEncoder = mpeg_ts_create(&tsHandler, encoder)))
		return false;
	encoder->VideoID = mpeg_ts_add_stream(encoder->tsEncoder, PSI_STREAM_H264, NULL, 0);
	encoder->AudioID = mpeg_ts_add_stream(encoder->tsEncoder, PSI_STREAM_AAC,  NULL, 0);

	encoder->IsStarted = true;
	HI_UNF_DMX_ResetTSBuffer(encoder->hTsBuffer);
	pthread_create(&encoder->m_process, NULL, (void *)&encoder_process, ops);
	return true;
}

bool encoder_stop(struct encoder_ops *ops) {
	printf("[INFO] %s -> called.\n", __FUNCTION__);
	HI_UNF_AVPLAY_STOP_OPT_S stStop;
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	stStop.u32TimeoutMs = 0;
	stStop.enMode = HI_UNF_AVPLAY_STOP_MODE_BLACK;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, NULL) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to play Audio.\n", __FUNCTION__);
		return false;
	} else if (HI_UNF_AVPLAY_Stop(encoder->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stStop) != HI_SUCCESS) {
		printf("[ERROR] %s -> Failed to play Video.\n", __FUNCTION__);
		return false;
	}

	encoder->IsStarted = false;
	return true;
}

int encoder_read(struct encoder_ops *ops, char *buf, size_t size) {
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return -EINVAL;
	} else if (!encoder->IsStarted) {
		return -EINVAL;
	}

	pthread_rwlock_wrlock(&encoder->m_write);
	size_t rSize = buffer_pull(&encoder->tsBuffer, buf, size);
	pthread_rwlock_unlock(&encoder->m_write);
	return rSize;
}

int encoder_write(struct encoder_ops *ops, const char *buf, size_t size) {
	HI_UNF_STREAM_BUF_S sBuf;
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return -EINVAL;
	} else if (!encoder->IsStarted) {
		return -EINVAL;
	}

	if (HI_UNF_DMX_GetTSBuffer(encoder->hTsBuffer, size, &sBuf, 1000) == HI_SUCCESS) {
		memcpy(sBuf.pu8Data, buf, size);
		if (HI_UNF_DMX_PutTSBuffer(encoder->hTsBuffer, size) != HI_SUCCESS) {
			printf("[ERROR] %s -> Failed to put buffer. (ID: %d)\n", __FUNCTION__, encoder->id);
			return -EAGAIN;
		}
	} else {
		printf("[ERROR] %s -> Failed to get buffer. (ID: %d)\n", __FUNCTION__, encoder->id);
		return -EAGAIN;
	}

	return size;
}

int encoder_poll(struct encoder_ops *ops, struct fuse_pollhandle *ph, unsigned *reventsp, bool condition) {
	HI_UNF_DMX_TSBUF_STATUS_S pStatus;
	struct s_encoder *encoder = (struct s_encoder *)ops->priv;

	if (!encoder) {
		printf("[ERROR] %s -> The Encoder it's not created.\n", __FUNCTION__);
		return -EINVAL;
	}

	*reventsp = 0;
	pthread_rwlock_rdlock(&encoder->m_write);
	if (HI_UNF_DMX_GetTSBufferStatus(encoder->hTsBuffer, &pStatus) == HI_SUCCESS)
		if ((pStatus.u32UsedSize * 100 / pStatus.u32BufSize) < 85)
			*reventsp |= POLLOUT; /* Request Write */

	if (buffer_length(&encoder->tsBuffer) > 0)
		*reventsp |= POLLIN; /* Request Read */
	pthread_rwlock_unlock(&encoder->m_write);

	fuse_pollhandle_destroy(ph);
	return 0;
}

struct encoder_ops *get_encoder(void) {
	struct encoder_ops *encoder = calloc(1, sizeof(struct encoder_ops));
	encoder->create  = encoder_create;
	encoder->destroy = encoder_destroy;
	encoder->set_pid = encoder_set_pid;
	encoder->set_type= encoder_set_type;
	encoder->play    = encoder_play;
	encoder->stop    = encoder_stop;
	encoder->read	 = encoder_read;
	encoder->write   = encoder_write;
	encoder->poll    = encoder_poll;
	return encoder;
}