#include <AVServer.h>
#include <dvb_filter.h>
#include <ring_buf.h>
#include <unistd.h>
#include <hi_adp_mpi.h>
#include <hi_common.h>
#include <hi_unf_avplay.h>
#include <hi_unf_descrambler.h>
#include <hi_unf_demux.h>
#include <hi_unf_ecs.h>
#include <hi_unf_keyled.h>
#include <hi_unf_sound.h>
#include <hi_unf_vo.h>
#include <hi_mpi_ao.h>
#include <hi_mpi_avplay.h>
#include <hi_mpi_sync.h>
#include <hi_mpi_vdec.h>
#include <hi_mpi_win.h>
#include <hi_video_codec.h>

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

#define AUDIO_STREAMTYPE_AC3	 0
#define AUDIO_STREAMTYPE_MPEG	 1
#define AUDIO_STREAMTYPE_DTS	 2
#define AUDIO_STREAMTYPE_LPCM	 6
#define AUDIO_STREAMTYPE_AAC	 8
#define AUDIO_STREAMTYPE_AACHE	 9
#define AUDIO_STREAMTYPE_MP3	 10
#define AUDIO_STREAMTYPE_AACPLUS 11
#define AUDIO_STREAMTYPE_DTSHD	 16
#define AUDIO_STREAMTYPE_DDP	 34
#define AUDIO_STREAMTYPE_RAW	 48

#define VIDEO_STREAMTYPE_MPEG2		 0
#define VIDEO_STREAMTYPE_MPEG4_H264	 1
#define VIDEO_STREAMTYPE_H263		 2
#define VIDEO_STREAMTYPE_VC1		 3
#define VIDEO_STREAMTYPE_MPEG4_Part2 4
#define VIDEO_STREAMTYPE_VC1_SM		 5
#define VIDEO_STREAMTYPE_MPEG1		 6
#define VIDEO_STREAMTYPE_H265_HEVC	 7
#define VIDEO_STREAMTYPE_XVID		 10
#define VIDEO_STREAMTYPE_DIVX311	 13
#define VIDEO_STREAMTYPE_DIVX4		 14
#define VIDEO_STREAMTYPE_DIVX5		 15

#define MAX_ADAPTER	4
#define PLAYER_DEMUX_PORT 4

struct class_ops player_ops;

struct s_player {
	bool IsCreated;
	int PlayerMode;		/* 0 demux, 1 memory */
	int AudioPid;		/* unknown pid */
	long long AudioPts;
	int AudioType;
	int AudioChannel;	/* 0 stereo, 1 left, 2 right */
	int AudioState;		/* 0 stoped, 1 playing, 2 paused */
	int VideoPid;		/* unknown pid */
	long long VideoPts;
	int VideoType;
	int VideoState;		/* 0 stoped, 1 playing, 2 freezed */
	int VideoFormat;	/* 0 4:3, 1 16:9, 2 2.21:1 */
	int DisplayFormat;	/* 0 Pan&Scan, 1 Letterbox, 2 Center Cut Out */

	bool IsPES;
	bool IsBlank;
	bool IsStopThread;
	bool IsSyncEnabled;
	bool IsMute;

	int AudioBufferState;
	int VideoBufferState;
	unsigned poll_status;
	struct fuse_pollhandle *poll_handle[2];

	unsigned event_status;
	struct {
		bool active;

		int type;
		int width;
		int heigth;
		int aspect;
		unsigned int framerate;
		bool progressive;
	} events[3];

	pthread_mutex_t m_apts;
	pthread_mutex_t m_vpts;
	pthread_mutex_t m_event;
	pthread_mutex_t m_poll;
	pthread_mutex_t m_write;
	pthread_cond_t m_condition;

	pthread_t m_thread;
	ring_buffer *m_buffer;
	struct dvb_filter_pes2ts p2t[2];

	unsigned int hPlayer;
	unsigned int hWindow;
	unsigned int hTrack;
	unsigned int hTsBuffer;
	unsigned int hVdec;
	unsigned int hSync;
};

int dvb_filter_pes2ts_cb(void *priv, unsigned char *data)
{
	struct s_player *player = (struct s_player *)priv;

	if (!write_to_buf_timeout(player->m_buffer, (char *)data, TS_SIZE, 10))
		printf("[ERROR] %s: Failed to write in TS buffer.\n", __FUNCTION__);

	return 0;
}

void *p2t_thread(void *data)
{
	pthread_mutex_t lock;
	struct s_player *player = (struct s_player *)player_ops.priv;

	pthread_mutex_init(&lock, NULL);

	while(!player->IsStopThread)
	{
		bool HasBuffer = false;
		bool NeedBuffer = false;

		if (player->PlayerMode != 1)
		{
			pthread_cond_wait(&player->m_condition, &lock);
			continue;
		}

		pthread_mutex_lock(&player->m_poll);
		NeedBuffer = ((player->AudioBufferState != HI_UNF_AVPLAY_BUF_STATE_FULL) ||
					 (player->VideoBufferState <= HI_UNF_AVPLAY_BUF_STATE_NORMAL ||
					  player->VideoBufferState == HI_UNF_AVPLAY_BUF_STATE_BUTT));
		pthread_mutex_unlock(&player->m_poll);

		HasBuffer = (get_max_read_size(player->m_buffer) > 0);

		if (NeedBuffer && HasBuffer)
		{
			size_t free;
			size_t size;
			HI_UNF_STREAM_BUF_S sBuf;
			HI_UNF_DMX_TSBUF_STATUS_S pStatus;

			if (HI_UNF_DMX_GetTSBufferStatus(player->hTsBuffer, &pStatus) != HI_SUCCESS)
			{
				usleep(1000);
				continue;
			}

			free = (int)((pStatus.u32BufSize - pStatus.u32UsedSize) / 188);
			if (free <= 0)
				continue;

			size = get_max_read_size(player->m_buffer);
			if (size > free * TS_SIZE)
				size = free * TS_SIZE;

			pthread_mutex_lock(&player->m_write);
			if (HI_UNF_DMX_GetTSBuffer(player->hTsBuffer, size, &sBuf, 1000) == HI_SUCCESS)
			{
				if (!read_buf(player->m_buffer, (void *)sBuf.pu8Data, size))
				{
					printf("[ERROR] %s: Failed to read buffer.\n", __FUNCTION__);
					HI_UNF_DMX_PutTSBuffer(player->hTsBuffer, 0);
					pthread_mutex_unlock(&player->m_write);
					continue;
				}

				if (HI_UNF_DMX_PutTSBuffer(player->hTsBuffer, size) != HI_SUCCESS)
					printf("[ERROR] %s: Failed to put buffer.\n", __FUNCTION__);
			}
			pthread_mutex_unlock(&player->m_write);
		}
		else
			usleep(1000 * 10);
	}

	return NULL;
}

