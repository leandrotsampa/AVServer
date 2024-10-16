#ifndef __AVServer_h
#define __AVServer_h

#include <config.h>

#include <fuse.h>
#include <poll.h>
#include <stdlib.h>
#include <string_ext.h>
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

/* Encoder Operations */
#define IOCTL_BROADCOM_SET_VPID          11
#define IOCTL_BROADCOM_SET_APID          12
#define IOCTL_BROADCOM_SET_PMTPID        13
#define IOCTL_BROADCOM_START_TRANSCODING 100
#define IOCTL_BROADCOM_STOP_TRANSCODING	 200

struct encoder_ops {
	bool (*create)(struct encoder_ops *, int);
	void (*destroy)(struct encoder_ops *);

	/** Call's for Player **/
	bool (*set_pid)(struct encoder_ops *, int, int);
	bool (*set_type)(struct encoder_ops *, int, int);
	bool (*play)(struct encoder_ops *);
	bool (*stop)(struct encoder_ops *);

	/** Other's operations **/
	int (*poll)(struct encoder_ops *, struct fuse_pollhandle *, unsigned *, bool);
	int (*read)(struct encoder_ops *, char *, size_t);
	int (*write)(struct encoder_ops *, const char *, size_t);

	/** Private data **/
	void *priv;
};

struct encoder_ops *get_encoder(void);

/* Class Operations */
struct class_ops {
	bool (*create)(void);
	void (*destroy)(void);

	/** Call's for Player **/
	bool (*clear)(int);
	void (*set_dvr)(bool);
	bool (*set_type)(int, int);
	bool (*set_pid)(int, int);
	bool (*set_mode)(int);
	bool (*set_blank)(bool);
	bool (*set_format)(int);
	bool (*set_framerate)(int);
	bool (*set_disp_format)(int);
	bool (*set_fastfoward)(int);
	bool (*set_slowmotion)(int);
	bool (*play)(int);
	bool (*pause)(int);
	bool (*resume)(int);
	bool (*stop)(int);
	bool (*mute)(bool);
	bool (*mixer)(audio_mixer_t *);
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