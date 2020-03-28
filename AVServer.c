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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <version.h>
#include <AVServer.h>

#define AUDIO_DEV	"audio0"
#define VIDEO_DEV	"video0"
#define DVR_DEV		"dvr"
#define PAINEL_DEV	"panel"

#define VIDEO_SET_FRAME_RATE _IO('o', 99)

static unsigned dvb_hisi_open_mask;

enum {
	DVB_NONE,
	DVB_ROOT,
	DVB_FILE,
	DVB_AUDIO_DEV,
	DVB_VIDEO_DEV,
	DVB_DVR_DEV,
	DVB_PAINEL_DEV,
};

static int dvb_hisi_file_type(const char *path) {
	if (strEquals((char*)path, "/", false))
		return DVB_ROOT;
	else if (strEquals((char*)path, "/" AUDIO_DEV, false))
		return DVB_AUDIO_DEV;
	else if (strEquals((char*)path, "/" VIDEO_DEV, false))
		return DVB_VIDEO_DEV;
	else if (strEquals((char*)path, "/" DVR_DEV, true))
		return DVB_DVR_DEV;
	else if (strEquals((char*)path, "/" PAINEL_DEV, false))
		return DVB_PAINEL_DEV;

	return DVB_NONE;
}

static void *dvb_hisi_init(struct fuse_conn_info *conn, struct fuse_config *cfg) {
	(void) conn;
	(void) cfg;
	struct class_ops *player = player_get_ops();

	printf("%s -> Called.\n", __FUNCTION__);
	if (player && player->create())
		return player;

	return NULL;
}

static void dvb_hisi_destroy(void *private_data) {
	struct class_ops *player = (struct class_ops *)private_data;

	printf("%s -> Called.\n", __FUNCTION__);
	if (player)
		player->destroy();
}

static int dvb_hisi_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
	(void) fi;
	stbuf->st_uid = getuid();
	stbuf->st_gid = getgid();
	stbuf->st_atime = stbuf->st_mtime = time(NULL);

	switch (dvb_hisi_file_type(path)) {
		case DVB_ROOT:
			stbuf->st_mode  = S_IFDIR | 0755;
			stbuf->st_nlink = 2;
		break;
		case DVB_FILE:
		case DVB_AUDIO_DEV:
		case DVB_VIDEO_DEV:
		case DVB_DVR_DEV:
		case DVB_PAINEL_DEV:
			stbuf->st_mode  = S_IFREG | 0660;
			stbuf->st_size  = 0;
			stbuf->st_nlink = 1;
		break;
		case DVB_NONE:
			return -ENOENT;
	}

	return 0;
}

static int dvb_hisi_open(const char *path, struct fuse_file_info *fi) {
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (type == DVB_NONE)
		return -ENOENT;
	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		/* Lock for only 1 access. */
		switch (type) {
			case DVB_AUDIO_DEV:
			case DVB_VIDEO_DEV:
			case DVB_DVR_DEV:
			case DVB_PAINEL_DEV:
				if (dvb_hisi_open_mask & (1 << type))
					return -EBUSY;

				dvb_hisi_open_mask |= (1 << type);

				/* fsel files are nonseekable somewhat pipe-like files which
				 * gets filled up periodically by producer thread and consumed
				 * on read.  Tell FUSE as such.
				 */
				if (type == DVB_DVR_DEV) {
					char filename[32];
					snprintf(filename, sizeof(filename), "/dev/dvb/adapter%c/dvr0", path[(strlen(path)-1)]);
					fi->fh = open(filename, O_WRONLY);
					player->set_dvr(true);
				}
			break;
			default:
			break;
		}
	}

	fi->direct_io  = 1;
	fi->keep_cache = 0;
	fi->flush      = 0;
	fi->nonseekable= 1;

	return 0;
}