void player_showtime(void)
{
	HI_UNF_KEYLED_TIME_S stTime;
	time_t _tm = time(NULL);
	struct tm *curtime = localtime(&_tm);

	stTime.u32Hour = curtime->tm_hour;
	stTime.u32Minute = curtime->tm_min;

	if (HI_UNF_LED_DisplayTime(stTime) != HI_SUCCESS)
		printf("[ERROR] %s: Time not writed to keyled.\n", __FUNCTION__);
}

void player_create_painel(void)
{
	if (HI_UNF_KEYLED_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to create painel.\n", __FUNCTION__);
		return;
	}

	int ret = HI_UNF_KEYLED_SelectType(HI_UNF_KEYLED_TYPE_FD650);
	ret |= HI_UNF_LED_Open();
	ret |= HI_UNF_LED_SetFlashFreq(HI_UNF_KEYLED_LEVEL_1);
	ret |= HI_UNF_LED_SetFlashPin(HI_UNF_KEYLED_LIGHT_ALL);

	if (ret == HI_SUCCESS)
		player_showtime();
}

void player_set_keyhandler(HI_HANDLE hPChannel, int pid)
{
	int i;
	HI_HANDLE hPKey;
	HI_UNF_DMX_GetDescramblerKeyHandle(hPChannel, &hPKey);

	for (i = 0; i < MAX_ADAPTER; i++)
	{
		HI_HANDLE hChannel;

		if (HI_UNF_DMX_GetChannelHandle(i, (HI_U32)pid, &hChannel) == HI_SUCCESS)
		{
			HI_HANDLE hKey;

			if (HI_UNF_DMX_GetDescramblerKeyHandle(hChannel, &hKey) == HI_SUCCESS)
			{
				if (hPKey && hPKey != hKey)
					HI_UNF_DMX_DetachDescrambler(hPKey, hPChannel);

				if (HI_UNF_DMX_AttachDescrambler(hKey, hPChannel) != HI_SUCCESS)
					printf("[ERROR] %s -> Failed to attach KeyHandle to PID %d.\n", __FUNCTION__, pid);
			}

			break;
		}
	}
}

int player_event_handler(HI_HANDLE handle, HI_UNF_AVPLAY_EVENT_E enEvent, HI_VOID *para)
{
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (enEvent == HI_UNF_AVPLAY_EVENT_NEW_VID_FRAME)
	{
		int width;
		int heigth;
		int aspect;
		int framerate;
		bool progressive;
		HI_UNF_VIDEO_FRAME_INFO_S *vFrame = (HI_UNF_VIDEO_FRAME_INFO_S *)para;

		/** VIDEO_EVENT_SIZE_CHANGED **/
		width  = vFrame->u32Width;
		heigth = vFrame->u32Height;

		if (vFrame->u32AspectWidth == 4 && vFrame->u32AspectHeight == 3)
			aspect = VIDEO_FORMAT_4_3;
		else if (vFrame->u32AspectWidth == 221 && vFrame->u32AspectHeight == 1)
			aspect = VIDEO_FORMAT_221_1;
		else /* if (vFrame->u32AspectWidth == 16 && vFrame->u32AspectHeight == 9) */
			aspect = VIDEO_FORMAT_16_9;

		/** VIDEO_EVENT_FRAME_RATE_CHANGED **/
		framerate = (vFrame->stFrameRate.u32fpsInteger * 1000) - 500;

		/** VIDEO_EVENT_PROGRESSIVE_CHANGED **/
		progressive = vFrame->bProgressive == HI_FALSE ? true : false;

		/** Check changes. **/
		/** VIDEO_EVENT_SIZE_CHANGED **/
		if (!player->events[0].active ||
			player->events[0].width != width ||
			player->events[0].heigth != heigth)
			{
				pthread_mutex_lock(&player->m_event);
				player->events[0].active = true;
				player->events[0].width  = width;
				player->events[0].heigth = heigth;
				player->events[0].aspect = player->VideoFormat = aspect;

				player->event_status |= (1 << player->events[0].type);
				pthread_mutex_unlock(&player->m_event);
			}

		/** VIDEO_EVENT_FRAME_RATE_CHANGED **/
		if (!player->events[1].active ||
			player->events[1].framerate != framerate)
			{
				pthread_mutex_lock(&player->m_event);
				player->events[1].active = true;
				player->events[1].framerate = framerate;

				player->event_status |= (1 << player->events[1].type);
				pthread_mutex_unlock(&player->m_event);
			}

		/** VIDEO_EVENT_PROGRESSIVE_CHANGED **/
		if (!player->events[2].active ||
			player->events[2].progressive != progressive)
			{
				pthread_mutex_lock(&player->m_event);
				player->events[2].active = true;
				player->events[2].progressive = progressive;

				player->event_status |= (1 << player->events[2].type);
				pthread_mutex_unlock(&player->m_event);
			}

		/** Set PTS **/
		pthread_mutex_lock(&player->m_vpts);
		player->VideoPts = vFrame->u32Pts * 90;
		pthread_mutex_unlock(&player->m_vpts);
	}
	else if (enEvent == HI_UNF_AVPLAY_EVENT_NEW_AUD_FRAME)
	{
		HI_UNF_AO_FRAMEINFO_S *aFrame = (HI_UNF_AO_FRAMEINFO_S *)para;

		/** Set PTS **/
		pthread_mutex_lock(&player->m_apts);
		player->AudioPts = aFrame->u32PtsMs * 90;
		pthread_mutex_unlock(&player->m_apts);
	}
	else if (player->IsPES)
	{
		pthread_mutex_lock(&player->m_poll);
		switch (enEvent)
		{
			case HI_UNF_AVPLAY_EVENT_AUD_BUF_STATE:
				switch ((HI_UNF_AVPLAY_BUF_STATE_E)para)
				{
					case HI_UNF_AVPLAY_BUF_STATE_EMPTY:
					case HI_UNF_AVPLAY_BUF_STATE_LOW:
					case HI_UNF_AVPLAY_BUF_STATE_NORMAL:
					case HI_UNF_AVPLAY_BUF_STATE_HIGH:
						if (player->poll_status & (1 << DEV_AUDIO))
						{
							struct fuse_pollhandle *ph = player->poll_handle[DEV_AUDIO];
							printf("[INFO] %s: Notify poll type Audio Poll status %d.\n", __FUNCTION__, (HI_UNF_AVPLAY_BUF_STATE_E)para);
							fuse_notify_poll(ph);
							fuse_pollhandle_destroy(ph);
							player->poll_handle[DEV_AUDIO] = NULL;
							player->poll_status &= ~(1 << DEV_AUDIO);
						}
					break;
					default:
					break;
				}

				player->AudioBufferState = (HI_UNF_AVPLAY_BUF_STATE_E)para;
			break;
			case HI_UNF_AVPLAY_EVENT_VID_BUF_STATE:
				switch ((HI_UNF_AVPLAY_BUF_STATE_E)para)
				{
					case HI_UNF_AVPLAY_BUF_STATE_EMPTY:
					case HI_UNF_AVPLAY_BUF_STATE_LOW:
					case HI_UNF_AVPLAY_BUF_STATE_NORMAL:
					//case HI_UNF_AVPLAY_BUF_STATE_HIGH: /* Adicionar para teste no futuro. */
						if (player->poll_status & (1 << DEV_VIDEO))
						{
							struct fuse_pollhandle *ph = player->poll_handle[DEV_VIDEO];
							printf("[INFO] %s: Notify poll type Video Poll status %d.\n", __FUNCTION__, (HI_UNF_AVPLAY_BUF_STATE_E)para);
							fuse_notify_poll(ph);
							fuse_pollhandle_destroy(ph);
							player->poll_handle[DEV_VIDEO] = NULL;
							player->poll_status &= ~(1 << DEV_VIDEO);
						}
					break;
					default:
					break;
				}

				player->VideoBufferState = (HI_UNF_AVPLAY_BUF_STATE_E)para;
			break;
			default:
			break;
		}
		pthread_mutex_unlock(&player->m_poll);
	}

    return HI_SUCCESS;
}

