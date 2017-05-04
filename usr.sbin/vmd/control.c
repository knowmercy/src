/*	$OpenBSD: control.c,v 1.18 2017/05/04 16:54:41 reyk Exp $	*/

/*
 * Copyright (c) 2010-2015 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>	/* nitems */
#include <sys/queue.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/tree.h>

#include <net/if.h>

#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#include "proc.h"
#include "vmd.h"

#define	CONTROL_BACKLOG	5

struct ctl_connlist ctl_conns;

void
	 control_accept(int, short, void *);
struct ctl_conn
	*control_connbyfd(int);
void	 control_close(int, struct control_sock *);
void	 control_dispatch_imsg(int, short, void *);
int	 control_dispatch_vmd(int, struct privsep_proc *, struct imsg *);
void	 control_imsg_forward(struct imsg *);
void	 control_run(struct privsep *, struct privsep_proc *, void *);

static struct privsep_proc procs[] = {
	{ "parent",	PROC_PARENT,	control_dispatch_vmd }
};

void
control(struct privsep *ps, struct privsep_proc *p)
{
	proc_run(ps, p, procs, nitems(procs), control_run, NULL);
}

void
control_run(struct privsep *ps, struct privsep_proc *p, void *arg)
{
	/*
	 * pledge in the control process:
	 * stdio - for malloc and basic I/O including events.
	 * cpath - for managing the control socket.
	 * unix - for the control socket.
	 * recvfd - for the proc fd exchange.
	 */
	if (pledge("stdio cpath unix recvfd", NULL) == -1)
		fatal("pledge");
}

int
control_dispatch_vmd(int fd, struct privsep_proc *p, struct imsg *imsg)
{
	struct ctl_conn		*c;
	struct privsep		*ps = p->p_ps;

	switch (imsg->hdr.type) {
	case IMSG_VMDOP_START_VM_RESPONSE:
	case IMSG_VMDOP_TERMINATE_VM_RESPONSE:
	case IMSG_VMDOP_GET_INFO_VM_DATA:
	case IMSG_VMDOP_GET_INFO_VM_END_DATA:
		if ((c = control_connbyfd(imsg->hdr.peerid)) == NULL) {
			log_warnx("%s: lost control connection: fd %d",
			    __func__, imsg->hdr.peerid);
			return (0);
		}
		imsg_compose_event(&c->iev, imsg->hdr.type,
		    0, 0, -1, imsg->data, IMSG_DATA_SIZE(imsg));
		break;
	case IMSG_VMDOP_CONFIG:
		config_getconfig(ps->ps_env, imsg);
		break;
	case IMSG_CTL_RESET:
		config_getreset(ps->ps_env, imsg);
		break;
	default:
		return (-1);
	}

	return (0);
}

int
control_init(struct privsep *ps, struct control_sock *cs)
{
	struct sockaddr_un	 sun;
	int			 fd;
	mode_t			 old_umask, mode;

	if (cs->cs_name == NULL)
		return (0);

	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0)) == -1) {
		log_warn("%s: socket", __func__);
		return (-1);
	}

	sun.sun_family = AF_UNIX;
	if (strlcpy(sun.sun_path, cs->cs_name,
	    sizeof(sun.sun_path)) >= sizeof(sun.sun_path)) {
		log_warn("%s: %s name too long", __func__, cs->cs_name);
		close(fd);
		return (-1);
	}

	if (unlink(cs->cs_name) == -1)
		if (errno != ENOENT) {
			log_warn("%s: unlink %s", __func__, cs->cs_name);
			close(fd);
			return (-1);
		}

	if (cs->cs_restricted) {
		old_umask = umask(S_IXUSR|S_IXGRP|S_IXOTH);
		mode = S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH;
	} else {
		old_umask = umask(S_IXUSR|S_IXGRP|S_IWOTH|S_IROTH|S_IXOTH);
		mode = S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP;
	}

	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) == -1) {
		log_warn("%s: bind: %s", __func__, cs->cs_name);
		close(fd);
		(void)umask(old_umask);
		return (-1);
	}
	(void)umask(old_umask);

	if (chmod(cs->cs_name, mode) == -1) {
		log_warn("%s: chmod", __func__);
		close(fd);
		(void)unlink(cs->cs_name);
		return (-1);
	}

	cs->cs_fd = fd;
	cs->cs_env = ps;

	return (0);
}

