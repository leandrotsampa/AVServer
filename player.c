#include <AVServer.h>
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

#define AUDIO_STREAM_S   0xC0
#define AUDIO_STREAM_E   0xDF
#define VIDEO_STREAM_S   0xE0
#define VIDEO_STREAM_E   0xEF

#define MAX_ADAPTER	4
#define PLAYER_DEMUX_PORT 4

static struct class_ops player_ops;

struct s_player {
	bool IsCreated;
	int PlayerMode;		/* 0 demux, 1 memory */
	int AudioPid;		/* unknown pid */
	int AudioType;
	int AudioChannel;	/* 0 stereo, 1 left, 2 right */
	int AudioState;		/* 0 stoped, 1 playing, 2 paused */
	int VideoPid;		/* unknown pid */
	int VideoType;
	int VideoState;		/* 0 stoped, 1 playing, 2 freezed */
	int VideoFormat;	/* 0 4:3, 1 16:9, 2 2.21:1 */
	int DisplayFormat;	/* 0 Pan&Scan, 1 Letterbox, 2 Center Cut Out */

	bool IsPES;
	bool IsDVR;
	bool IsTimeShift;
	bool IsBlank;
	bool IsStopThread;
	bool IsSyncEnabled;
	bool IsMute;

	int AudioBufferState;
	int VideoBufferState;

	unsigned int hPlayer;
	unsigned int hWindow;
	unsigned int hTrack;
	unsigned int hTsBuffer;
	unsigned int hVdec;
	unsigned int hSync;

	unsigned poll_status;
	unsigned event_status;

	pthread_mutex_t m_apts;
	pthread_mutex_t m_vpts;
	pthread_mutex_t m_event;
	pthread_mutex_t m_poll;
	pthread_rwlock_t m_write;

	struct {
		bool active;

		int type;
		int width;
		int heigth;
		int aspect;
		unsigned int framerate;
		bool progressive;
	} events[3];

	struct {
		char header[256];
		size_t size;
		size_t es_size;
		long long pts;
	} p2e[2];