bool player_create(void)
{
	printf("[INFO] %s -> called.\n", __FUNCTION__);
	HI_UNF_SYNC_ATTR_S       stSyncAttr;
	HI_UNF_AVPLAY_ATTR_S     stAvplayAttr;
	HI_UNF_AVPLAY_OPEN_OPT_S OpenOpt;
	HI_UNF_AUDIOTRACK_ATTR_S stTrackAttr;
	struct s_player *player = calloc(1, sizeof(struct s_player));

	if (!player)
		return false;

	if (HI_SYS_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_SYS_Init failed.\n", __FUNCTION__);
		return false;
	}

	if (HIADP_Snd_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HIADP_Snd_Init failed.\n", __FUNCTION__);
		goto SYS_DEINIT;
	}

	if (HIADP_Disp_Init(HI_UNF_ENC_FMT_720P_50) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HIADP_Disp_Init failed.\n", __FUNCTION__);
		goto SND_DEINIT;
	}

	if (HIADP_VO_Init(HI_UNF_VO_DEV_MODE_NORMAL) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HIADP_VO_Init failed.\n", __FUNCTION__);
		goto DISP_DEINIT;
	}

	if (HIADP_VO_CreatWin(HI_NULL, &player->hWindow) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HIADP_VO_CreatWin failed.\n", __FUNCTION__);
		goto VO_DEINIT;
	}

	if (HI_UNF_DMX_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_DMX_Init failed.\n", __FUNCTION__);
		goto VO_DEINIT;
	}

	if (HI_UNF_DMX_AttachTSPort(PLAYER_DEMUX_PORT, HI_UNF_DMX_PORT_TSI_0) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> Failed to Attach TS Port.\n", __FUNCTION__);
		goto DMX_DEINIT;
	}

	if (HIADP_AVPlay_RegADecLib() != HI_SUCCESS)
		printf("[ERROR] %s -> HIADP_AVPlay_RegADecLib failed.\n", __FUNCTION__);

	if (HI_UNF_AVPLAY_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_AVPLAY_Init failed.\n", __FUNCTION__);
		goto DMX_DEINIT;
	}

	HI_UNF_AVPLAY_GetDefaultConfig(&stAvplayAttr, HI_UNF_AVPLAY_STREAM_TYPE_TS);
	stAvplayAttr.u32DemuxId = PLAYER_DEMUX_PORT;
	stAvplayAttr.stStreamAttr.u32AudBufSize = 4 * 1024 * 1024; // Allocate 4MB
	stAvplayAttr.stStreamAttr.u32VidBufSize = 9 * 1024 * 1024; // Allocate 9MB
	if (HI_UNF_AVPLAY_Create(&stAvplayAttr, &player->hPlayer) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_AVPLAY_Create failed.\n", __FUNCTION__);
		goto AVPLAY_DEINIT;
	}

	OpenOpt.enDecType  = HI_UNF_VCODEC_DEC_TYPE_NORMAL;
	OpenOpt.enCapLevel = HI_UNF_VCODEC_CAP_LEVEL_FULLHD;
	OpenOpt.enProtocolLevel = HI_UNF_VCODEC_PRTCL_LEVEL_H264;
	if (HI_UNF_AVPLAY_ChnOpen(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &OpenOpt) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> Failed to Open Video Channel.\n", __FUNCTION__);
		goto AVPLAY_DESTROY;
	}

	if (HI_UNF_AVPLAY_ChnOpen(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, HI_NULL) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> Failed to Open Audio Channel.\n", __FUNCTION__);
		goto VCHN_CLOSE;
	}

	if (HI_UNF_VO_AttachWindow(player->hWindow, player->hPlayer) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_VO_AttachWindow failed.\n", __FUNCTION__);
		goto ACHN_CLOSE;
	}

	if (HI_UNF_VO_SetWindowEnable(player->hWindow, HI_TRUE) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_VO_SetWindowEnable failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_GetDefaultTrackAttr(HI_UNF_SND_TRACK_TYPE_MASTER, &stTrackAttr) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_SND_GetDefaultTrackAttr failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_CreateTrack(HI_UNF_SND_0, &stTrackAttr, &player->hTrack) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_SND_CreateTrack failed.\n", __FUNCTION__);
		goto WIN_DETACH;
	}

	if (HI_UNF_SND_Attach(player->hTrack, player->hPlayer) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_SND_Attach failed.\n", __FUNCTION__);
		goto TRACK_DESTROY;
	}

	HIADP_AVPlay_SetAdecAttr(player->hPlayer, HA_AUDIO_ID_AAC, HD_DEC_MODE_RAWPCM, 0);
	HIADP_AVPlay_SetVdecAttr(player->hPlayer, HI_UNF_VCODEC_TYPE_MPEG2, HI_UNF_VCODEC_MODE_NORMAL);

	HI_UNF_AVPLAY_GetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr);
	stSyncAttr.enSyncRef = HI_UNF_SYNC_REF_AUDIO;
	if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_AVPLAY_SetAttr failed.\n", __FUNCTION__);
		goto SND_DETACH;
	}

	if (HI_UNF_DMX_CreateTSBuffer(HI_UNF_DMX_PORT_RAM_0, 0x1000000, &player->hTsBuffer) != HI_SUCCESS)
		printf("[WARNING] %s -> Failed to create TS buffer.\n", __FUNCTION__);
	if (HI_UNF_AVPLAY_RegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_NEW_AUD_FRAME, (HI_UNF_AVPLAY_EVENT_CB_FN)player_event_handler) != HI_SUCCESS)
		printf("[WARNING] %s -> Failed to register audio event callback.\n", __FUNCTION__);
	if (HI_UNF_AVPLAY_RegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_NEW_VID_FRAME, (HI_UNF_AVPLAY_EVENT_CB_FN)player_event_handler) != HI_SUCCESS)
		printf("[WARNING] %s -> Failed to register video event callback.\n", __FUNCTION__);
	if (HI_UNF_AVPLAY_RegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_AUD_BUF_STATE, (HI_UNF_AVPLAY_EVENT_CB_FN)player_event_handler) != HI_SUCCESS)
		printf("[WARNING] %s -> Failed to register audio buffer event callback.\n", __FUNCTION__);
	if (HI_UNF_AVPLAY_RegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_VID_BUF_STATE, (HI_UNF_AVPLAY_EVENT_CB_FN)player_event_handler) != HI_SUCCESS)
		printf("[WARNING] %s -> Failed to register video buffer event callback.\n", __FUNCTION__);

	HI_UNF_DISP_SetVirtualScreen(HI_UNF_DISPLAY1, 1920, 1080);

	if (HI_MPI_AVPLAY_GetSyncVdecHandle(player->hPlayer, &player->hVdec, &player->hSync) != HI_SUCCESS)
		printf("[ERROR] %s: Failed to get Vdec and Sync handler from player.\n", __FUNCTION__);

	player->events[0].type = VIDEO_EVENT_SIZE_CHANGED;
	player->events[1].type = VIDEO_EVENT_FRAME_RATE_CHANGED;
	player->events[2].type = 16; /* VIDEO_EVENT_PROGRESSIVE_CHANGED */
	player->event_status = 0;
	pthread_mutex_init(&player->m_apts, NULL);
	pthread_mutex_init(&player->m_vpts, NULL);
	pthread_mutex_init(&player->m_event, NULL);
	pthread_mutex_init(&player->m_poll, NULL);
	pthread_mutex_init(&player->m_write, NULL);
	pthread_cond_init(&player->m_condition, NULL);
	player->m_buffer = create_buf(16 * 1024 * 1024);

	player->IsCreated		= true;
	player->PlayerMode		= 0;
	player->AudioPid		= 0x1FFFF;
	player->AudioPts		= HI_INVALID_PTS;
	player->AudioType		= HA_AUDIO_ID_AAC;
	player->AudioChannel	= 0;
	player->AudioState		= 0;
	player->VideoPid		= 0x1FFFF;
	player->VideoPts		= HI_INVALID_PTS;
	player->VideoType		= HI_UNF_VCODEC_TYPE_MPEG2;
	player->VideoState		= 0;
	player->VideoFormat		= 1;
	player->DisplayFormat	= 0;
	player->IsBlank			= true;
	player->IsSyncEnabled	= true;
	player->IsMute			= false;
	player->AudioBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;
	player->VideoBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;

	player_ops.priv = player;
	pthread_create(&player->m_thread, NULL, p2t_thread, NULL);
	player_create_painel();
	return true;