int
control_listen(struct control_sock *cs)
{
	if (cs->cs_name == NULL)
		return (0);

	if (listen(cs->cs_fd, CONTROL_BACKLOG) == -1) {
		log_warn("%s: listen", __func__);
		return (-1);
	}

	event_set(&cs->cs_ev, cs->cs_fd, EV_READ,
	    control_accept, cs);
	event_add(&cs->cs_ev, NULL);
	evtimer_set(&cs->cs_evt, control_accept, cs);

	return (0);
}

void
control_cleanup(struct control_sock *cs)
{
	if (cs->cs_name == NULL)
		return;
	event_del(&cs->cs_ev);
	event_del(&cs->cs_evt);
}

/* ARGSUSED */
void
control_accept(int listenfd, short event, void *arg)
{
	struct control_sock	*cs = arg;
	int			 connfd;
	socklen_t		 len;
	struct sockaddr_un	 sun;
	struct ctl_conn		*c;

	event_add(&cs->cs_ev, NULL);
	if ((event & EV_TIMEOUT))
		return;

	len = sizeof(sun);
	if ((connfd = accept4(listenfd,
	    (struct sockaddr *)&sun, &len, SOCK_NONBLOCK)) == -1) {
		/*
		 * Pause accept if we are out of file descriptors, or
		 * libevent will haunt us here too.
		 */
		if (errno == ENFILE || errno == EMFILE) {
			struct timeval evtpause = { 1, 0 };

			event_del(&cs->cs_ev);
			evtimer_add(&cs->cs_evt, &evtpause);
		} else if (errno != EWOULDBLOCK && errno != EINTR &&
		    errno != ECONNABORTED)
			log_warn("%s: accept", __func__);
		return;
	}

	if ((c = calloc(1, sizeof(struct ctl_conn))) == NULL) {
		log_warn("%s", __func__);
		close(connfd);
		return;
	}

	if (getsockopt(connfd, SOL_SOCKET, SO_PEERCRED,
	    &c->peercred, &len) != 0) {
		log_warn("%s: failed to get peer credentials", __func__);
		close(connfd);
		free(c);
		return;
	}

	imsg_init(&c->iev.ibuf, connfd);
	c->iev.handler = control_dispatch_imsg;
	c->iev.events = EV_READ;
	c->iev.data = cs;
	event_set(&c->iev.ev, c->iev.ibuf.fd, c->iev.events,
	    c->iev.handler, c->iev.data);
	event_add(&c->iev.ev, NULL);

	TAILQ_INSERT_TAIL(&ctl_conns, c, entry);
}

struct ctl_conn *
control_connbyfd(int fd)
{
	struct ctl_conn	*c;

	TAILQ_FOREACH(c, &ctl_conns, entry) {
		if (c->iev.ibuf.fd == fd)
			break;
	}

	return (c);
}

void
control_close(int fd, struct control_sock *cs)
{
	struct ctl_conn	*c;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warn("%s: fd %d: not found", __func__, fd);
		return;
	}

	msgbuf_clear(&c->iev.ibuf.w);
	TAILQ_REMOVE(&ctl_conns, c, entry);

	event_del(&c->iev.ev);
	close(c->iev.ibuf.fd);

	/* Some file descriptors are available again. */
	if (evtimer_pending(&cs->cs_evt, NULL)) {
		evtimer_del(&cs->cs_evt);
		event_add(&cs->cs_ev, NULL);
	}

	free(c);
}