	struct fuse_pollhandle *poll_handle[2];
};

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

	for (i = 0; i < MAX_ADAPTER; i++)
	{
		HI_HANDLE hChannel;

		if (HI_UNF_DMX_GetChannelHandle(i, (HI_U32)pid, &hChannel) == HI_SUCCESS)
		{
			HI_HANDLE hKey;

			if (HI_UNF_DMX_GetDescramblerKeyHandle(hChannel, &hKey) == HI_SUCCESS)
			{
				HI_HANDLE hPKey;

				if (HI_UNF_DMX_GetDescramblerKeyHandle(hPChannel, &hPKey) == HI_SUCCESS)
					if (hPKey != hKey)
						if (HI_UNF_DMX_DetachDescrambler(hPKey, hPChannel) != HI_SUCCESS)
							printf("[ERROR] %s: Failed to detach KeyHandle from Player.\n", __FUNCTION__);

				if (HI_UNF_DMX_AttachDescrambler(hKey, hPChannel) != HI_SUCCESS)
					printf("[ERROR] %s: Failed to attach KeyHandle to PID %d.\n", __FUNCTION__, pid);
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
		FILE *file = NULL;
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

		/** Fix Auto-change of 4:3 <--> 16:9 **/
		file = fopen((aspect == VIDEO_FORMAT_4_3) ? "/proc/stb/video/policy" : "/proc/stb/video/policy2", "r");
		if (file)
		{
			char mode[10];
			HI_UNF_WINDOW_ATTR_S pAttr;
			fgets(mode, 10, file);
			fclose(file);

			if (HI_UNF_VO_GetWindowAttr(player->hWindow, &pAttr) == HI_SUCCESS)
			{
				unsigned int h, w;
				HI_UNF_VO_ASPECT_CVRS_E aspect = HI_UNF_VO_ASPECT_CVRS_IGNORE; /* Valid for bestfit/non/nonlinear/scale */

				if (strEquals(mode, "letterbox", false))
					aspect = HI_UNF_VO_ASPECT_CVRS_LETTERBOX;
				else if (strEquals(mode, "panscan", false))
					aspect = HI_UNF_VO_ASPECT_CVRS_PAN_SCAN;

				pAttr.bVirtual          = HI_FALSE;
				pAttr.bUseCropRect      = HI_FALSE;
				pAttr.stOutputRect.s32X = 0;
				pAttr.stOutputRect.s32Y = 0;

				if (HI_UNF_DISP_GetVirtualScreen(HI_UNF_DISPLAY1, &w, &h) == HI_SUCCESS)
				{
					w -= 2;
					h -= 4;

					if (pAttr.stOutputRect.s32Width != w || pAttr.stOutputRect.s32Height != h || pAttr.stWinAspectAttr.enAspectCvrs != aspect)
					{
						pAttr.stOutputRect.s32Width = w;
						pAttr.stOutputRect.s32Height= h;
						pAttr.stWinAspectAttr.enAspectCvrs = aspect;

						HI_UNF_VO_SetWindowAttr(player->hWindow, &pAttr);
					}
				}
			}
		}
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
					case HI_UNF_AVPLAY_BUF_STATE_HIGH:
						if (player->poll_status & (1 << DEV_VIDEO))
						{
							struct fuse_pollhandle *ph = player->poll_handle[DEV_VIDEO];
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
	DIR *lib_dir;
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

	if (HIADP_Disp_Init(HI_UNF_ENC_FMT_720P_60) != HI_SUCCESS)
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

	/** Auto load all codecs **/
	lib_dir = opendir ("/usr/lib");
	if (lib_dir)
	{
		struct dirent *file = readdir(lib_dir);

		while (file)
		{
			if (startsWith(file->d_name, "libHA.AUDIO.") && endsWith(file->d_name, "decode.so"))
				if (HI_UNF_AVPLAY_RegisterAcodecLib(file->d_name) != HI_SUCCESS)
					printf("[ERROR] %s: Failed to Register Audio library %s\n", __FUNCTION__, file->d_name);

			if (startsWith(file->d_name, "libHV.VIDEO.") && endsWith(file->d_name, "decode.so"))
				if (HI_UNF_AVPLAY_RegisterVcodecLib(file->d_name) != HI_SUCCESS)
					printf("[ERROR] %s: Failed to Register Video library %s\n", __FUNCTION__, file->d_name);

			file = readdir(lib_dir);
		}

		closedir (lib_dir);
	}
	else if (HIADP_AVPlay_RegADecLib() != HI_SUCCESS)
		printf("[ERROR] %s -> HIADP_AVPlay_RegADecLib failed.\n", __FUNCTION__);

	if (HI_UNF_AVPLAY_Init() != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_AVPLAY_Init failed.\n", __FUNCTION__);
		goto DMX_DEINIT;
	}

	HI_UNF_AVPLAY_GetDefaultConfig(&stAvplayAttr, HI_UNF_AVPLAY_STREAM_TYPE_TS);
	stAvplayAttr.u32DemuxId = PLAYER_DEMUX_PORT;
	stAvplayAttr.stStreamAttr.u32AudBufSize = 4 * 1024 * 1024;  // Allocate 4MB
	stAvplayAttr.stStreamAttr.u32VidBufSize = 16 * 1024 * 1024; // Allocate 16MB
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
	pthread_mutex_init(&player->m_apts,   0);
	pthread_mutex_init(&player->m_vpts,   0);
	pthread_mutex_init(&player->m_event,  0);
	pthread_mutex_init(&player->m_poll,   0);
	pthread_rwlock_init(&player->m_write, 0);

	player->IsCreated		= true;
	player->PlayerMode		= 0;
	player->AudioPid		= 0x1FFFF;
	player->AudioType		= HA_AUDIO_ID_AAC;
	player->AudioChannel	= 0;
	player->AudioState		= 0;
	player->VideoPid		= 0x1FFFF;
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
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_NEW_VID_FRAME);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_AUD_BUF_STATE);
		HI_UNF_AVPLAY_UnRegisterEvent(player->hPlayer, HI_UNF_AVPLAY_EVENT_VID_BUF_STATE);

		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);

		HI_UNF_DMX_DetachTSPort(PLAYER_DEMUX_PORT);
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

	pthread_rwlock_rdlock(&player->m_write);
	if (player->IsPES)
		if (HI_UNF_AVPLAY_FlushStream(player->hPlayer, HI_NULL) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to Flush buffer.\n", __FUNCTION__);

	/** Remove poll from waiting state. **/
	pthread_mutex_lock(&player->m_poll);
	switch (dev_type)
	{
		case DEV_AUDIO:
			if (HI_UNF_AVPLAY_Reset(player->hPlayer, HI_NULL) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to Reset player.\n", __FUNCTION__);

			player->AudioBufferState = HI_UNF_AVPLAY_BUF_STATE_EMPTY;
		break;
		case DEV_VIDEO:
			player->VideoBufferState = HI_UNF_AVPLAY_BUF_STATE_EMPTY;
		break;
	}

	if (player->poll_status & (1 << dev_type))
	{
		struct fuse_pollhandle *ph = player->poll_handle[dev_type];
		fuse_notify_poll(ph);
		fuse_pollhandle_destroy(ph);
		player->poll_handle[dev_type] = NULL;
		player->poll_status &= ~(1 << dev_type);
	}
	pthread_mutex_unlock(&player->m_poll);
	pthread_rwlock_unlock(&player->m_write);

	return true;
}

void player_set_dvr(bool status)
{
	printf("[INFO] %s(%s) -> called.\n", __FUNCTION__, status ? "true" : "false");
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
	else
		player->IsDVR = status;
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
			char s_mode[12];
			HA_CODEC_ID_E htype;
			FILE *file = NULL;
			HI_UNF_SND_HDMI_MODE_E h_mode = HI_UNF_SND_HDMI_MODE_LPCM;

			switch (type)
			{
				case AUDIO_STREAMTYPE_AC3:
				case AUDIO_STREAMTYPE_DDP:
					htype = HA_AUDIO_ID_DOLBY_PLUS;
					file  = fopen("/proc/stb/audio/ac3", "r");
				break;
				case AUDIO_STREAMTYPE_AAC:
				case AUDIO_STREAMTYPE_AACPLUS:
				case AUDIO_STREAMTYPE_AACHE:
					htype = HA_AUDIO_ID_AAC;
					file  = fopen("/proc/stb/audio/aac", "r");
				break;
				case AUDIO_STREAMTYPE_DTS:
				case AUDIO_STREAMTYPE_DTSHD:
					htype = HA_AUDIO_ID_DTSHD;
				break;
				case AUDIO_STREAMTYPE_RAW:
				case AUDIO_STREAMTYPE_LPCM:
					htype = HA_AUDIO_ID_PCM;
				break;
				case AUDIO_STREAMTYPE_MP3:
				case AUDIO_STREAMTYPE_MPEG:
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
			if (file)
			{
				fgets(s_mode, 12, file);
				fclose(file);

				if (strEquals(s_mode, "downmix", false))
					h_mode = HI_UNF_SND_HDMI_MODE_LPCM;
				else if (strEquals(s_mode, "passthrough", false))
					h_mode = HI_UNF_SND_HDMI_MODE_RAW;
			}
			if (HI_UNF_SND_SetHdmiMode(HI_UNF_SND_0, HI_UNF_SND_OUTPUTPORT_HDMI0, h_mode) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to set HDMI OutPut Mode.\n", __FUNCTION__);
		}
		break;
		case DEV_VIDEO:
		{
			HI_UNF_VCODEC_TYPE_E htype;

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
	else if (player->IsPES)
		return true;

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

	if (mode == 1) /* MEMORY */
	{
		/** Change to ES Mode **/
		if (stAvplayAttr.stStreamAttr.enStreamType != HI_UNF_AVPLAY_STREAM_TYPE_ES)
		{
			stAvplayAttr.stStreamAttr.enStreamType = HI_UNF_AVPLAY_STREAM_TYPE_ES;
			if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_STREAM_MODE, &stAvplayAttr) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to change to Memory Mode.\n", __FUNCTION__);
		}

		player->AudioPid = 0;
		player->VideoPid = 0;
		player->IsPES = true;
	}
	else
	{
		player->IsPES = false;

		if (player->IsDVR)
		{
			HI_UNF_DMX_PORT_E enFromPortId;

			/** Change Port **/
			if (HI_UNF_DMX_GetTSPortId(stAvplayAttr.u32DemuxId, &enFromPortId) == HI_SUCCESS)
			{
				if (enFromPortId == HI_UNF_DMX_PORT_RAM_0)
					goto TS;
				else if (HI_UNF_DMX_DetachTSPort(stAvplayAttr.u32DemuxId) != HI_SUCCESS)
					return false;
			}

			if (HI_UNF_DMX_AttachTSPort(stAvplayAttr.u32DemuxId, HI_UNF_DMX_PORT_RAM_0) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set Mode %d.\n", __FUNCTION__, mode);
				return false;
			}
		}

TS:
		/** Change to TS Mode **/
		if (stAvplayAttr.stStreamAttr.enStreamType != HI_UNF_AVPLAY_STREAM_TYPE_TS)
		{
			player_ops.stop(DEV_AUDIO);
			player_ops.stop(DEV_VIDEO);

			stAvplayAttr.stStreamAttr.enStreamType = HI_UNF_AVPLAY_STREAM_TYPE_TS;
			if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_STREAM_MODE, &stAvplayAttr) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to change to Demux Mode.\n", __FUNCTION__);
		}
	}

	player->PlayerMode = mode;

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

	/*pthread_rwlock_rdlock(&player->m_write);
	if (speed == 0)
	{
		if (HI_UNF_AVPLAY_SetDecodeMode(player->hPlayer, HI_UNF_VCODEC_MODE_NORMAL) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to set decoding mode.\n", __FUNCTION__);
		if (HI_UNF_AVPLAY_Resume(player->hPlayer, HI_NULL) != HI_SUCCESS)
			printf("[ERROR] %s: Failed to resume from FF.", __FUNCTION__);

		pthread_rwlock_unlock(&player->m_write);
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
		pthread_rwlock_unlock(&player->m_write);
		return false;
	}

	pthread_rwlock_unlock(&player->m_write);*/
	return true;
}

bool player_set_slowmotion(int speed)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, speed);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	if (speed == 11)
		player->IsTimeShift = true;
	else if (speed == 10)
		player->IsTimeShift = false;

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

			pthread_mutex_lock(&player->m_poll);
			player->AudioBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;
			pthread_mutex_unlock(&player->m_poll);

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

			pthread_mutex_lock(&player->m_poll);
			player->VideoBufferState= HI_UNF_AVPLAY_BUF_STATE_BUTT;
			pthread_mutex_unlock(&player->m_poll);

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
			if (player->AudioState == 0)
				return true;

			pthread_rwlock_rdlock(&player->m_write);
			if (HI_MPI_SYNC_Stop(player->hSync, SYNC_CHAN_AUD) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to stop sync Audio.\n", __FUNCTION__);
			if (HI_MPI_AO_Track_Pause(player->hTrack) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to pause Audio.\n", __FUNCTION__);
				pthread_rwlock_unlock(&player->m_write);
				return false;
			}

			player->AudioState = 2;
			pthread_rwlock_unlock(&player->m_write);
		}
		break;
		case DEV_VIDEO:
			if (player->VideoState == 0)
				return true;

			pthread_rwlock_rdlock(&player->m_write);
			if (HI_MPI_SYNC_Stop(player->hSync, SYNC_CHAN_VID) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to stop sync Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Pause(player->hWindow, HI_TRUE) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to pause Video.\n", __FUNCTION__);
				pthread_rwlock_unlock(&player->m_write);
				return false;
			}
			if (HI_MPI_WIN_Freeze(player->hWindow, HI_TRUE, HI_DRV_WIN_SWITCH_LAST) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to freeze Video.\n", __FUNCTION__);

			player->VideoState = 2;
			pthread_rwlock_unlock(&player->m_write);
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

			pthread_rwlock_rdlock(&player->m_write);
			if (HI_MPI_SYNC_Start(player->hSync, SYNC_CHAN_AUD) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to start sync Audio.\n", __FUNCTION__);
			if (HI_MPI_AO_Track_Resume(player->hTrack) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to resume Audio.\n", __FUNCTION__);
				pthread_rwlock_unlock(&player->m_write);
				return false;
			}

			player->AudioState = 1;
			pthread_rwlock_unlock(&player->m_write);
		}
		break;
		case DEV_VIDEO:
			if (player->VideoState == 1)
				return true;
			else if (player->VideoState == 0)
				return false;

			pthread_rwlock_rdlock(&player->m_write);
			if (HI_MPI_SYNC_Start(player->hSync, SYNC_CHAN_VID) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to start sync Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Freeze(player->hWindow, HI_FALSE, HI_DRV_WIN_SWITCH_LAST) != HI_SUCCESS)
				printf("[ERROR] %s: Failed to unfreeze Video.\n", __FUNCTION__);
			if (HI_MPI_WIN_Pause(player->hWindow, HI_FALSE) != HI_SUCCESS)
			{
				printf("[ERROR] %s: Failed to resume Video.\n", __FUNCTION__);
				pthread_rwlock_unlock(&player->m_write);
				return false;
			}

			player->VideoState = 1;
			pthread_rwlock_unlock(&player->m_write);
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
			stStop.enMode = (player->IsBlank && !player->IsTimeShift) ? HI_UNF_AVPLAY_STOP_MODE_BLACK : HI_UNF_AVPLAY_STOP_MODE_STILL;

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

	if (HI_UNF_SND_SetMute(HI_UNF_SND_0, HI_UNF_SND_OUTPUTPORT_HDMI0, mute) != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to set mute.\n", __FUNCTION__);
		return false;
	}

	player->IsMute = mute;

	return true;
}

bool player_mixer(audio_mixer_t *mixer)
{
	printf("[INFO] %s: Mixer - VL(%d)/VR(%d).\n", __FUNCTION__, mixer->volume_left, mixer->volume_right);
	HI_UNF_SND_ABSGAIN_ATTR_S AbsWeightGain;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	AbsWeightGain.bLinearMode = HI_TRUE;
	AbsWeightGain.s32GainL = mixer->volume_left;
	AbsWeightGain.s32GainR = mixer->volume_right;

	if (HI_UNF_SND_SetTrackAbsWeight(player->hTrack, &AbsWeightGain) != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to set mixer.\n", __FUNCTION__);
		return false;
	}

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
	HI_UNF_TRACK_MODE_E tMode;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (channel)
	{
		case 0:
			tMode = HI_UNF_TRACK_MODE_STEREO;
		break;
		case 1:
			tMode = HI_UNF_TRACK_MODE_ONLY_LEFT;
		break;
		case 2:
			tMode = HI_UNF_TRACK_MODE_ONLY_RIGHT;
		break;
		case 3:
			tMode = HI_UNF_TRACK_MODE_DOUBLE_MONO;
		break;
		case 4:
			tMode = HI_UNF_TRACK_MODE_EXCHANGE;
		break;
		default:
			return false;
		break;
	}

	if (HI_UNF_SND_SetTrackChannelMode(player->hTrack, tMode) != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to set channel to %d.\n", __FUNCTION__, channel);
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
			HI_UNF_SND_ABSGAIN_ATTR_S AbsWeightGain;
			audio_status_t *status = (audio_status_t *)data;

			if (!status)
				return false;

			status->AV_sync_state	= player->IsSyncEnabled;
			status->mute_state		= player->IsMute;
			status->play_state		= player->AudioState;
			status->stream_source	= player->PlayerMode;
			status->channel_select	= player->AudioChannel;
			status->bypass_mode		= (player->AudioType != HA_AUDIO_ID_AAC && player->AudioType != 10);

			if (HI_UNF_SND_GetTrackAbsWeight(player->hTrack, &AbsWeightGain) == HI_SUCCESS)
			{
				if (AbsWeightGain.bLinearMode == HI_TRUE)
				{
					status->mixer_state.volume_left = AbsWeightGain.s32GainL;
					status->mixer_state.volume_right= AbsWeightGain.s32GainR;
				}
			}
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
	HI_UNF_AVPLAY_STATUS_INFO_S stStatusInfo;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	*pts = 0;
	if (HI_UNF_AVPLAY_GetStatusInfo(player->hPlayer, &stStatusInfo) == HI_SUCCESS)
	{
		long long AudioPTS = stStatusInfo.stSyncStatus.u32LastAudPts;
		long long VideoPTS = stStatusInfo.stSyncStatus.u32LastVidPts;

		switch (dev_type)
		{
			case DEV_AUDIO:
				if (AudioPTS != HI_INVALID_PTS)
					*pts = AudioPTS * 90;
			break;
			case DEV_VIDEO:
				if (VideoPTS != HI_INVALID_PTS)
					*pts = VideoPTS * 90;
			break;
		}
	}

	return (*pts != 0);
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
				case HI_UNF_AVPLAY_BUF_STATE_HIGH:
					if (condition)
						*reventsp |= (POLLOUT | POLLWRNORM);
				default:
					pthread_mutex_lock(&player->m_event);
					if (player->event_status != 0)
						*reventsp |= POLLPRI;
					pthread_mutex_unlock(&player->m_event);
				break;
			}
		break;
		case DEV_DVR:
		{
			HI_UNF_DMX_TSBUF_STATUS_S pStatus;

			if (HI_UNF_DMX_GetTSBufferStatus(player->hTsBuffer, &pStatus) == HI_SUCCESS)
			{
				if (pStatus.u32UsedSize <= 256 && !player->IsTimeShift)
					*reventsp = POLLIN;
			}

			pthread_mutex_unlock(&player->m_poll);
			return 0;
		}
		break;
	}

	if (ph && !(*reventsp & POLLOUT))
	{
		struct fuse_pollhandle *oldph = player->poll_handle[dev_type];

		if (oldph)
			fuse_pollhandle_destroy(oldph);

		player->poll_status |= (1 << dev_type);
		player->poll_handle[dev_type] = ph;
	}
	pthread_mutex_unlock(&player->m_poll);

	return 0;
}