SND_DETACH:
	HI_UNF_SND_Detach(player->hTrack, player->hPlayer);
TRACK_DESTROY:
	HI_UNF_SND_DestroyTrack(player->hTrack);
WIN_DETACH:
	HI_UNF_VO_SetWindowEnable(player->hWindow, HI_FALSE);
	HI_UNF_VO_DetachWindow(player->hWindow, player->hPlayer);
ACHN_CLOSE:
	HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
VCHN_CLOSE:
	HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);
AVPLAY_DESTROY:
	HI_UNF_AVPLAY_Destroy(player->hPlayer);
AVPLAY_DEINIT:
	HI_UNF_AVPLAY_DeInit();
DMX_DEINIT:
	HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT);
	HI_UNF_DMX_DeInit();
VO_DEINIT:
	HI_UNF_VO_DestroyWindow(player->hWindow);
	HIADP_VO_DeInit();
DISP_DEINIT:
	HIADP_Disp_DeInit();
SND_DEINIT:
	HIADP_Snd_DeInit();
SYS_DEINIT:
	HI_SYS_DeInit();

	return false;
}

void player_destroy(void)
{
	printf("[INFO] %s -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (player && player->IsCreated)
	{
		HI_UNF_AVPLAY_STOP_OPT_S stStop;
		stStop.u32TimeoutMs = 0;
		stStop.enMode = HI_UNF_AVPLAY_STOP_MODE_BLACK;

		player_showtime();
		HI_UNF_KEYLED_DeInit();

		HI_UNF_AVPLAY_Stop(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, HI_NULL);
		HI_UNF_AVPLAY_Stop(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stStop);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_NEW_AUD_FRAME);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_NEW_VID_FRAME);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_AUD_BUF_STATE);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_VID_BUF_STATE);

		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);

		HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT);
		free_buf(player->m_buffer);
		if (player->hTsBuffer)
			HI_UNF_DMX_DestroyTSBuffer(player->hTsBuffer);

		HI_UNF_SND_Detach(player->hTrack, player->hPlayer);
		HI_UNF_SND_DestroyTrack(player->hTrack);

		HI_UNF_VO_SetWindowEnable(player->hWindow, HI_FALSE);
		HI_UNF_VO_DetachWindow(player->hWindow, player->hPlayer);

		HI_UNF_AVPLAY_Destroy(player->hPlayer);
		HI_UNF_AVPLAY_DeInit();

		HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT);
		HI_UNF_DMX_DeInit();

		HI_UNF_VO_DestroyWindow(player->hWindow);
		HIADP_VO_DeInit();

		HIADP_Disp_DeInit();
		HIADP_Snd_DeInit();
		HI_SYS_DeInit();
	}
	else
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
}

