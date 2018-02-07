#include <AVServer.h>
#include <hi_adp_mpi.h>
#include <hi_common.h>
#include <hi_unf_avplay.h>
#include <hi_unf_descrambler.h>
#include <hi_unf_demux.h>
#include <hi_unf_ecs.h>
#include <hi_unf_keyled.h>
#include <hi_unf_sound.h>
#include <hi_unf_vo.h>
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
	int TunerID;
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
	long long LastPTS;

	bool IsBlank;
	bool IsSyncEnabled;
	bool IsMute;

	unsigned int hPlayer;
	unsigned int hWindow;
	unsigned int hTrack;
};

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

	HIADP_AVPlay_SetAdecAttr(player->hPlayer, HA_AUDIO_ID_MP3, HD_DEC_MODE_RAWPCM, 0);
	HIADP_AVPlay_SetVdecAttr(player->hPlayer, HI_UNF_VCODEC_TYPE_MPEG2, HI_UNF_VCODEC_MODE_NORMAL);

	HI_UNF_AVPLAY_GetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr);
	stSyncAttr.enSyncRef = HI_UNF_SYNC_REF_AUDIO;
	if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_SYNC, &stSyncAttr) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> HI_UNF_AVPLAY_SetAttr failed.\n", __FUNCTION__);
		goto SND_DETACH;
	}

	HI_UNF_DISP_SetVirtualScreen(HI_UNF_DISPLAY1, 1920, 1080);

	player->IsCreated		= true;
	player->PlayerMode		= 0;
	player->AudioPid		= 0x1FFFF;
	player->AudioType		= 10; /* MP3 */
	player->AudioChannel	= 0;
	player->AudioState		= 0;
	player->VideoPid		= 0x1FFFF;
	player->VideoType		= 1; /* H264 */
	player->VideoState		= 0;
	player->VideoFormat		= 1;
	player->DisplayFormat	= 0;
	player->IsBlank			= true;
	player->IsSyncEnabled	= true;
	player->IsMute			= false;

	player_ops.priv = player;
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
		printf("[INFO] %s shit.\n", __FUNCTION__);
		return;
		//hi_player_audio_stop();
		//hi_player_video_stop(HI_UNF_AVPLAY_STOP_MODE_BLACK);

		HI_UNF_SND_Detach(player->hTrack, player->hPlayer);
		HI_UNF_SND_DestroyTrack(player->hTrack);

		HI_UNF_VO_SetWindowEnable(player->hWindow, HI_FALSE);
		HI_UNF_VO_DetachWindow(player->hWindow, player->hPlayer);

		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_AUD);
		HI_UNF_AVPLAY_ChnClose(player->hPlayer, HI_UNF_AVPLAY_MEDIA_CHAN_VID);

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

			if (player->AudioType == type)
				return true;
			else if (player->AudioState == 1)
			{
				printf("[ERROR] %s -> Only can change Audio Type if is in STOPED / PAUSED state.\n", __FUNCTION__);
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

			if (HIADP_AVPlay_SetAdecAttr(player->hPlayer, htype, HD_DEC_MODE_RAWPCM, 0) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set Audio Type %d.\n", __FUNCTION__, type);
				return false;
			}

			player->AudioType = type;
		}
		break;
		case DEV_VIDEO:
		{
			HI_UNF_VCODEC_TYPE_E htype;

			if (player->VideoType == type)
				return true;
			else if (player->VideoState == 1)
			{
				printf("[ERROR] %s -> Only can change Video Type if is in STOPED / PAUSED state.\n", __FUNCTION__);
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

			if (HIADP_AVPlay_SetVdecAttr(player->hPlayer, htype, HI_UNF_VCODEC_MODE_NORMAL) != HI_SUCCESS)
			{
				printf("[ERROR] %s -> Failed to set Video Type %d.\n", __FUNCTION__, type);
				return false;
			}

			player->VideoType = type;
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
	HI_UNF_DMX_PORT_E enToPortId;
	HI_UNF_DMX_PORT_E enFromPortId;
	HI_UNF_AVPLAY_ATTR_S stAvplayAttr;
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}
	else if (HI_UNF_AVPLAY_GetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_STREAM_MODE, &stAvplayAttr) != HI_SUCCESS)
		return false;

	enToPortId = (mode != 0 /* DEMUX */ ? HI_UNF_DMX_PORT_RAM_0 : (HI_UNF_DMX_PORT_TSI_0 + player->TunerID));
	if (HI_UNF_DMX_GetTSPortId(stAvplayAttr.u32DemuxId, &enFromPortId) == HI_SUCCESS)
	{
		if (enFromPortId == enToPortId)
			return true;
		else if (HI_UNF_DMX_DetachTSPort(stAvplayAttr.u32DemuxId) != HI_SUCCESS)
			return false;
	}

	if (HI_UNF_DMX_AttachTSPort(stAvplayAttr.u32DemuxId, enToPortId) != HI_SUCCESS)
	{
		printf("[ERROR] %s -> Failed to set Mode %d.\n", __FUNCTION__, mode);
		return false;
	}

	if (stAvplayAttr.stStreamAttr.enStreamType != (mode != 0 ? HI_UNF_AVPLAY_STREAM_TYPE_ES : HI_UNF_AVPLAY_STREAM_TYPE_TS))
	{
		stAvplayAttr.stStreamAttr.enStreamType = (mode != 0 ? HI_UNF_AVPLAY_STREAM_TYPE_ES : HI_UNF_AVPLAY_STREAM_TYPE_TS);

		if (HI_UNF_AVPLAY_SetAttr(player->hPlayer, HI_UNF_AVPLAY_ATTR_ID_STREAM_MODE, &stAvplayAttr) != HI_SUCCESS)
			printf("[ERROR] %s -> Failed to change Stream Mode.\n", __FUNCTION__);
	}

	/*switch (dev_type)
	{
		case DEV_AUDIO:
			if (player->AudioState == 1)
			{
				printf("[ERROR] %s -> Only can change Audio Mode if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}
		break;
		case DEV_VIDEO:
			if (player->VideoState == 1)
			{
				printf("[ERROR] %s -> Only can change Video Mode if is in STOPED / PAUSED state.\n", __FUNCTION__);
				return false;
			}
		break;
	}*/

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

	switch (dev_type)
	{
		case DEV_AUDIO:
			player->AudioState = 2;
		break;
		case DEV_VIDEO:
			player->VideoState = 2;
		break;
		default:
			return false;
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

	switch (dev_type)
	{
		case DEV_AUDIO:
			player->AudioState = 1;
		break;
		case DEV_VIDEO:
			player->VideoState = 1;
		break;
		default:
			return false;
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
			struct audio_status *status = (struct audio_status *)data;

			if (!status)
				return false;

			status->AV_sync_state	= player->IsSyncEnabled;
			status->mute_state		= player->IsMute;
			status->play_state		= player->AudioState;
			status->stream_source	= player->PlayerMode;
			status->channel_select	= player->AudioChannel;
			status->bypass_mode		= (player->AudioType != 1 && player->AudioType != 10);
			//status->mixer_state;
			printf("[WARNING] %s -> Mixer state not implemented.\n", __FUNCTION__);
		}
		break;
		case DEV_VIDEO:
		{
			struct video_status *status = (struct video_status *)data;

			if (!status)
				return false;

			status->video_blank		= player->IsBlank;		/* blank video on freeze? */
			status->play_state		= player->VideoState;	/* current state of playback */
			status->stream_source	= player->PlayerMode;	/* current source (demux/memory) */
			status->video_format	= player->VideoFormat;	/* current aspect ratio of stream */
			status->display_format	= player->DisplayFormat;/* selected cropping mode */
		}
		break;
		default:
			return false;
		break;
	}

	return true;
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

	return true;
}