int player_write(int dev_type, const char *buf, size_t size)
{
	bool IsHeader;
	unsigned char c_s;
	unsigned char c_e;
	HI_UNF_STREAM_BUF_S sBuf;
	HI_UNF_AVPLAY_BUFID_E bID;
	size_t pSize = size;
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
			bID = HI_UNF_AVPLAY_BUF_ID_ES_AUD;
		break;
		case DEV_VIDEO:
			c_s = VIDEO_STREAM_S;
			c_e = VIDEO_STREAM_E;
			bID = HI_UNF_AVPLAY_BUF_ID_ES_VID;
		break;
		case DEV_DVR:
		{
			HI_UNF_STREAM_BUF_S sBuf;
			HI_UNF_DMX_TSBUF_STATUS_S pStatus;

			if (HI_UNF_DMX_GetTSBufferStatus(player->hTsBuffer, &pStatus) != HI_SUCCESS ||
			   (pStatus.u32BufSize - pStatus.u32UsedSize) < size)
			{
				usleep(1000);
				return -EAGAIN;
			}

			pthread_rwlock_wrlock(&player->m_write);
			if (HI_UNF_DMX_GetTSBuffer(player->hTsBuffer, size, &sBuf, 1000) == HI_SUCCESS)
			{
				memcpy(sBuf.pu8Data, buf, size);
				if (HI_UNF_DMX_PutTSBuffer(player->hTsBuffer, size) != HI_SUCCESS)
				{
					printf("[ERROR] %s: Failed to put buffer.\n", __FUNCTION__);
					pthread_rwlock_unlock(&player->m_write);
					return -EAGAIN;
				}
			}
			pthread_rwlock_unlock(&player->m_write);

			return size;
		}
		break;
		case DEV_PAINEL:
		{
			if (size <= 6)
			{
				struct tm hour;
				int i, value = 0;
				HI_U8 DigDisCode[10]   = { 0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f };
				HI_U8 UCharDisCode[26] = {
					0x77, 0x7f, 0x39, 0x3f, 0x79, 0x71, 0x3d, 0x76, 0x06, 0x07, 0x36, 0x38, 0x4f,
					0x37, 0x3f, 0x73, 0xff, 0x77, 0x6d, 0x31, 0x3e, 0x3e, 0x4f, 0x76, 0x72, 0x5b
				};
				HI_U8 LCharDisCode[26] = {
					0x5c, 0x7c, 0x58, 0x5e, 0x7b, 0x71, 0x6f, 0x74, 0x05, 0x0d, 0x70, 0x30, 0x4f,
					0x54, 0x5c, 0x73, 0x67, 0x50, 0x6d, 0x78, 0x1c, 0x1c, 0x4f, 0x76, 0x6e, 0x5b
				};

				if (!strncmp(buf, "....", 4) || !strncmp(buf, "----", 4)) {
					value = 0x0; /* Clear panel */
				} else if (strptime(buf, "%H:%M", &hour) != NULL) {
					HI_UNF_KEYLED_TIME_S stTime;
					stTime.u32Hour = hour.tm_hour;
					stTime.u32Minute = hour.tm_min;

					if (HI_UNF_LED_DisplayTime(stTime) != HI_SUCCESS)
						printf("[ERROR] %s: Time not writed to keyled.\n", __FUNCTION__);

					return size;
				} else if (!strncmp(buf, "TIME", 4)) {
					player_showtime();
					return size;
				} else {
					for (i = 0; i < 4; i++) {
						if (buf[i] >= '0' && buf[i] <= '9')
							value |= DigDisCode[buf[i] - 48] << (8 * i);
						else if (buf[i] >= 'A' && buf[i] <= 'Z')
							value |= UCharDisCode[buf[i] - 65] << (8 * i);
						else
							value |= LCharDisCode[buf[i] - 97] << (8 * i);
					}
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

	/* Extract PTS from header. */
	if (IsHeader)
	{
		if ((buf[7] & 0x80) >> 7) /* PTS */
		{
			HI_U32 PTSLow = 0;

			PTSLow = (((buf[9] & 0x06) >> 1) << 30)
					| ((buf[10]) << 22)
					| (((buf[11] & 0xfe) >> 1) << 15)
					| ((buf[12]) << 7)
					| (((buf[13] & 0xfe)) >> 1);

			if (buf[9] & 0x08)
				player->p2e[dev_type].pts = (PTSLow / 90) + 0x2D82D82; //(1 << 32) / 90;
			else
				player->p2e[dev_type].pts = (PTSLow / 90);
		}
		else
			player->p2e[dev_type].pts = 0;

		/* Not need write Header for Audio but need keep for now. */
		if (dev_type == DEV_AUDIO)
		{
			/* Check ES size in PES package.
			 * ES Size = (PES Size - PES Header Size)
			 *
			 * Sometimes the Header contains ES data too.
			 * And because of this, is necessary save header for check in next step.
			 */
			player->p2e[dev_type].es_size = (((buf[4]<<8) | buf[5]) + 6) - (9 + buf[8]);
			if (player->p2e[dev_type].es_size < 0)
				player->p2e[dev_type].es_size = 0; /* The ES size is big, and not have data in header. */

			/* Save header for use when receive data. */
			memcpy(&player->p2e[dev_type].header, buf, size);
			player->p2e[dev_type].size = size;

			return size;
		}
	}
	else if (dev_type == DEV_AUDIO)
	{
		if (size < player->p2e[dev_type].es_size && player->p2e[dev_type].size > 0)
		{
			IsHeader = true;
			pSize = player->p2e[dev_type].es_size;
		}
	}

	pthread_rwlock_wrlock(&player->m_write);
	if (HI_UNF_AVPLAY_GetBuf(player->hPlayer, bID, pSize, &sBuf, 0) != HI_SUCCESS)
	{
		usleep(1000);
		pthread_rwlock_unlock(&player->m_write);
		return 0;
	}

	if (dev_type == DEV_AUDIO && IsHeader)
	{
		/** ((Header + Data) - ES) = Get the init position of ES Data **/
		size_t start= (player->p2e[dev_type].size + size) - player->p2e[dev_type].es_size;
		/** Header - start = Get the ES Data Size in Header **/
		size_t length = player->p2e[dev_type].size - start;

		memcpy(sBuf.pu8Data, &player->p2e[dev_type].header[start], length);
		memcpy(&sBuf.pu8Data[length], buf, size);
	}
	else
		memcpy(sBuf.pu8Data, buf, size);

	if (HI_UNF_AVPLAY_PutBuf(player->hPlayer, bID, pSize, player->p2e[dev_type].pts) != HI_SUCCESS)
	{
		printf("[ERROR] %s: Failed to put buffer for device type %d.\n", __FUNCTION__, dev_type);
		usleep(1000);
		pthread_rwlock_unlock(&player->m_write);
		return 0;
	}

	if (dev_type == DEV_AUDIO && IsHeader)
	{
		player->p2e[dev_type].size    = 0;
		player->p2e[dev_type].es_size = 0;
	}
	pthread_rwlock_unlock(&player->m_write);

	return size;
}

struct class_ops *player_get_ops(void)
{
	player_ops.create			= player_create;
	player_ops.destroy			= player_destroy;
	player_ops.clear			= player_clear;
	player_ops.set_dvr			= player_set_dvr;
	player_ops.set_type			= player_set_type;
	player_ops.set_pid			= player_set_pid;
	player_ops.set_mode			= player_set_mode;
	player_ops.set_blank		= player_set_blank;
	player_ops.set_format		= player_set_format;
	player_ops.set_disp_format	= player_set_display_format;
	player_ops.set_fastfoward	= player_set_fastfoward;
	player_ops.set_slowmotion	= player_set_slowmotion;
	player_ops.play				= player_play;
	player_ops.pause			= player_pause;
	player_ops.resume			= player_resume;
	player_ops.stop				= player_stop;
	player_ops.mute				= player_mute;
	player_ops.mixer			= player_mixer;
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