bool player_clear(int dev_type)
{
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	pthread_mutex_lock(&player->m_write);
	if (HI_UNF_DMX_ResetTSBuffer(player->hTsBuffer) != HI_SUCCESS)
		printf("[ERROR] %s: Failed to reset buffer for device type %d.\n", __FUNCTION__, dev_type);

	if (dev_type == DEV_AUDIO)
	{
		HI_UNF_AVPLAY_RESET_OPT_S stResetOpt;
		pthread_mutex_lock(&player->m_apts);
		stResetOpt.u32SeekPtsMs = player->AudioPts;
		if (HI_UNF_AVPLAY_Reset(player->hPlayer, &stResetOpt) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to reset player for device type %d.\n", __FUNCTION__, dev_type);
		pthread_mutex_unlock(&player->m_apts);
	}
	pthread_mutex_unlock(&player->m_write);

	return true;
}

bool player_set_type(int dev_type, int type)
{
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
		{
			HA_CODEC_ID_E htype;

			if (player->AudioState == 1)
			{
				printf("[ERROR] %s: Only can change Audio Type if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}

			switch (type)
			{
				case AUDIO_STREAMTYPE_AC3:
					htype = HA_AUDIO_ID_DOLBY_PLUS;
				break;
				case AUDIO_STREAMTYPE_MPEG:
				case AUDIO_STREAMTYPE_MP3:
					htype = HA_AUDIO_ID_MP3;
				break;
				case AUDIO_STREAMTYPE_LPCM:
					htype = HA_AUDIO_ID_PCM;
				break;
				case AUDIO_STREAMTYPE_AAC:
				case AUDIO_STREAMTYPE_AACPLUS:
				case AUDIO_STREAMTYPE_AACHE:
					htype = HA_AUDIO_ID_AAC;
				break;
				case AUDIO_STREAMTYPE_DTS:
				case AUDIO_STREAMTYPE_DTSHD:
					htype = HA_AUDIO_ID_DTSHD;
				break;
				case AUDIO_STREAMTYPE_DDP:
					htype = HA_AUDIO_ID_DOLBY_PLUS;
				break;
				case AUDIO_STREAMTYPE_RAW:
					htype = HA_AUDIO_ID_PCM;
				break;
				default: /* FallBack to MP3 */
					htype = HA_AUDIO_ID_MP3;
				break;
			}

			if (player->AudioType != htype)
			{
				if (HIADP_AVPlay_SetAdecAttr(player->hPlayer, htype, HD_DEC_MODE_RAWPCM, 0) != HI_SUCCESS)
				{
					printf("[ERROR] %s: Failed to set Audio Type %d.\n", __FUNCTION__, type);
					return false;
				}

				player->AudioType = htype;
			}

			/* Check HDMI OutPut Mode for Re-Apply this. */
			char s_mode[12];
			HI_UNF_SND_HDMI_MODE_E h_mode = HI_UNF_SND_HDMI_MODE_LPCM;
			FILE *file = fopen("/proc/stb/audio/ac3","r");

			if (file)
			{
				fgets(s_mode, 12, file);
				fclose(file);

				if (strcmp(s_mode, "downmix") == 0)
					h_mode = HI_UNF_SND_HDMI_MODE_LPCM;
				else if (strcmp(s_mode, "passthrough") == 0)
					h_mode = HI_UNF_SND_HDMI_MODE_RAW;
			}
			if (HI_UNF_SND_SetHdmiMode(HI_UNF_SND_0, HI_UNF_SND_OUTPUTPORT_HDMI0, h_mode) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to set HDMI OutPut Mode.\n", __FUNCTION__);
		}
		break;
		case DEV_VIDEO:
		{
			HI_UNF_VCODEC_TYPE_E htype;

			if (player->VideoState == 1)
			{
				printf("[ERROR] %s: Only can change Video Type if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}

			switch (type)
			{
				case VIDEO_STREAMTYPE_MPEG2:
					htype = HI_UNF_VCODEC_TYPE_MPEG2;
				break;
				case VIDEO_STREAMTYPE_MPEG4_H264:
					htype = HI_UNF_VCODEC_TYPE_H264;
				break;
				case VIDEO_STREAMTYPE_VC1:
				case VIDEO_STREAMTYPE_VC1_SM:
					htype = HI_UNF_VCODEC_TYPE_VC1;
				break;
				case VIDEO_STREAMTYPE_DIVX4:
				case VIDEO_STREAMTYPE_DIVX5:
				case VIDEO_STREAMTYPE_MPEG4_Part2:
					htype = HI_UNF_VCODEC_TYPE_MPEG4;
				break;
				case VIDEO_STREAMTYPE_H263:
					htype = HI_UNF_VCODEC_TYPE_H263;
				break;
				case VIDEO_STREAMTYPE_DIVX311:
					htype = HI_UNF_VCODEC_TYPE_DIVX3;
				break;
				case VIDEO_STREAMTYPE_H265_HEVC:
					htype = HI_UNF_VCODEC_TYPE_HEVC;
				break;
				case VIDEO_STREAMTYPE_MPEG1:
				default:
					htype = HI_UNF_VCODEC_TYPE_MPEG2;
				break;
			}

			if (player->VideoType == htype)
				return true;
			else if (HIADP_AVPlay_SetVdecAttr(player->hPlayer, htype, HI_UNF_VCODEC_MODE_NORMAL) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to set Video Type %d.\n", __FUNCTION__, type);
				return false;
			}

			player->VideoType = htype;
		}
		break;
	}

	return true;
}

bool player_set_pid(int dev_type, int pid)
{
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, pid);
	HI_HANDLE hChannel;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	else if (!(pid > 0 && pid < 0x1FFFF))
	{
		printf("[ERROR] %s(%d, %d) -> Wrong PID.\n", __FUNCTION__, dev_type, pid);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			if (player->AudioPid == pid)
				return true;
			else if (player->AudioState == 1)
			{
				printf("[ERROR] %s -> Only can change Audio PID if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}

			if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_AUD_PID, &pid) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set a new PID %d for Audio.\n", __FUNCTION__, pid);
				return false;
			}

			if (HI_UNF_AVPLAY_GetDmxAudChnHandle(player->hPlayer, &hChannel) == HI_SUCCESS)
				player_set_keyhandler(hChannel, pid);

			dvb_filter_pes2ts_init(&player->p2t[0], pid, dvb_filter_pes2ts_cb, player);
			player->AudioPid = pid;
		break;
		case DEV_VIDEO:
			if (player->VideoPid == pid)
				return true;
			else if (player->VideoState == 1)
			{
				printf("[ERROR] %s -> Only can change Video PID if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}

			if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_VID_PID, &pid) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set a new PID %d for Video.\n", __FUNCTION__, pid);
				return false;
			}

			if (HI_UNF_AVPLAY_GetDmxVidChnHandle(player->hPlayer, &hChannel) == HI_SUCCESS)
				player_set_keyhandler(hChannel, pid);

			dvb_filter_pes2ts_init(&player->p2t[1], pid, dvb_filter_pes2ts_cb, player);
			player->VideoPid = pid;
		break;
	}

	return true;
}

bool player_set_mode(int mode)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, mode);
	int i;
	HI_UNF_AVPLAY_ATTR_S stAvplayAttr;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	else if (HI_UNF_AVPLAY_GetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_STREAM_MODE, &stAvplayAttr) != HI_SUCCESS)
		return false;

	/** Clean Events **/
	pthread_mutex_lock(&player->m_event);
	player->event_status = 0;
	for (i = 0; i < 3; i++)
		if (player->events[i].active)
			player->events[i].active = false;
	pthread_mutex_unlock(&player->m_event);

	/** Clear PTS **/
	pthread_mutex_lock(&player->m_apts);
	player->AudioPts = HI_INVALID_PTS;
	pthread_mutex_unlock(&player->m_apts);

	pthread_mutex_lock(&player->m_vpts);
	player->VideoPts = HI_INVALID_PTS;
	pthread_mutex_unlock(&player->m_vpts);

	pthread_mutex_lock(&player->m_poll);
	player->AudioBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;
	player->VideoBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;
	pthread_mutex_unlock(&player->m_poll);

	if (mode == 1) /* MEMORY */
	{
		char *buf;
		HI_UNF_DMX_PORT_E enFromPortId;

		/** Reset buffers **/
		if (get_max_read_size(player->m_buffer) > 0)
		{
			buf = malloc(get_max_read_size(player->m_buffer));
			read_buf(player->m_buffer, buf, get_max_read_size(player->m_buffer));
			free(buf);
		}

		/** Set PIDs if necessary. **/
		if (!(player->AudioPid > 0 && player->AudioPid < 0x1FFFF))
			player_set_pid(DEV_AUDIO, 100);
		if (!(player->VideoPid > 0 && player->VideoPid < 0x1FFFF))
			player_set_pid(DEV_VIDEO, 101);

		/** Change Port **/
		if (HI_UNF_DMX_GetTSPortId(stAvplayAttr.u32DemuxId, &enFromPortId) == HI_SUCCESS)
		{
			if (enFromPortId == HI_UNF_DMX_PORT_RAM_0)
				return true;

			else if (HI_UNF_DMX_DetachTSPort(stAvplayAttr.u32DemuxId) != HI_SUCCESS)
				return false;
		}

		if (HI_UNF_DMX_AttachTSPort(stAvplayAttr.u32DemuxId, HI_UNF_DMX_PORT_RAM_0) != HI_SUCCESS)
		{
			printf("[ERROR] %s -> Failed to set Mode %d.\n", __FUNCTION__, mode);
			return false;
		}

		player->IsPES = true;
	}
	else
		player->IsPES = false;

	player->PlayerMode = mode;
	pthread_cond_signal(&player->m_condition);

	return true;
}

bool player_set_blank(bool blank)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, blank);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->IsBlank = blank;

	return true;
}