bool player_get_framerate(int *framerate)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	return true;
}

bool player_get_progressive(int *progressive)
{
	printf("[INFO] %s() -> called.\n", __FUNCTION__);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	return true;
}

bool player_get_pts(int dev_type, long long *pts)
{
	printf("[INFO] %s(%d, PTS) -> called.\n", __FUNCTION__, dev_type);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	return true;
}

struct class_ops *player_get_ops(void)
{
	player_ops.create			= player_create;
	player_ops.destroy			= player_destroy;
	player_ops.set_type			= player_set_type;
	player_ops.set_pid			= player_set_pid;
	player_ops.set_mode			= player_set_mode;
	player_ops.set_blank		= player_set_blank;
	player_ops.set_format		= player_set_format;
	player_ops.set_disp_format	= player_set_display_format;
    player_ops.play				= player_play;
	player_ops.pause			= player_pause;
	player_ops.resume			= player_resume;
	player_ops.stop				= player_stop;
	player_ops.mute				= player_mute;
    player_ops.sync				= player_sync;
	player_ops.channel			= player_channel;
	player_ops.status			= player_get_status;
	player_ops.get_vsize		= player_get_vsize;
	player_ops.get_framerate	= player_get_framerate;
	player_ops.get_progressive	= player_get_progressive;
	player_ops.get_pts			= player_get_pts;

	return &player_ops;
}