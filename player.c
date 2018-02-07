#include <AVServer.h>

struct class_ops player_ops;

struct s_player {
	bool IsCreated;
	int TunerID;
	int PlayerMode;
	int AudioPid;
	int AudioType;
	int AudioChannel;
	int AudioState;
	int VideoPid;
	int VideoType;
	int VideoState;
	int VideoFormat;
	int DisplayFormat;
	long long LastPTS;

	bool IsBlank;
	bool IsSyncEnabled;
	bool IsMute;

	unsigned int hPlayer;
	unsigned int hWindow;
	unsigned int hTrack;
};

bool player_create(void)
{
	struct s_player *player = calloc(1, sizeof(struct s_player));

	if (!player)
		return false;

	player->IsCreated		= true;
	player->PlayerMode		= 0; /* 0 demux, 1 memory */
	player->AudioPid		= 0x1FFFF; /* unknown pid */
	player->AudioType		= 0;
	player->AudioChannel	= 0; /* 0 stereo, 1 left, 2 right */
	player->AudioState		= 0; /* 0 stoped, 1 playing, 2 paused */
	player->VideoPid		= 0x1FFFF; /* unknown pid */
	player->VideoType		= 0;
	player->VideoState		= 0; /* 0 stoped, 1 playing, 2 freezed */
	player->VideoFormat		= 1; /* 0 4:3, 1 16:9, 2 2.21:1 */
	player->DisplayFormat	= 0; /* 0 Pan&Scan, 1 Letterbox, 2 Center Cut Out */
	player->IsBlank			= true;
	player->IsSyncEnabled	= true;
	player->IsMute			= false;

	player_ops.priv = player;
	return true;
}

void player_destroy(void)
{
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (player && player->IsCreated)
	{
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

	return true;
}

bool player_set_pid(int dev_type, int pid)
{
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, pid);
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
			player->AudioPid = pid;
		break;
		case DEV_VIDEO:
			player->VideoPid = pid;
		break;
		default:
			printf("[ERROR] %s(%d, %d) -> Wrong device type.\n", __FUNCTION__, dev_type, pid);
		break;
	}

	return true;
}

bool player_set_mode(int mode)
{
	printf("[INFO] %s(%d) -> called.\n", __FUNCTION__, mode);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
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

bool player_stop(int dev_type, int mode)
{
	printf("[INFO] %s(%d, %d) -> called.\n", __FUNCTION__, dev_type, mode);
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	switch (dev_type)
	{
		case DEV_AUDIO:
			player->AudioState = 0;
		break;
		case DEV_VIDEO:
			player->VideoState = 0;
		break;
		default:
			return false;
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
	struct s_player *player = (struct s_player *)player_ops.priv;

	if (!(player && player->IsCreated))
	{
		printf("[ERROR] %s -> The Player it's not created.\n", __FUNCTION__);
		return false;
	}

	player->IsSyncEnabled = sync;

	return true;
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