#ifndef __AVServer_h
#define __AVServer_h

#define FUSE_USE_VERSION 30

#include <config.h>

#include <fuse.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <sys/types.h>
#include <dirent.h>
#include <linux/ioctl.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/video.h>
#include <pthread.h>

enum {
	DEV_AUDIO,
	DEV_VIDEO,
	DEV_DVR,
	DEV_PAINEL,
};

/* Class Operations */
struct class_ops {
    bool (*create)(void);
    void (*destroy)(void);

	/** Call's for Player **/
	bool (*clear)(int);
	bool (*set_type)(int, int);
	bool (*set_pid)(int, int);
	bool (*set_mode)(int);
	bool (*set_blank)(bool);
	bool (*set_format)(int);
	bool (*set_disp_format)(int);
	bool (*set_fastfoward)(int);
    bool (*play)(int);
	bool (*pause)(int);
	bool (*resume)(int);
	bool (*stop)(int);
	bool (*mute)(bool);
    bool (*sync)(bool);
	bool (*channel)(int);
	bool (*status)(int, void *);
	bool (*get_event)(struct video_event *);
	bool (*get_vsize)(video_size_t *);
	bool (*get_framerate)(unsigned int *);
	bool (*get_pts)(int, long long *);

	/** Other's operations **/
	int (*poll)(int, struct fuse_pollhandle *, unsigned *, bool);
	int (*write)(int, const char *, size_t);

	/** Private data **/
	void *priv;
};

struct class_ops *player_get_ops(void);

#endif /* __AVServer_h */