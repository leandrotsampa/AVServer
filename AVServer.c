/*
  FUSE fioc: FUSE ioctl example
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

/** @file
 * @tableofcontents
 *
 * This example illustrates how to write a FUSE file system that can
 * process (a restricted set of) ioctls. It can be tested with the
 * ioctl_client.c program.
 *
 * Compile with:
 *
 *     arm-linux-gnueabi-gcc -Wall -D_FILE_OFFSET_BITS=64 -I./include AVServer.c -L../lib/.libs -lfuse3 -o AVServer
 *
 * Compile with:
 *
 *     ./AVServer -f -o auto_unmount /tmp/AVServer
 *
 * ## Source code ##
 * \include AVServer.c
 */

#define FUSE_USE_VERSION 30

#include <config.h>

#include <fuse.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <poll.h>
#include <AVServer.h>

#define AUDIO_DEV	"audio0"
#define VIDEO_DEV	"video0"
#define DVR_DEV		"dvr0"

enum {
	DVB_NONE,
	DVB_ROOT,
	DVB_FILE,
	DVB_AUDIO_DEV,
	DVB_VIDEO_DEV,
	DVB_DVR_DEV,
};

static unsigned dvb_hisi_open_mask;

/* Mutex for Player */
static pthread_mutex_t m_audio;
static pthread_mutex_t m_video;

static int dvb_hisi_file_type(const char *path)
{
	if (strcmp(path, "/") == 0)
		return DVB_ROOT;
	else if (strcmp(path, "/" AUDIO_DEV) == 0)
		return DVB_AUDIO_DEV;
	else if (strcmp(path, "/" VIDEO_DEV) == 0)
		return DVB_VIDEO_DEV;
	else if (strcmp(path, "/" DVR_DEV) == 0)
		return DVB_DVR_DEV;

	return DVB_NONE;
}

static void *dvb_hisi_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
	printf("%s -> Called.\n", __FUNCTION__);
	struct class_ops *player = player_get_ops();

	if (player && player->create())
		return player;

	return NULL;
}

static void dvb_hisi_destroy(void *private_data)
{
	printf("%s -> Called.\n", __FUNCTION__);
	struct class_ops *player = (struct class_ops *)private_data;

	if (player)
		player->destroy();
}

static int dvb_hisi_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi)
{
	printf("%s -> Called.\n", __FUNCTION__);

	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_atime = stbuf->st_mtime = time(NULL);

	switch (dvb_hisi_file_type(path))
	{
		case DVB_ROOT:
			stbuf->st_mode  = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
		break;
		case DVB_FILE:
		case DVB_AUDIO_DEV:
		case DVB_VIDEO_DEV:
		case DVB_DVR_DEV:
			stbuf->st_mode  = S_IFREG | 0644;
			stbuf->st_size  = 0;
			stbuf->st_nlink = 1;
		break;
		case DVB_NONE:
			return -ENOENT;
	}

	return 0;
}

static int dvb_hisi_open(const char *path, struct fuse_file_info *fi)
{
	int type = dvb_hisi_file_type(path);
	//struct fuse_context *cxt = fuse_get_context();
	//struct class_ops *player = (struct class_ops *)cxt->private_data;

	printf("%s -> type is %d.\n", __FUNCTION__, type);

	if (type == DVB_NONE)
		return -ENOENT;
	if ((fi->flags & O_ACCMODE) != O_RDONLY)
	{
		/* Lock for only 1 access. */
		switch (type)
		{
			case DVB_AUDIO_DEV:
			case DVB_VIDEO_DEV:
			case DVB_DVR_DEV:
				if (dvb_hisi_open_mask & (1 << type))
					return -EBUSY;

				dvb_hisi_open_mask |= (1 << type);

				/* fsel files are nonseekable somewhat pipe-like files which
				 * gets filled up periodically by producer thread and consumed
				 * on read.  Tell FUSE as such.
				 */
				fi->fh = type;
				fi->direct_io = 1;
				fi->nonseekable = 1;
				/* Make cache persistent even if file is closed,
				 *  this makes it easier to see the effects.
				 */
				fi->keep_cache = 1;
			break;
			default:
			break;
		}
		//dvb_ringbuffer_flush_spinlock_wakeup(&av7110->aout);
		//dvb_ringbuffer_flush_spinlock_wakeup(&av7110->avout);
		//av7110->video_blank = 1;
		//av7110->audiostate.AV_sync_state = 1;
		//av7110->videostate.stream_source = VIDEO_SOURCE_DEMUX;

		/*  empty event queue */
		//av7110->video_events.eventr = av7110->video_events.eventw = 0;
	}

	return 0;
}