static int dvb_hisi_release(const char *path, struct fuse_file_info *fi) {
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if ((fi->flags & O_ACCMODE) != O_RDONLY) {
		/* Release for access. */
		switch (type) {
			case DVB_AUDIO_DEV:
			case DVB_VIDEO_DEV:
			case DVB_DVR_DEV:
			case DVB_PAINEL_DEV:
				dvb_hisi_open_mask &= ~(1 << type);

				if (type == DVB_AUDIO_DEV && player)
					player->stop(DEV_AUDIO);
				else if (type == DVB_VIDEO_DEV && player)
					player->stop(DEV_VIDEO);
				else if (type == DVB_DVR_DEV) {
					if (fi->fh >= 0)
						close(fi->fh);

					player->set_dvr(false);
				}
			break;
			default:
			break;
		}
	}

	return 0;
}

static int dvb_hisi_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
	(void) offset;
	int ret = -EINVAL;
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (!player || size < 4)
		return -EINVAL;

	switch (type) {
		case DVB_AUDIO_DEV:
			ret = player->write(DEV_AUDIO, buf, size);
		break;
		case DVB_VIDEO_DEV:
			ret = player->write(DEV_VIDEO, buf, size);
		break;
		case DVB_DVR_DEV:
			ret = player->write(DEV_DVR, buf, size);
			if (ret > 0 && fi->fh >= 0)
				write(fi->fh, buf, ret);
		break;
		case DVB_PAINEL_DEV:
			ret = player->write(DEV_PAINEL, buf, size);
		break;
	}

	return ret;
}

static int dvb_hisi_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
	(void) offset;
	(void) fi;
	(void) flags;

	printf("%s -> Called.\n", __FUNCTION__);
	if (dvb_hisi_file_type(path) != DVB_ROOT)
		return -ENOENT;

	filler(buf, ".", NULL, 0, 0);
	filler(buf, "..", NULL, 0, 0);
	filler(buf, AUDIO_DEV, NULL, 0, 0);
	filler(buf, VIDEO_DEV, NULL, 0, 0);
	filler(buf, DVR_DEV, NULL, 0, 0);
	filler(buf, PAINEL_DEV, NULL, 0, 0);

	return 0;
}