bool player_set_format(int format)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, format);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->VideoFormat = format;

	return true;
}

bool player_set_display_format(int format)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, format);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->DisplayFormat = format;

	return true;
}

bool player_set_fastfoward(int speed)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, speed);
	//HI_UNF_AVPLAY_TPLAY_OPT_S stTplayOpts;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	/*pthread_mutex_lock(&player->m_write);
	if (speed == 0)
	{
		if (HI_UNF_AVPLAY_SetDecodeMode(player->hPlayer, HI_UNF_VCODEC_MODE_NORMAL) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to set decoding mode.\n", __FUNCTION__);
		if (HI_UNF_AVPLAY_Resume(player->hPlayer, HI_NULL) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to resume from FF.", __FUNCTION__);

		pthread_mutex_unlock(&player->m_write);
		return true;
	}

	if (HI_UNF_AVPLAY_SetDecodeMode(player->hPlayer, HI_UNF_VCODEC_MODE_I) != HI_SUCCESS)
		printf("[ERROR] %s: Failed to set decoding mode.\n", __FUNCTION__);

	stTplayOpts.enTplayDirect = (speed < 0) ? HI_UNF_AVPLAY_TPLAY_DIRECT_BACKWARD : HI_UNF_AVPLAY_TPLAY_DIRECT_FORWARD;
	stTplayOpts.u32SpeedInteger = abs(speed);
	stTplayOpts.u32SpeedDecimal = 0;
	if (HI_UNF_AVPLAY_Tplay(player->hPlayer, &stTplayOpts) != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to set speed to %d.\n", __FUNCTION__, speed);
		pthread_mutex_unlock(&player->m_write);
		return false;
	}

	pthread_mutex_unlock(&player->m_write);*/
	return true;
}

bool player_play(int dev_type)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, dev_type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			if (player->AudioState == 1)
			{
				printf("[ERROR] %s -> Only can play Audio if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}
			else if (HI_UNF_AVPLAY_Start(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, NULL) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to play Audio.\n", __FUNCTION__);
				return false;
			}

			player->AudioState = 1;
		break;
		case DEV_VIDEO:
			if (player->VideoState == 1)
			{
				printf("[ERROR] %s -> Only can play Video if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}
			else if (HI_UNF_AVPLAY_Start(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, NULL) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to play Video.\n", __FUNCTION__);
				return false;
			}

			player->VideoState = 1;
		break;
	}

	return true;
}

bool player_pause(int dev_type)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, dev_type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	else if (!player->hSync)
		printf("[ERROR] %s: Sync handler it's not setted.\n", __FUNCTION__);

	switch (dev_type)
	{
		case DEV_AUDIO:
		{
			if (player->AudioState == 2)
				return true;
			else if (player->AudioState == 0)
				return false;

			pthread_mutex_lock(&player->m_write);
			if (HI_MPI_SYNC_Stop(player->hSync, SYNC_CHAN_AUD) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to stop sync Audio.\n", __FUNCTION__);
			if (HI_MPI_AO_Track_Pause(player->hTrack) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to pause Audio.\n", __FUNCTION__);
				pthread_mutex_unlock(&player->m_write);
				return false;
			}

			player->AudioState = 2;
			pthread_mutex_unlock(&player->m_write);
		}
		break;
		case DEV_VIDEO:
			if (player->VideoState == 2)
				return true;
			else if (player->VideoState == 0)
				return false;

			pthread_mutex_lock(&player->m_write);
			if (HI_MPI_SYNC_Stop(player->hSync, SYNC_CHAN_VID) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to stop sync Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Pause(player->hWindow, HI_TRUE) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to pause Video.\n", __FUNCTION__);
				pthread_mutex_unlock(&player->m_write);
				return false;
			}
			if (HI_MPI_WIN_Freeze(player->hWindow, HI_TRUE, HI_DRV_WIN_SWITCH_LAST) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to freeze Video.\n", __FUNCTION__);

			player->VideoState = 2;
			pthread_mutex_unlock(&player->m_write);
		break;
	}

	return true;
}

bool player_resume(int dev_type)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, dev_type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	else if (!player->hSync)
		printf("[ERROR] %s: Sync handler it's not setted.\n", __FUNCTION__);

	switch (dev_type)
	{
		case DEV_AUDIO:
		{
			if (player->AudioState == 1)
				return true;
			else if (player->AudioState == 0)
				return false;

			pthread_mutex_lock(&player->m_write);
			if (HI_MPI_SYNC_Start(player->hSync, SYNC_CHAN_AUD) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to start sync Audio.\n", __FUNCTION__);
			if (HI_MPI_AO_Track_Resume(player->hTrack) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to resume Audio.\n", __FUNCTION__);
				pthread_mutex_unlock(&player->m_write);
				return false;
			}

			player->AudioState = 1;
			pthread_mutex_unlock(&player->m_write);
		}
		break;
		case DEV_VIDEO:
			if (player->VideoState == 1)
				return true;
			else if (player->VideoState == 0)
				return false;

			pthread_mutex_lock(&player->m_write);
			if (HI_MPI_SYNC_Start(player->hSync, SYNC_CHAN_VID) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to start sync Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Freeze(player->hWindow, HI_FALSE, HI_DRV_WIN_SWITCH_LAST) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to unfreeze Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Pause(player->hWindow, HI_FALSE) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to resume Video.\n", __FUNCTION__);
				pthread_mutex_unlock(&player->m_write);
				return false;
			}

			player->VideoState = 1;
			pthread_mutex_unlock(&player->m_write);
		break;
	}

	return true;
}

bool player_stop(int dev_type)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, dev_type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			if (player->AudioState == 0)
				return true;
			else if (HI_UNF_AVPLAY_Stop(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD, HI_NULL) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to stop Audio.\n", __FUNCTION__);
				return false;
			}

			player->AudioState = 0;
		break;
		case DEV_VIDEO:
		{
			HI_UNF_AVPLAY_STOP_OPT_S stStop;

			if (player->VideoState == 0)
				return true;

			stStop.u32TimeoutMs = 0;
			stStop.enMode = player->IsBlank ? HI_UNF_AVPLAY_STOP_MODE_BLACK : HI_UNF_AVPLAY_STOP_MODE_STILL;

			if (HI_UNF_AVPLAY_Stop(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID, &stStop) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to stop Video.\n", __FUNCTION__);
				return false;
			}

			player->VideoState = 0;
		}
		break;
	}

	return true;
}

bool player_mute(bool mute)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, mute);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->IsMute = mute;

	return true;
}