static int dvb_hisi_release(const char *path, struct fuse_file_info *fi)
{
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	printf("%s -> type is %d.\n", __FUNCTION__, type);

	if ((fi->flags & O_ACCMODE) != O_RDONLY)
	{
		/* Release for access. */
		switch (type)
		{
			case DVB_AUDIO_DEV:
			case DVB_VIDEO_DEV:
			case DVB_DVR_DEV:
				dvb_hisi_open_mask &= ~(1 << type);

				if (type == DVB_AUDIO_DEV && player)
					player->stop(DEV_AUDIO);
				else if (type == DVB_VIDEO_DEV && player)
					player->stop(DEV_VIDEO);
			break;
			default:
			break;
		}
	}

	return 0;
}

static int dvb_hisi_do_read(char *buf, size_t size, off_t offset)
{
	printf("%s -> Called.\n", __FUNCTION__);

	/*if (offset >= dvb_hisi_size)
		return 0;

	if (size > dvb_hisi_size - offset)
		size = dvb_hisi_size - offset;

	memcpy(buf, dvb_hisi_buf + offset, size);*/
	return 0;
}

static int dvb_hisi_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	printf("%s -> Called.\n", __FUNCTION__);
	//struct fuse_context *cxt = fuse_get_context();
	//struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (dvb_hisi_file_type(path) < DVB_FILE)
		return -EINVAL;

	return dvb_hisi_do_read(buf, size, offset);
}

static int dvb_hisi_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int ret = -EINVAL;
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (!player)
		return -EINVAL;

	switch (type)
	{
		case DVB_AUDIO_DEV:
			pthread_mutex_lock(&m_audio);
			ret = player->write(DEV_AUDIO, buf, size);
			pthread_mutex_unlock(&m_audio);
		break;
		case DVB_VIDEO_DEV:
			pthread_mutex_lock(&m_video);
			ret = player->write(DEV_VIDEO, buf, size);
			pthread_mutex_unlock(&m_video);
		break;
		case DVB_DVR_DEV:
			printf("[NOTICE] %s: The DVR device it's not implemented yet.", __FUNCTION__);
		break;
	}

	return ret;
}

static int dvb_hisi_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	printf("%s -> Called.\n", __FUNCTION__);

	if (dvb_hisi_file_type(path) != DVB_ROOT)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, AUDIO_DEV, NULL, 0, 0);
	filler(buf, VIDEO_DEV, NULL, 0, 0);
	filler(buf, DVR_DEV, NULL, 0, 0);

	return 0;
}