static int dvb_hisi_ioctl(const char *path, unsigned int cmd, void *arg, struct fuse_file_info *fi, unsigned int flags, void *data) {
	(void) fi;
	(void) flags;
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	switch (type) {
		case DVB_AUDIO_DEV:
		case DVB_VIDEO_DEV:
			if (!player)
				return -EINVAL;
			if (!(dvb_hisi_open_mask & (1 << type))) {
				switch (cmd) {
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

	switch (cmd) {
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

			return player->status(DEV_AUDIO, (audio_status_t *)data) - 1;
		break;
		case AUDIO_GET_CAPABILITIES:
			printf("%s: AUDIO_GET_CAPABILITIES\n", __FUNCTION__);

			*(unsigned int *)data = AUDIO_CAP_LPCM | AUDIO_CAP_DTS | AUDIO_CAP_AC3 | AUDIO_CAP_MP1 | AUDIO_CAP_MP2;
		break;
		case AUDIO_CLEAR_BUFFER:
			printf("%s: AUDIO_CLEAR_BUFFER\n", __FUNCTION__);

			return player->clear(DEV_AUDIO) - 1;
		break;
		case AUDIO_SET_ID:
			printf("%s: AUDIO_SET_ID\n", __FUNCTION__);
		break;
		case AUDIO_SET_MIXER:
			printf("%s: AUDIO_SET_MIXER\n", __FUNCTION__);

			return player->mixer((audio_mixer_t *)data) - 1;
		break;
		case AUDIO_SET_STREAMTYPE:
			printf("%s: AUDIO_SET_STREAMTYPE\n", __FUNCTION__);
		break;
		case AUDIO_GET_PTS:
			return player->get_pts(DEV_AUDIO, (long long *)data) - 1;
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

			return player->status(DEV_VIDEO, (struct video_status *)data) - 1;
		break;
		case VIDEO_GET_EVENT:
			printf("%s: VIDEO_GET_EVENT\n", __FUNCTION__);

			return player->get_event((struct video_event *)data) - 1;
		break;
		case VIDEO_GET_SIZE:
			printf("%s: VIDEO_GET_SIZE\n", __FUNCTION__);

			return player->get_vsize((video_size_t *)data) - 1;
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
			printf("%s: VIDEO_FAST_FORWARD - %d\n", __FUNCTION__, (int)(intptr_t)arg);

			return player->set_fastfoward((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_SLOWMOTION:
			printf("%s: VIDEO_SLOWMOTION - %d\n", __FUNCTION__, (int)(intptr_t)arg);

			return player->set_slowmotion((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_GET_CAPABILITIES:
			printf("%s: VIDEO_GET_CAPABILITIES\n", __FUNCTION__);

			*(unsigned int *)data = VIDEO_CAP_MPEG1 | VIDEO_CAP_MPEG2 | VIDEO_CAP_SYS | VIDEO_CAP_PROG;
		break;
		case VIDEO_CLEAR_BUFFER:
			printf("%s: VIDEO_CLEAR_BUFFER\n", __FUNCTION__);

			return player->clear(DEV_VIDEO) - 1;
		break;
		case VIDEO_SET_STREAMTYPE:
			printf("%s: VIDEO_SET_STREAMTYPE\n", __FUNCTION__);

			return player->set_type(DEV_VIDEO, (int)(intptr_t)arg) - 1;
		break;
		case VIDEO_GET_FRAME_RATE:
			printf("%s: VIDEO_GET_FRAME_RATE\n", __FUNCTION__);

			return player->get_framerate((unsigned int *)data) - 1;
		break;
		case VIDEO_SET_FRAME_RATE:
			printf("%s: VIDEO_SET_FRAME_RATE\n", __FUNCTION__);

			return player->set_framerate((int)(intptr_t)arg) - 1;
		break;
		case VIDEO_GET_PTS:
			return player->get_pts(DEV_VIDEO, (long long *)data) - 1;
		break;
		default:
			printf("[INFO] %s: UNKNOWN %u\n", __FUNCTION__, cmd);

			return -EINVAL;
		break;
	}

	return 0;
}

static int dvb_hisi_poll(const char *path, struct fuse_file_info *fi, struct fuse_pollhandle *ph, unsigned *reventsp) {
	(void) fi;
	int type = dvb_hisi_file_type(path);
	struct fuse_context *cxt = fuse_get_context();
	struct class_ops *player = (struct class_ops *)cxt->private_data;

	if (!player) {
		if (ph)
			fuse_pollhandle_destroy(ph);
		*reventsp = -EINVAL;
		return 0;
	}
	else
		*reventsp = 0;

	switch (type) {
		case DVB_AUDIO_DEV:
			return player->poll(DEV_AUDIO, ph, reventsp, false);
		break;
		case DVB_VIDEO_DEV:
			return player->poll(DEV_VIDEO, ph, reventsp, dvb_hisi_open_mask & (1 << type));
		break;
		case DVB_DVR_DEV:
			return player->poll(DEV_DVR, ph, reventsp, dvb_hisi_open_mask & (1 << type));
		break;
		default:
			if (ph)
				fuse_pollhandle_destroy(ph);
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
	.write		= dvb_hisi_write,
	.ioctl		= dvb_hisi_ioctl,
	.poll		= dvb_hisi_poll,
};

int main(int argc, char *argv[]) {
	printf("_____________________________________\n");
	printf("#   _   _   _   _   _   _   _   _   #\n");
	printf("#  / \\ / \\ / \\ / \\ / \\ / \\ / \\ / \\  #\n");
	printf("# ( A | V | S | e | r | v | e | r ) #\n");
	printf("#  \\_/ \\_/ \\_/ \\_/ \\_/ \\_/ \\_/ \\_/  #\n");
	printf("#                                   #\n");
	printf("# Created by:                       #\n");
	printf("# 	leandrotsampa               #\n");
	printf("# Contact:                          #\n");
	printf("# 	leandrotsampa@yahoo.com.br  #\n");
	printf("# Current Version:                  #\n");
	printf("# 	%s  #\n", AVSERVER_VERSION);
	printf("\e[4m#___________________________________#\e[24m\n\n");

	return fuse_main(argc, argv, &dvb_hisi_oper, NULL);
}