bool player_sync(bool sync)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, sync);
	HI_UNF_SYNC_ATTR_S stSyncAttr;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	if (HI_UNF_AVPLAY_GetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr) == HI_SUCCESS)
	{
		stSyncAttr.enSyncRef = sync ? HI_UNF_SYNC_REF_AUDIO : HI_UNF_SYNC_REF_NONE;

		if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr) == HI_SUCCESS)
		{
			player->IsSyncEnabled = sync;
			return true;
		}
	}

	printf("[ERROR] %s -> Failed to set Sync.\n", __FUNCTION__);
	return false;
}

bool player_channel(int channel)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, channel);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->AudioChannel = channel;

	return true;
}

bool player_get_status(int dev_type, void *data)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	
	switch (dev_type)
	{
		case DEV_AUDIO:
		{
			audio_status_t *status = (audio_status_t *)data;

			if (!status)
				return false;

			status->AV_sync_state	= player->IsSyncEnabled;
			status->mute_state		= player->IsMute;
			status->play_state		= player->AudioState;
			status->stream_source	= player->PlayerMode;
			status->channel_select	= player->AudioChannel;
			status->bypass_mode		= (player->AudioType != HA_AUDIO_ID_AAC && player->AudioType != 10);
			//status->mixer_state;
			printf("[WARNING] %s -> Mixer state not implemented.\n", __FUNCTION__);
		}
		break;
		case DEV_VIDEO:
		{
			struct video_status *status = (struct video_status *)data;

			if (!status)
				return false;

			pthread_mutex_lock(&player->m_event);
			status->video_blank		= player->IsBlank;		/* blank video on freeze? */
			status->play_state		= player->VideoState;	/* current state of playback */
			status->stream_source	= player->PlayerMode;	/* current source (demux/memory) */
			status->video_format	= player->VideoFormat;	/* current aspect ratio of stream */
			status->display_format	= player->DisplayFormat;/* selected cropping mode */
			pthread_mutex_unlock(&player->m_event);
		}
		break;
		default:
			return false;
		break;
	}

	return true;
}

bool player_get_event(struct video_event *event)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	int i;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	pthread_mutex_lock(&player->m_event);
	for (i = 0; i < 3; i++)
	{
		if (player->events[i].active)
			if (player->event_status & (1 << player->events[i].type))
			{
				switch (player->events[i].type)
				{
					case VIDEO_EVENT_SIZE_CHANGED:
						event->u.size.w = player->events[i].width;
						event->u.size.h = player->events[i].heigth;
						event->u.size.aspect_ratio = player->events[i].aspect;
					break;
					case VIDEO_EVENT_FRAME_RATE_CHANGED:
						event->u.frame_rate = player->events[i].framerate;
					break;
					case 16: /* VIDEO_EVENT_PROGRESSIVE_CHANGED */
						event->u.frame_rate = player->events[i].progressive;
					break;
					default:
						printf("[ERROR] %s: The event type %d not is managed.", __FUNCTION__, player->events[i].type);
						pthread_mutex_unlock(&player->m_event);
						return false;
					break;
				}

				event->type = player->events[i].type;
				player->event_status &= ~(1 << player->events[i].type);
				pthread_mutex_unlock(&player->m_event);
				return true;
			}
	}
	pthread_mutex_unlock(&player->m_event);

	return false;
}

bool player_get_vsize(video_size_t *vsize)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	pthread_mutex_lock(&player->m_event);
	if (player->events[0].active)
	{
		vsize->w = player->events[0].width;
		vsize->h = player->events[0].heigth;
		vsize->aspect_ratio = player->events[0].aspect;

		pthread_mutex_unlock(&player->m_event);
		return true;
	}
	pthread_mutex_unlock(&player->m_event);

	return false;
}

bool player_get_framerate(unsigned int *framerate)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	pthread_mutex_lock(&player->m_event);
	if (player->events[1].active)
		*framerate = player->events[1].framerate;
	else
		*framerate = -1;
	pthread_mutex_unlock(&player->m_event);

	return true;
}

bool player_get_pts(int dev_type, long long *pts)
{
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	pthread_mutex_lock(&player->m_write);
	switch (dev_type)
	{
		case DEV_AUDIO:
			pthread_mutex_lock(&player->m_apts);
			*pts = player->AudioPts;
			pthread_mutex_unlock(&player->m_apts);
		break;
		case DEV_VIDEO:
			pthread_mutex_lock(&player->m_vpts);
			*pts = player->VideoPts;
			pthread_mutex_unlock(&player->m_vpts);
		break;
	}
	pthread_mutex_unlock(&player->m_write);

	return (*pts != HI_INVALID_PTS);
}

int player_poll(int dev_type, struct fuse_pollhandle *ph, unsigned *reventsp, bool condition)
{
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return 0;
	}

	pthread_mutex_lock(&player->m_poll);
	if (ph)
	{
		struct fuse_pollhandle *oldph = player->poll_handle[dev_type];

		if (oldph)
			fuse_pollhandle_destroy(oldph);

		player->poll_status |= (1 << dev_type);
		player->poll_handle[dev_type] = ph;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			switch (player->AudioBufferState)
			{
				case HI_UNF_AVPLAY_BUF_STATE_BUTT:
				case HI_UNF_AVPLAY_BUF_STATE_EMPTY:
				case HI_UNF_AVPLAY_BUF_STATE_LOW:
				case HI_UNF_AVPLAY_BUF_STATE_NORMAL:
				case HI_UNF_AVPLAY_BUF_STATE_HIGH:
					*reventsp |= (POLLOUT | POLLWRNORM);
				break;
				default:
				break;
			}
		break;
		case DEV_VIDEO:
			switch (player->VideoBufferState)
			{
				case HI_UNF_AVPLAY_BUF_STATE_BUTT:
				case HI_UNF_AVPLAY_BUF_STATE_EMPTY:
				case HI_UNF_AVPLAY_BUF_STATE_LOW:
				case HI_UNF_AVPLAY_BUF_STATE_NORMAL:
				//case HI_UNF_AVPLAY_BUF_STATE_HIGH: /* Adicionar para teste no futuro. */
					if (condition)
						*reventsp |= (POLLOUT | POLLWRNORM);
				break;
				default:
				break;
			}
		break;
	}
	pthread_mutex_unlock(&player->m_poll);

	if (dev_type == DEV_VIDEO)
	{
		pthread_mutex_lock(&player->m_event);
		if (player->event_status != 0)
			*reventsp |= POLLPRI;
		pthread_mutex_unlock(&player->m_event);
	}

	return 0;
}