static int dvb_hisi_ioctl(const char *path, int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags, void *data)
{
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	switch (type)
	{
		case DVB_AUDIO_DEV:
		case DVB_VIDEO_DEV:
			if (!player)
				return -EINVAL;
			if (!(dvb_hisi_open_mask & (1 << type)))
			{
				switch (cmd)
				{
					case AUDIO_GET_STATUS:
					case VIDEO_GET_STATUS:
					case VIDEO_GET_EVENT:
					case VIDEO_GET_SIZE:
					break;
					default:
						return -EPERM;
					break;
				}
			}
		break;
		default:
			return -EINVAL;
		break;
	}

	switch (cmd)
	{
		/* AUDIO IOCTL
		 */
		case AUDIO_STOP:
			printf("%s: AUDIO_STOP\n", __FUNCTION__);

			return player->stop(DEV_AUDIO) - 1;
		break;
		case AUDIO_PLAY:
			printf("%s: AUDIO_PLAY\n", __FUNCTION__);

			if (arg)
				player->set_pid(DEV_AUDIO, (int)(intptr_t)arg);

			return player->play(DEV_AUDIO) - 1;
		break;
		case AUDIO_PAUSE:
			printf("%s: AUDIO_PAUSE\n", __FUNCTION__);

			return player->pause(DEV_AUDIO) - 1;
		break;
		case AUDIO_CONTINUE:
			printf("%s: AUDIO_CONTINUE\n", __FUNCTION__);

			return player->resume(DEV_AUDIO) - 1;
		break;
		case AUDIO_SELECT_SOURCE:
			printf("%s: AUDIO_SELECT_SOURCE\n", __FUNCTION__);

			return player->set_mode((int)(intptr_t)arg) - 1;
		break;
		case AUDIO_SET_MUTE:
			printf("%s: AUDIO_SET_MUTE\n", __FUNCTION__);

			return player->mute((int)(intptr_t)arg) - 1;
		break;
		case AUDIO_SET_AV_SYNC:
			printf("%s: AUDIO_SET_AV_SYNC\n", __FUNCTION__);

			return player->sync((int)(intptr_t)arg) - 1;
		break;
		case AUDIO_SET_BYPASS_MODE:
			printf("%s: AUDIO_SET_BYPASS_MODE\n", __FUNCTION__);

			return player->set_type(DEV_AUDIO, (int)(intptr_t)arg) - 1;
		break;
		case AUDIO_CHANNEL_SELECT:
			printf("%s: AUDIO_CHANNEL_SELECT\n", __FUNCTION__);

			return player->channel((int)(intptr_t)arg) - 1;
		break;
		case AUDIO_GET_STATUS:
			printf("%s: AUDIO_GET_STATUS\n", __FUNCTION__);

			return player->status(DEV_AUDIO, (struct audio_status *)data) - 1;
		break;
		case AUDIO_GET_CAPABILITIES:
			printf("%s: AUDIO_GET_CAPABILITIES\n", __FUNCTION__);

			*(unsigned int *)arg = AUDIO_CAP_LPCM | AUDIO_CAP_DTS | AUDIO_CAP_AC3 | AUDIO_CAP_MP1 | AUDIO_CAP_MP2;
		break;
		case AUDIO_CLEAR_BUFFER:
			printf("%s: AUDIO_CLEAR_BUFFER\n", __FUNCTION__);
		break;
		case AUDIO_SET_ID:
			printf("%s: AUDIO_SET_ID\n", __FUNCTION__);
		break;
		case AUDIO_SET_MIXER:
			printf("%s: AUDIO_SET_MIXER\n", __FUNCTION__);
			//struct audio_mixer *amix = (struct audio_mixer *)parg;
		break;
		case AUDIO_SET_STREAMTYPE:
			printf("%s: AUDIO_SET_STREAMTYPE\n", __FUNCTION__);
		break;
		case AUDIO_GET_PTS:
			printf("%s: AUDIO_GET_PTS\n", __FUNCTION__);

			return player->get_pts(DEV_AUDIO, (long long *)arg) - 1;
		break;


		/* VIDEO IOCTL
		 */
		case VIDEO_STOP:
			printf("%s: VIDEO_STOP\n", __FUNCTION__);

			return player->stop(DEV_VIDEO) - 1;
		break;
		case VIDEO_PLAY:
			printf("%s: VIDEO_PLAY\n", __FUNCTION__);

			if (arg)
				player->set_pid(DEV_VIDEO, (int)(intptr_t)arg);

			return player->play(DEV_VIDEO) - 1;
		break;
		case VIDEO_FREEZE:
			printf("%s: VIDEO_FREEZE\n", __FUNCTION__);

			return player->pause(DEV_VIDEO) - 1;
		break;
		case VIDEO_CONTINUE:
			printf("%s: VIDEO_CONTINUE\n", __FUNCTION__);

			return player->resume(DEV_VIDEO) - 1;
		break;
		case VIDEO_SELECT_SOURCE:
			printf("%s: VIDEO_SELECT_SOURCE\n", __FUNCTION__);

			return player->set_mode((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_SET_BLANK:
			printf("%s: VIDEO_SET_BLANK\n", __FUNCTION__);

			return player->set_blank((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_GET_STATUS:
			printf("%s: VIDEO_GET_STATUS\n", __FUNCTION__);
			//trick_dlen = sizeof(struct video_status);

			return player->status(DEV_VIDEO, (struct video_status *)data) - 1;
		break;
		case VIDEO_GET_EVENT:
			printf("%s: VIDEO_GET_EVENT\n", __FUNCTION__);

			return player->get_event((struct video_event *)data) - 1;
		break;
		case VIDEO_GET_SIZE:
			printf("%s: VIDEO_GET_SIZE\n", __FUNCTION__);

			return player->get_vsize((video_size_t *)arg) - 1;
		break;
		case VIDEO_SET_DISPLAY_FORMAT:
			printf("%s: VIDEO_SET_DISPLAY_FORMAT\n", __FUNCTION__);

			return player->set_disp_format((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_SET_FORMAT:
			printf("%s: VIDEO_SET_FORMAT\n", __FUNCTION__);

			return player->set_format((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_STILLPICTURE:
			printf("%s: VIDEO_STILLPICTURE\n", __FUNCTION__);
		break;
		case VIDEO_FAST_FORWARD:
			printf("%s: VIDEO_FAST_FORWARD\n", __FUNCTION__);
		break;
		case VIDEO_SLOWMOTION:
			printf("%s: VIDEO_SLOWMOTION\n", __FUNCTION__);
		break;
		case VIDEO_GET_CAPABILITIES:
			printf("%s: VIDEO_GET_CAPABILITIES\n", __FUNCTION__);

			*(int *)arg = VIDEO_CAP_MPEG1 | VIDEO_CAP_MPEG2 | VIDEO_CAP_SYS | VIDEO_CAP_PROG;
		break;
		case VIDEO_CLEAR_BUFFER:
			printf("%s: VIDEO_CLEAR_BUFFER\n", __FUNCTION__);
		break;
		case VIDEO_SET_STREAMTYPE:
			printf("%s: VIDEO_SET_STREAMTYPE\n", __FUNCTION__);

			return player->set_type(DEV_VIDEO, (int)(intptr_t)arg) - 1;
		break;
		case VIDEO_GET_FRAME_RATE:
			printf("%s: VIDEO_GET_FRAME_RATE\n", __FUNCTION__);

			return player->get_framerate((int *)arg) - 1;
		break;
		case VIDEO_GET_PTS:
			printf("%s: VIDEO_GET_PTS\n", __FUNCTION__);

			return player->get_pts(DEV_VIDEO, (long long *)arg) - 1;
		break;
		default:
			printf("[INFO] %s: UNKNOWN %u\n", __FUNCTION__, cmd);

			return -EINVAL;
		break;
	}

	return 0;
}

static int dvb_hisi_poll(const char *path, struct fuse_file_info *fi, struct fuse_pollhandle *ph, unsigned *reventsp)
{
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (!player)
	{
		*reventsp = -EINVAL;
		return 0;
	}
	else
		*reventsp = 0;

	switch (type)
	{
		case DVB_AUDIO_DEV:
			pthread_mutex_lock(&m_audio);
			*reventsp |= (POLLOUT | POLLWRNORM);
			pthread_mutex_unlock(&m_audio);
		case DVB_VIDEO_DEV:
			if (player->have_event())
				*reventsp = POLLPRI;

			if (dvb_hisi_open_mask & (1 << type))
			{
				pthread_mutex_lock(&m_video);
				*reventsp |= (POLLOUT | POLLWRNORM);
				pthread_mutex_unlock(&m_video);
			}
		break;
		default:
			*reventsp = -EINVAL;
		break;
	}

	return 0;
}

static struct fuse_operations dvb_hisi_oper = {
	.init		= dvb_hisi_init,
	.destroy	= dvb_hisi_destroy,
	.getattr	= dvb_hisi_getattr,
	.readdir	= dvb_hisi_readdir,
	.open		= dvb_hisi_open,
	.release	= dvb_hisi_release,
	.read		= dvb_hisi_read,
	.write		= dvb_hisi_write,
	.ioctl		= dvb_hisi_ioctl,
	.poll		= dvb_hisi_poll,
};

int main(int argc, char *argv[])
{
	pthread_mutex_init(&m_audio, NULL);
	pthread_mutex_init(&m_video, NULL);

	return fuse_main(argc, argv, &dvb_hisi_oper, "Leandro" /*NULL*/);
}