/* ARGSUSED */
void
control_dispatch_imsg(int fd, short event, void *arg)
{
	struct control_sock		*cs = arg;
	struct privsep			*ps = cs->cs_env;
	struct ctl_conn			*c;
	struct imsg			 imsg;
	struct vmop_create_params	 vmc;
	struct vmop_id			 vid;
	int				 n, v, ret = 0;

	if ((c = control_connbyfd(fd)) == NULL) {
		log_warn("%s: fd %d: not found", __func__, fd);
		return;
	}

	if (event & EV_READ) {
		if (((n = imsg_read(&c->iev.ibuf)) == -1 && errno != EAGAIN) ||
		    n == 0) {
			control_close(fd, cs);
			return;
		}
	}
	if (event & EV_WRITE) {
		if (msgbuf_write(&c->iev.ibuf.w) <= 0 && errno != EAGAIN) {
			control_close(fd, cs);
			return;
		}
	}

	for (;;) {
		if ((n = imsg_get(&c->iev.ibuf, &imsg)) == -1) {
			control_close(fd, cs);
			return;
		}

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_VMDOP_GET_INFO_VM_REQUEST:
		case IMSG_VMDOP_TERMINATE_VM_REQUEST:
		case IMSG_VMDOP_START_VM_REQUEST:
			break;
		default:
			if (c->peercred.uid != 0) {
				log_warnx("denied request %d from uid %d",
				    imsg.hdr.type, c->peercred.uid);
				ret = EPERM;
				goto fail;
			}
			break;
		}

		control_imsg_forward(&imsg);

		switch (imsg.hdr.type) {
		case IMSG_CTL_NOTIFY:
			if (c->flags & CTL_CONN_NOTIFY) {
				log_debug("%s: "
				    "client requested notify more than once",
				    __func__);
				ret = EINVAL;
				goto fail;
			}
			c->flags |= CTL_CONN_NOTIFY;
			break;
		case IMSG_CTL_VERBOSE:
			if (IMSG_DATA_SIZE(&imsg) < sizeof(v))
				goto fail;
			memcpy(&v, imsg.data, sizeof(v));
			log_setverbose(v);

			/* FALLTHROUGH */
		case IMSG_VMDOP_LOAD:
		case IMSG_VMDOP_RELOAD:
		case IMSG_CTL_RESET:
			proc_forward_imsg(ps, &imsg, PROC_PARENT, -1);

			/* Report success to the control client */
			imsg_compose_event(&c->iev, IMSG_CTL_OK,
			    0, 0, -1, NULL, 0);
			break;
		case IMSG_VMDOP_START_VM_REQUEST:
			if (IMSG_DATA_SIZE(&imsg) < sizeof(vmc))
				goto fail;
			memcpy(&vmc, imsg.data, sizeof(vmc));
			vmc.vmc_uid = c->peercred.uid;
			vmc.vmc_gid = -1;

			if (proc_compose_imsg(ps, PROC_PARENT, -1,
			    imsg.hdr.type, fd, -1, &vmc, sizeof(vmc)) == -1) {
				control_close(fd, cs);
				return;
			}
			break;
		case IMSG_VMDOP_TERMINATE_VM_REQUEST:
			if (IMSG_DATA_SIZE(&imsg) < sizeof(vid))
				goto fail;
			memcpy(&vid, imsg.data, sizeof(vid));
			vid.vid_uid = c->peercred.uid;

			if (proc_compose_imsg(ps, PROC_PARENT, -1,
			    imsg.hdr.type, fd, -1, &vid, sizeof(vid)) == -1) {
				control_close(fd, cs);
				return;
			}
			break;
		case IMSG_VMDOP_GET_INFO_VM_REQUEST:
			if (IMSG_DATA_SIZE(&imsg) != 0)
				goto fail;
			if (proc_compose_imsg(ps, PROC_PARENT, -1,
			    imsg.hdr.type, fd, -1, NULL, 0) == -1) {
				control_close(fd, cs);
				return;
			}
			break;
		default:
			log_debug("%s: error handling imsg %d",
			    __func__, imsg.hdr.type);
			control_close(fd, cs);
			break;
		}
		imsg_free(&imsg);
	}

	imsg_event_add(&c->iev);
	return;

 fail:
	if (ret == 0)
		ret = EINVAL;
	imsg_compose_event(&c->iev, IMSG_CTL_FAIL,
	    0, 0, -1, &ret, sizeof(ret));
	imsg_flush(&c->iev.ibuf);
	control_close(fd, cs);
}

void
control_imsg_forward(struct imsg *imsg)
{
	struct ctl_conn *c;

	TAILQ_FOREACH(c, &ctl_conns, entry)
		if (c->flags & CTL_CONN_NOTIFY)
			imsg_compose_event(&c->iev, imsg->hdr.type,
			    imsg->hdr.peerid, imsg->hdr.pid, -1, imsg->data,
			    imsg->hdr.len - IMSG_HEADER_SIZE);
}