int player_write(int dev_type, const char *buf, size_t size)
{
	bool IsHeader;
	unsigned char c_s;
	unsigned char c_e;
	HI_UNF_STREAM_BUF_S sBuf;
	struct dvb_filter_pes2ts *p2t;
	unsigned char h = buf[0];
	unsigned char e = buf[1];
	unsigned char a = buf[2];
	unsigned char d = buf[3];
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return -1;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			c_s = AUDIO_STREAM_S;
			c_e = AUDIO_STREAM_E;
			p2t = &player->p2t[0];
		break;
		case DEV_VIDEO:
			c_s = VIDEO_STREAM_S;
			c_e = VIDEO_STREAM_E;
			p2t = &player->p2t[1];
		break;
		case DEV_DVR:
			if (player->PlayerMode != 1)
				player_set_mode(1);

			pthread_mutex_lock(&player->m_write);
			if (HI_UNF_DMX_GetTSBuffer(player->hTsBuffer, size, &sBuf, 50000 /* 50ms */) == HI_SUCCESS)
			{
				memcpy(sBuf.pu8Data, buf, size);

				if (HI_UNF_DMX_PutTSBuffer(player->hTsBuffer, size) == HI_SUCCESS)
				{
					pthread_mutex_unlock(&player->m_write);
					return size;
				}
				else
					printf("[ERROR] %s: Failed to put buffer for device type %d.\n", __FUNCTION__, dev_type);
			}
			else
				printf("[ERROR] %s: Failed to get buffer for device type %d and size %d.\n", __FUNCTION__, dev_type, size);
			pthread_mutex_unlock(&player->m_write);

			return 0;
		break;
		case DEV_PAINEL:
		{
			if (strncmp(buf, "TIME", 4) == HI_SUCCESS)
				player_showtime();
			else
			{
				HI_U8 DigDisCode[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
				HI_U8 UCharDisCode[] = {
					0xFF, 0xFF, 0x39, 0xFF, 0x79,
					0x71, 0xFF, 0x76, 0xFF, 0xFF,
					0xFF, 0x38, 0x95, 0xFF, 0x3f,
					0x73, 0xFF, 0x77,/*R*/ 0x6d, 0xFF,
					0x3e, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF
				};
				//Uppercase letters can only be displayed 'C'、'E'、'F'、'H'、'L'、'O'、'P'、'S'、'U'，From left to right respectively correspond to this array 0xFF value.
				HI_U8 LCharDisCode[] = {
					0xFF, 0x7c, 0x58, 0x5e, 0x79,
					0xFF, 0x6f, 0x74, 0x30, 0xFF,
					0xFF, 0x38, 0xFF, 0x54, 0x5c,
					0x73, 0x67, 0xFF, 0x6d, 0x78,
					0x1c, 0xFF, 0xFF, 0xFF, 0xFF,
					0xFF
				};

				int i, value = 0;
				for (i = 0; i < 4; i++)
				{
					if (buf[i] >= '0' && buf[i] <= '9')
						value |= DigDisCode[buf[i] - 48] << (8 * i);
					else if (buf[i] >= 'A' && buf[i] <= 'Z')
						value |= UCharDisCode[buf[i] - 65] << (8 * i);
					else
						value |= LCharDisCode[buf[i] - 97] << (8 * i);
				}

				if (HI_UNF_LED_Display(value) != HI_SUCCESS)
					printf("[ERROR] %s: The value(%s) not writed to keyled.\n", __FUNCTION__, buf);
			}

			return size;
		}
		break;
		default:
			return size;
		break;
	}

	if (h == 0x00 && e == 0x00 && a == 0x01 && d >= c_s && d <= c_e)
		IsHeader = true;

	dvb_filter_pes2ts(p2t, (void *)buf, size, IsHeader);

	/*time_t seconds = 10;
	time_t start = time(NULL);
	time_t endwait = start + seconds;

	while (start < endwait)
	{
		pthread_mutex_lock(&player->m_write);
		if (HI_UNF_DMX_GetTSBuffer(player->hTsBuffer, ts_total_size, &sBuf, 1000) == HI_SUCCESS)
		{
			int ret = 0;
			char *from = malloc(data_size);

			if (!read_buf(rbuf, from, IsHeader ? data_size : data_size - size))
				printf("[ERROR] %s: Failed to read buffer for device type %d.\n", __FUNCTION__, dev_type);
			if (!IsHeader)
				memcpy(&from[data_size - size], buf, size);

			//player_pes2ts(player, sBuf.pu8Data, from, data_size);

			if (HI_UNF_DMX_PutTSBuffer(player->hTsBuffer, ts_total_size) == HI_SUCCESS)
			{
				ret = size;

				if (IsHeader)
				{
					write_to_buf(rbuf, (char *)buf, size);
					*pkt_size = ((buf[4]<<8) | buf[5]) + 6;
				}
			}
			else
				printf("[ERROR] %s: Failed to put buffer for device type %d.\n", __FUNCTION__, dev_type);

			pthread_mutex_unlock(&player->m_write);
			return ret;
		}
		pthread_mutex_unlock(&player->m_write);
		start = time(NULL);
	};*/

	return size;
}

struct class_ops *player_get_ops(void)
{
	player_ops.create			= player_create;
	player_ops.destroy			= player_destroy;
	player_ops.clear			= player_clear;
	player_ops.set_type			= player_set_type;
	player_ops.set_pid			= player_set_pid;
	player_ops.set_mode			= player_set_mode;
	player_ops.set_blank		= player_set_blank;
	player_ops.set_format		= player_set_format;
	player_ops.set_disp_format	= player_set_display_format;
	player_ops.set_fastfoward	= player_set_fastfoward;
    player_ops.play				= player_play;
	player_ops.pause			= player_pause;
	player_ops.resume			= player_resume;
	player_ops.stop				= player_stop;
	player_ops.mute				= player_mute;
    player_ops.sync				= player_sync;
	player_ops.channel			= player_channel;
	player_ops.status			= player_get_status;
	player_ops.get_event		= player_get_event;
	player_ops.get_vsize		= player_get_vsize;
	player_ops.get_framerate	= player_get_framerate;
	player_ops.get_pts			= player_get_pts;
	player_ops.poll				= player_poll;
	player_ops.write			= player_write;

	return &player_ops;
}
