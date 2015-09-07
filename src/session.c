/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2010-2012 Bert Vermeulen <bert@biot.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <glib.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

/** @cond PRIVATE */
#define LOG_PREFIX "session"
/** @endcond */

/**
 * @file
 *
 * Creating, using, or destroying libsigrok sessions.
 */

/**
 * @defgroup grp_session Session handling
 *
 * Creating, using, or destroying libsigrok sessions.
 *
 * @{
 */

struct source {
	int64_t timeout;	/* microseconds */
	int64_t due;		/* microseconds */

	sr_receive_data_callback cb;
	void *cb_data;

	/* This is used to keep track of the object (fd, pollfd or channel) which is
	 * being polled and will be used to match the source when removing it again.
	 */
	gintptr poll_object;

	/* Number of fds to poll for this source. This will be 0 for timer
	 * sources, 1 for normal I/O sources, and 1 or more for libusb I/O
	 * sources on Unix platforms.
	 */
	int num_fds;

	gboolean triggered;
};

struct datafeed_callback {
	sr_datafeed_callback cb;
	void *cb_data;
};

/**
 * Create a new session.
 *
 * @param ctx         The context in which to create the new session.
 * @param new_session This will contain a pointer to the newly created
 *                    session if the return value is SR_OK, otherwise the value
 *                    is undefined and should not be used. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.4.0
 */
SR_API int sr_session_new(struct sr_context *ctx,
		struct sr_session **new_session)
{
	struct sr_session *session;

	if (!new_session)
		return SR_ERR_ARG;

	session = g_malloc0(sizeof(struct sr_session));

	session->ctx = ctx;

	session->sources = g_array_new(FALSE, FALSE, sizeof(struct source));
	session->pollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));

	g_mutex_init(&session->stop_mutex);

	*new_session = session;

	return SR_OK;
}

/**
 * Destroy a session.
 * This frees up all memory used by the session.
 *
 * @param session The session to destroy. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 *
 * @since 0.4.0
 */
SR_API int sr_session_destroy(struct sr_session *session)
{
	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	sr_session_dev_remove_all(session);
	g_mutex_clear(&session->stop_mutex);
	if (session->trigger)
		sr_trigger_free(session->trigger);

	g_slist_free_full(session->owned_devs, (GDestroyNotify)sr_dev_inst_free);

	g_array_unref(session->pollfds);
	g_array_unref(session->sources);

	g_free(session);

	return SR_OK;
}

/**
 * Remove all the devices from a session.
 *
 * The session itself (i.e., the struct sr_session) is not free'd and still
 * exists after this function returns.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_BUG Invalid session passed.
 *
 * @since 0.4.0
 */
SR_API int sr_session_dev_remove_all(struct sr_session *session)
{
	struct sr_dev_inst *sdi;
	GSList *l;

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	for (l = session->devs; l; l = l->next) {
		sdi = (struct sr_dev_inst *) l->data;
		sdi->session = NULL;
	}

	g_slist_free(session->devs);
	session->devs = NULL;

	return SR_OK;
}

/**
 * Add a device instance to a session.
 *
 * @param session The session to add to. Must not be NULL.
 * @param sdi The device instance to add to a session. Must not
 *            be NULL. Also, sdi->driver and sdi->driver->dev_open must
 *            not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.4.0
 */
SR_API int sr_session_dev_add(struct sr_session *session,
		struct sr_dev_inst *sdi)
{
	int ret;

	if (!sdi) {
		sr_err("%s: sdi was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	/* If sdi->session is not NULL, the device is already in this or
	 * another session. */
	if (sdi->session) {
		sr_err("%s: already assigned to session", __func__);
		return SR_ERR_ARG;
	}

	/* If sdi->driver is NULL, this is a virtual device. */
	if (!sdi->driver) {
		/* Just add the device, don't run dev_open(). */
		session->devs = g_slist_append(session->devs, (gpointer)sdi);
		sdi->session = session;
		return SR_OK;
	}

	/* sdi->driver is non-NULL (i.e. we have a real device). */
	if (!sdi->driver->dev_open) {
		sr_err("%s: sdi->driver->dev_open was NULL", __func__);
		return SR_ERR_BUG;
	}

	session->devs = g_slist_append(session->devs, (gpointer)sdi);
	sdi->session = session;

	if (session->running) {
		/* Adding a device to a running session. Commit settings
		 * and start acquisition on that device now. */
		if ((ret = sr_config_commit(sdi)) != SR_OK) {
			sr_err("Failed to commit device settings before "
			       "starting acquisition in running session (%s)",
			       sr_strerror(ret));
			return ret;
		}
		if ((ret = sdi->driver->dev_acquisition_start(sdi,
						(void *)sdi)) != SR_OK) {
			sr_err("Failed to start acquisition of device in "
			       "running session (%s)", sr_strerror(ret));
			return ret;
		}
	}

	return SR_OK;
}

/**
 * List all device instances attached to a session.
 *
 * @param session The session to use. Must not be NULL.
 * @param devlist A pointer where the device instance list will be
 *                stored on return. If no devices are in the session,
 *                this will be NULL. Each element in the list points
 *                to a struct sr_dev_inst *.
 *                The list must be freed by the caller, but not the
 *                elements pointed to.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.4.0
 */
SR_API int sr_session_dev_list(struct sr_session *session, GSList **devlist)
{
	if (!session)
		return SR_ERR_ARG;

	if (!devlist)
		return SR_ERR_ARG;

	*devlist = g_slist_copy(session->devs);

	return SR_OK;
}

/**
 * Remove all datafeed callbacks in a session.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 *
 * @since 0.4.0
 */
SR_API int sr_session_datafeed_callback_remove_all(struct sr_session *session)
{
	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	g_slist_free_full(session->datafeed_callbacks, g_free);
	session->datafeed_callbacks = NULL;

	return SR_OK;
}

/**
 * Add a datafeed callback to a session.
 *
 * @param session The session to use. Must not be NULL.
 * @param cb Function to call when a chunk of data is received.
 *           Must not be NULL.
 * @param cb_data Opaque pointer passed in by the caller.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_BUG No session exists.
 *
 * @since 0.3.0
 */
SR_API int sr_session_datafeed_callback_add(struct sr_session *session,
		sr_datafeed_callback cb, void *cb_data)
{
	struct datafeed_callback *cb_struct;

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_BUG;
	}

	if (!cb) {
		sr_err("%s: cb was NULL", __func__);
		return SR_ERR_ARG;
	}

	cb_struct = g_malloc0(sizeof(struct datafeed_callback));
	cb_struct->cb = cb;
	cb_struct->cb_data = cb_data;

	session->datafeed_callbacks =
	    g_slist_append(session->datafeed_callbacks, cb_struct);

	return SR_OK;
}

/**
 * Get the trigger assigned to this session.
 *
 * @param session The session to use.
 *
 * @retval NULL Invalid (NULL) session was passed to the function.
 * @retval other The trigger assigned to this session (can be NULL).
 *
 * @since 0.4.0
 */
SR_API struct sr_trigger *sr_session_trigger_get(struct sr_session *session)
{
	if (!session)
		return NULL;

	return session->trigger;
}

/**
 * Set the trigger of this session.
 *
 * @param session The session to use. Must not be NULL.
 * @param trig The trigger to assign to this session. Can be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.4.0
 */
SR_API int sr_session_trigger_set(struct sr_session *session, struct sr_trigger *trig)
{
	if (!session)
		return SR_ERR_ARG;

	session->trigger = trig;

	return SR_OK;
}

static gboolean sr_session_check_aborted(struct sr_session *session)
{
	gboolean stop;

	g_mutex_lock(&session->stop_mutex);
	stop = session->abort_session;
	if (stop) {
		sr_session_stop_sync(session);
		/* But once is enough. */
		session->abort_session = FALSE;
	}
	g_mutex_unlock(&session->stop_mutex);

	return stop;
}

/**
 * Poll the session's event sources.
 *
 * @param session The session to use. Must not be NULL.
 * @retval SR_OK Success.
 * @retval SR_ERR Error occurred.
 */
static int sr_session_iteration(struct sr_session *session)
{
	int64_t start_time, stop_time, min_due, due;
	int timeout_ms;
	unsigned int i;
	int k, fd_index;
	int ret;
	int fd;
	int revents;
	gboolean triggered, stopped;
	struct source *source;
	GPollFD *pollfd;
	gintptr poll_object;
#if HAVE_LIBUSB_1_0
	int64_t usb_timeout;
	int64_t usb_due;
	struct timeval tv;
#endif
	if (session->sources->len == 0) {
		sr_session_check_aborted(session);
		return SR_OK;
	}
	start_time = g_get_monotonic_time();
	min_due = INT64_MAX;

	for (i = 0; i < session->sources->len; ++i) {
		source = &g_array_index(session->sources, struct source, i);
		if (source->due < min_due)
			min_due = source->due;
		source->triggered = FALSE;
	}
#if HAVE_LIBUSB_1_0
	usb_due = INT64_MAX;
	if (session->ctx->usb_source_present) {
		ret = libusb_get_next_timeout(session->ctx->libusb_ctx, &tv);
		if (ret < 0) {
			sr_err("Error getting libusb timeout: %s",
				libusb_error_name(ret));
			return SR_ERR;
		} else if (ret == 1) {
			usb_timeout = (int64_t)tv.tv_sec * G_USEC_PER_SEC
					+ tv.tv_usec;
			usb_due = start_time + usb_timeout;
			if (usb_due < min_due)
				min_due = usb_due;

			sr_spew("poll: next USB timeout %g ms",
				1e-3 * usb_timeout);
		}
	}
#endif
	if (min_due == INT64_MAX)
		timeout_ms = -1;
	else if (min_due > start_time)
		timeout_ms = MIN((min_due - start_time + 999) / 1000, INT_MAX);
	else
		timeout_ms = 0;

	sr_spew("poll enter: %u sources, %u fds, %d ms timeout",
		session->sources->len, session->pollfds->len, timeout_ms);

	ret = g_poll((GPollFD *)session->pollfds->data,
			session->pollfds->len, timeout_ms);
#ifdef G_OS_UNIX
	if (ret < 0 && errno != EINTR) {
		sr_err("Error in poll: %s", g_strerror(errno));
		return SR_ERR;
	}
#else
	if (ret < 0) {
		sr_err("Error in poll: %d", ret);
		return SR_ERR;
	}
#endif
	stop_time = g_get_monotonic_time();

	sr_spew("poll leave: %g ms elapsed, %d events",
		1e-3 * (stop_time - start_time), ret);

	triggered = FALSE;
	stopped = FALSE;
	fd_index = 0;

	for (i = 0; i < session->sources->len; ++i) {
		source = &g_array_index(session->sources, struct source, i);

		poll_object = source->poll_object;
		fd = (int)poll_object;
		revents = 0;

		for (k = 0; k < source->num_fds; ++k) {
			pollfd = &g_array_index(session->pollfds,
					GPollFD, fd_index + k);
			fd = pollfd->fd;
			revents |= pollfd->revents;
		}
		fd_index += source->num_fds;

		if (source->triggered)
			continue; /* already handled */
		if (ret > 0 && revents == 0)
			continue; /* skip timeouts if any I/O event occurred */

		/* Make invalid to avoid confusion in case of multiple FDs. */
		if (source->num_fds > 1)
			fd = -1;
		if (ret <= 0)
			revents = 0;

		due = source->due;
#if HAVE_LIBUSB_1_0
		if (usb_due < due && poll_object
				== (gintptr)session->ctx->libusb_ctx)
			due = usb_due;
#endif
		if (revents == 0 && stop_time < due)
			continue;
		/*
		 * The source may be gone after the callback returns,
		 * so access any data now that needs accessing.
		 */
		if (source->timeout >= 0)
			source->due = stop_time + source->timeout;
		source->triggered = TRUE;
		triggered = TRUE;
		/*
		 * Invoke the source's callback on an event or timeout.
		 */
		sr_spew("callback for event source %" G_GINTPTR_FORMAT " with"
			" event mask 0x%.2X", poll_object, (unsigned)revents);
		if (!source->cb(fd, revents, source->cb_data))
			sr_session_source_remove_internal(session, poll_object);
		/*
		 * We want to take as little time as possible to stop
		 * the session if we have been told to do so. Therefore,
		 * we check the flag after processing every source, not
		 * just once per main event loop.
		 */
		if (!stopped)
			stopped = sr_session_check_aborted(session);

		/* Restart loop as the sources list may have changed. */
		fd_index = 0;
		i = 0;
	}

	/* Check for abort at least once per iteration. */
	if (!triggered)
		sr_session_check_aborted(session);

	return SR_OK;
}

static int verify_trigger(struct sr_trigger *trigger)
{
	struct sr_trigger_stage *stage;
	struct sr_trigger_match *match;
	GSList *l, *m;

	if (!trigger->stages) {
		sr_err("No trigger stages defined.");
		return SR_ERR;
	}

	sr_spew("Checking trigger:");
	for (l = trigger->stages; l; l = l->next) {
		stage = l->data;
		if (!stage->matches) {
			sr_err("Stage %d has no matches defined.", stage->stage);
			return SR_ERR;
		}
		for (m = stage->matches; m; m = m->next) {
			match = m->data;
			if (!match->channel) {
				sr_err("Stage %d match has no channel.", stage->stage);
				return SR_ERR;
			}
			if (!match->match) {
				sr_err("Stage %d match is not defined.", stage->stage);
				return SR_ERR;
			}
			sr_spew("Stage %d match on channel %s, match %d", stage->stage,
					match->channel->name, match->match);
		}
	}

	return SR_OK;
}

/**
 * Start a session.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 *
 * @since 0.4.0
 */
SR_API int sr_session_start(struct sr_session *session)
{
	struct sr_dev_inst *sdi;
	struct sr_channel *ch;
	GSList *l, *c;
	int enabled_channels, ret;

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!session->devs) {
		sr_err("%s: session->devs was NULL; a session "
		       "cannot be started without devices.", __func__);
		return SR_ERR_ARG;
	}

	if (session->trigger && verify_trigger(session->trigger) != SR_OK)
		return SR_ERR;

	sr_info("Starting.");

	ret = SR_OK;
	for (l = session->devs; l; l = l->next) {
		sdi = l->data;
		enabled_channels = 0;
		for (c = sdi->channels; c; c = c->next) {
			ch = c->data;
			if (ch->enabled) {
				enabled_channels++;
				break;
			}
		}
		if (enabled_channels == 0) {
			ret = SR_ERR;
			sr_err("%s using connection %s has no enabled channels!",
					sdi->driver->name, sdi->connection_id);
			break;
		}

		if ((ret = sr_config_commit(sdi)) != SR_OK) {
			sr_err("Failed to commit device settings before "
			       "starting acquisition (%s)", sr_strerror(ret));
			break;
		}
		if ((ret = sdi->driver->dev_acquisition_start(sdi, sdi)) != SR_OK) {
			sr_err("%s: could not start an acquisition "
			       "(%s)", __func__, sr_strerror(ret));
			break;
		}
	}

	/* TODO: What if there are multiple devices? Which return code? */

	return ret;
}

/**
 * Run a session.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 * @retval SR_ERR Error during event processing.
 *
 * @since 0.4.0
 */
SR_API int sr_session_run(struct sr_session *session)
{
	int ret;

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!session->devs) {
		/* TODO: Actually the case? */
		sr_err("%s: session->devs was NULL; a session "
		       "cannot be run without devices.", __func__);
		return SR_ERR_ARG;
	}
	session->running = TRUE;

	sr_info("Running.");

	/* Poll event sources until none are left. */
	while (session->sources->len > 0) {
		ret = sr_session_iteration(session);
		if (ret != SR_OK)
			return ret;
	}
	return SR_OK;
}

/**
 * Stop a session.
 *
 * The session is stopped immediately, with all acquisition sessions stopped
 * and hardware drivers cleaned up.
 *
 * This must be called from within the session thread, to prevent freeing
 * resources that the session thread will try to use.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 *
 * @private
 */
SR_PRIV int sr_session_stop_sync(struct sr_session *session)
{
	struct sr_dev_inst *sdi;
	GSList *l;

	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_ARG;
	}

	sr_info("Stopping.");

	for (l = session->devs; l; l = l->next) {
		sdi = l->data;
		if (sdi->driver) {
			if (sdi->driver->dev_acquisition_stop)
				sdi->driver->dev_acquisition_stop(sdi, sdi);
		}
	}
	session->running = FALSE;

	return SR_OK;
}

/**
 * Stop a session.
 *
 * The session is stopped immediately, with all acquisition sessions being
 * stopped and hardware drivers cleaned up.
 *
 * If the session is run in a separate thread, this function will not block
 * until the session is finished executing. It is the caller's responsibility
 * to wait for the session thread to return before assuming that the session is
 * completely decommissioned.
 *
 * @param session The session to use. Must not be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid session passed.
 *
 * @since 0.4.0
 */
SR_API int sr_session_stop(struct sr_session *session)
{
	if (!session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_BUG;
	}

	g_mutex_lock(&session->stop_mutex);
	session->abort_session = TRUE;
	g_mutex_unlock(&session->stop_mutex);

	return SR_OK;
}

/**
 * Debug helper.
 *
 * @param packet The packet to show debugging information for.
 */
static void datafeed_dump(const struct sr_datafeed_packet *packet)
{
	const struct sr_datafeed_logic *logic;
	const struct sr_datafeed_analog *analog;
	const struct sr_datafeed_analog2 *analog2;

	/* Please use the same order as in libsigrok.h. */
	switch (packet->type) {
	case SR_DF_HEADER:
		sr_dbg("bus: Received SR_DF_HEADER packet.");
		break;
	case SR_DF_END:
		sr_dbg("bus: Received SR_DF_END packet.");
		break;
	case SR_DF_META:
		sr_dbg("bus: Received SR_DF_META packet.");
		break;
	case SR_DF_TRIGGER:
		sr_dbg("bus: Received SR_DF_TRIGGER packet.");
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		sr_dbg("bus: Received SR_DF_LOGIC packet (%" PRIu64 " bytes, "
		       "unitsize = %d).", logic->length, logic->unitsize);
		break;
	case SR_DF_ANALOG:
		analog = packet->payload;
		sr_dbg("bus: Received SR_DF_ANALOG packet (%d samples).",
		       analog->num_samples);
		break;
	case SR_DF_FRAME_BEGIN:
		sr_dbg("bus: Received SR_DF_FRAME_BEGIN packet.");
		break;
	case SR_DF_FRAME_END:
		sr_dbg("bus: Received SR_DF_FRAME_END packet.");
		break;
	case SR_DF_ANALOG2:
		analog2 = packet->payload;
		sr_dbg("bus: Received SR_DF_ANALOG2 packet (%d samples).",
		       analog2->num_samples);
		break;
	default:
		sr_dbg("bus: Received unknown packet type: %d.", packet->type);
		break;
	}
}

/**
 * Send a packet to whatever is listening on the datafeed bus.
 *
 * Hardware drivers use this to send a data packet to the frontend.
 *
 * @param sdi TODO.
 * @param packet The datafeed packet to send to the session bus.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @private
 */
SR_PRIV int sr_session_send(const struct sr_dev_inst *sdi,
		const struct sr_datafeed_packet *packet)
{
	GSList *l;
	struct datafeed_callback *cb_struct;
	struct sr_datafeed_packet *packet_in, *packet_out;
	struct sr_transform *t;
	int ret;

	if (!sdi) {
		sr_err("%s: sdi was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!packet) {
		sr_err("%s: packet was NULL", __func__);
		return SR_ERR_ARG;
	}

	if (!sdi->session) {
		sr_err("%s: session was NULL", __func__);
		return SR_ERR_BUG;
	}

	/*
	 * Pass the packet to the first transform module. If that returns
	 * another packet (instead of NULL), pass that packet to the next
	 * transform module in the list, and so on.
	 */
	packet_in = (struct sr_datafeed_packet *)packet;
	for (l = sdi->session->transforms; l; l = l->next) {
		t = l->data;
		sr_spew("Running transform module '%s'.", t->module->id);
		ret = t->module->receive(t, packet_in, &packet_out);
		if (ret < 0) {
			sr_err("Error while running transform module: %d.", ret);
			return SR_ERR;
		}
		if (!packet_out) {
			/*
			 * If any of the transforms don't return an output
			 * packet, abort.
			 */
			sr_spew("Transform module didn't return a packet, aborting.");
			return SR_OK;
		} else {
			/*
			 * Use this transform module's output packet as input
			 * for the next transform module.
			 */
			packet_in = packet_out;
		}
	}
	packet = packet_in;

	/*
	 * If the last transform did output a packet, pass it to all datafeed
	 * callbacks.
	 */
	for (l = sdi->session->datafeed_callbacks; l; l = l->next) {
		if (sr_log_loglevel_get() >= SR_LOG_DBG)
			datafeed_dump(packet);
		cb_struct = l->data;
		cb_struct->cb(sdi, packet, cb_struct->cb_data);
	}

	return SR_OK;
}

/**
 * Add an event source for a file descriptor.
 *
 * @param session The session to use. Must not be NULL.
 * @param[in] pollfds The FDs to poll, or NULL if @a num_fds is 0.
 * @param[in] num_fds Number of FDs in the array.
 * @param[in] timeout Max time in ms to wait before the callback is called,
 *                    or -1 to wait indefinitely.
 * @param cb Callback function to add. Must not be NULL.
 * @param cb_data Data for the callback function. Can be NULL.
 * @param poll_object Handle by which the source is identified
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 * @retval SR_ERR An event source for @a poll_object is already installed.
 */
SR_PRIV int sr_session_source_add_internal(struct sr_session *session,
		const GPollFD *pollfds, int num_fds, int timeout,
		sr_receive_data_callback cb, void *cb_data,
		gintptr poll_object)
{
	struct source src;
	unsigned int i;
	int k;

	/* Note: cb_data can be NULL, that's not a bug. */
	if (!cb) {
		sr_err("%s: cb was NULL", __func__);
		return SR_ERR_ARG;
	}
	if (!pollfds && num_fds != 0) {
		sr_err("%s: pollfds was NULL", __func__);
		return SR_ERR_ARG;
	}
	/* Make sure that poll_object is unique.
	 */
	for (i = 0; i < session->sources->len; ++i) {
		if (g_array_index(session->sources, struct source, i)
				.poll_object == poll_object) {
			sr_err("Event source %" G_GINTPTR_FORMAT
				" already installed.", poll_object);
			return SR_ERR;
		}
	}
	sr_dbg("Installing event source %" G_GINTPTR_FORMAT " with %d FDs"
		" and %d ms timeout.", poll_object, num_fds, timeout);
	src.cb = cb;
	src.cb_data = cb_data;
	src.poll_object = poll_object;
	src.num_fds = num_fds;
	src.triggered = FALSE;

	if (timeout >= 0) {
		src.timeout = INT64_C(1000) * timeout;
		src.due = g_get_monotonic_time() + src.timeout;
	} else {
		src.timeout = -1;
		src.due = INT64_MAX;
	}
	g_array_append_val(session->sources, src);

	for (k = 0; k < num_fds; ++k) {
		sr_dbg("Registering poll FD %" G_GINTPTR_FORMAT
			" with event mask 0x%.2X.",
			(gintptr)pollfds[k].fd, (unsigned)pollfds[k].events);
	}
	g_array_append_vals(session->pollfds, pollfds, num_fds);

	return SR_OK;
}

/**
 * Add an event source for a file descriptor.
 *
 * @param session The session to use. Must not be NULL.
 * @param fd The file descriptor.
 * @param events Events to check for.
 * @param timeout Max time in ms to wait before the callback is called,
 *                or -1 to wait indefinitely.
 * @param cb Callback function to add. Must not be NULL.
 * @param cb_data Data for the callback function. Can be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.3.0
 */
SR_API int sr_session_source_add(struct sr_session *session, int fd,
		int events, int timeout, sr_receive_data_callback cb, void *cb_data)
{
	GPollFD p;

	if (fd < 0 && timeout < 0) {
		sr_err("Timer source without timeout would block indefinitely");
		return SR_ERR_ARG;
	}
	p.fd = fd;
	p.events = events;
	p.revents = 0;

	return sr_session_source_add_internal(session,
		&p, (fd < 0) ? 0 : 1, timeout, cb, cb_data, fd);
}

/**
 * Add an event source for a GPollFD.
 *
 * @param session The session to use. Must not be NULL.
 * @param pollfd The GPollFD. Must not be NULL.
 * @param timeout Max time in ms to wait before the callback is called,
 *                or -1 to wait indefinitely.
 * @param cb Callback function to add. Must not be NULL.
 * @param cb_data Data for the callback function. Can be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.3.0
 */
SR_API int sr_session_source_add_pollfd(struct sr_session *session,
		GPollFD *pollfd, int timeout, sr_receive_data_callback cb,
		void *cb_data)
{
	if (!pollfd) {
		sr_err("%s: pollfd was NULL", __func__);
		return SR_ERR_ARG;
	}
	return sr_session_source_add_internal(session, pollfd, 1,
			timeout, cb, cb_data, (gintptr)pollfd);
}

/**
 * Add an event source for a GIOChannel.
 *
 * @param session The session to use. Must not be NULL.
 * @param channel The GIOChannel.
 * @param events Events to poll on.
 * @param timeout Max time in ms to wait before the callback is called,
 *                or -1 to wait indefinitely.
 * @param cb Callback function to add. Must not be NULL.
 * @param cb_data Data for the callback function. Can be NULL.
 *
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 *
 * @since 0.3.0
 */
SR_API int sr_session_source_add_channel(struct sr_session *session,
		GIOChannel *channel, int events, int timeout,
		sr_receive_data_callback cb, void *cb_data)
{
	GPollFD p;

#ifdef G_OS_WIN32
	g_io_channel_win32_make_pollfd(channel, events, &p);
#else
	p.fd = g_io_channel_unix_get_fd(channel);
	p.events = events;
	p.revents = 0;
#endif
	return sr_session_source_add_internal(session, &p, 1,
			timeout, cb, cb_data, (gintptr)channel);
}

/**
 * Remove the source identified by the specified poll object.
 *
 * @param session The session to use. Must not be NULL.
 * @param poll_object The channel for which the source should be removed.
 *
 * @retval SR_OK Success
 * @retval SR_ERR_BUG No event source for poll_object found.
 */
SR_PRIV int sr_session_source_remove_internal(struct sr_session *session,
		gintptr poll_object)
{
	struct source *source;
	unsigned int i;
	int fd_index = 0;

	for (i = 0; i < session->sources->len; ++i) {
		source = &g_array_index(session->sources, struct source, i);

		if (source->poll_object == poll_object) {
			if (source->num_fds > 0)
				g_array_remove_range(session->pollfds,
						fd_index, source->num_fds);
			g_array_remove_index(session->sources, i);
			/*
			 * This is a bit of a hack. To be removed when
			 * porting over to the GLib main loop.
			 */
			if (poll_object == (gintptr)session->ctx->libusb_ctx)
				session->ctx->usb_source_present = FALSE;

			sr_dbg("Removed event source %" G_GINTPTR_FORMAT ".",
				poll_object);
			return SR_OK;
		}
		fd_index += source->num_fds;
	}
	/* Trying to remove an already removed event source is problematic
	 * since the poll_object handle may have been reused in the meantime.
	 */
	sr_warn("Cannot remove non-existing event source %"
		G_GINTPTR_FORMAT ".", poll_object);

	return SR_ERR_BUG;
}

/**
 * Remove the source belonging to the specified file descriptor.
 *
 * @param session The session to use. Must not be NULL.
 * @param fd The file descriptor for which the source should be removed.
 *
 * @retval SR_OK Success
 * @retval SR_ERR_ARG Invalid argument
 * @retval SR_ERR_BUG Internal error.
 *
 * @since 0.3.0
 */
SR_API int sr_session_source_remove(struct sr_session *session, int fd)
{
	return sr_session_source_remove_internal(session, fd);
}

/**
 * Remove the source belonging to the specified poll descriptor.
 *
 * @param session The session to use. Must not be NULL.
 * @param pollfd The poll descriptor for which the source should be removed.
 *               Must not be NULL.
 * @return SR_OK upon success, SR_ERR_ARG upon invalid arguments, or
 *         SR_ERR_MALLOC upon memory allocation errors, SR_ERR_BUG upon
 *         internal errors.
 *
 * @since 0.2.0
 */
SR_API int sr_session_source_remove_pollfd(struct sr_session *session,
		GPollFD *pollfd)
{
	if (!pollfd) {
		sr_err("%s: pollfd was NULL", __func__);
		return SR_ERR_ARG;
	}
	return sr_session_source_remove_internal(session, (gintptr)pollfd);
}

/**
 * Remove the source belonging to the specified channel.
 *
 * @param session The session to use. Must not be NULL.
 * @param channel The channel for which the source should be removed.
 *                Must not be NULL.
 * @retval SR_OK Success.
 * @retval SR_ERR_ARG Invalid argument.
 * @return SR_ERR_BUG Internal error.
 *
 * @since 0.2.0
 */
SR_API int sr_session_source_remove_channel(struct sr_session *session,
		GIOChannel *channel)
{
	if (!channel) {
		sr_err("%s: channel was NULL", __func__);
		return SR_ERR_ARG;
	}
	return sr_session_source_remove_internal(session, (gintptr)channel);
}

static void copy_src(struct sr_config *src, struct sr_datafeed_meta *meta_copy)
{
	g_variant_ref(src->data);
	meta_copy->config = g_slist_append(meta_copy->config,
	                                   g_memdup(src, sizeof(struct sr_config)));
}

SR_PRIV int sr_packet_copy(const struct sr_datafeed_packet *packet,
		struct sr_datafeed_packet **copy)
{
	const struct sr_datafeed_meta *meta;
	struct sr_datafeed_meta *meta_copy;
	const struct sr_datafeed_logic *logic;
	struct sr_datafeed_logic *logic_copy;
	const struct sr_datafeed_analog *analog;
	struct sr_datafeed_analog *analog_copy;
	uint8_t *payload;

	*copy = g_malloc0(sizeof(struct sr_datafeed_packet));
	(*copy)->type = packet->type;

	switch (packet->type) {
	case SR_DF_TRIGGER:
	case SR_DF_END:
		/* No payload. */
		break;
	case SR_DF_HEADER:
		payload = g_malloc(sizeof(struct sr_datafeed_header));
		memcpy(payload, packet->payload, sizeof(struct sr_datafeed_header));
		(*copy)->payload = payload;
		break;
	case SR_DF_META:
		meta = packet->payload;
		meta_copy = g_malloc0(sizeof(struct sr_datafeed_meta));
		g_slist_foreach(meta->config, (GFunc)copy_src, meta_copy->config);
		(*copy)->payload = meta_copy;
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		logic_copy = g_malloc(sizeof(logic));
		logic_copy->length = logic->length;
		logic_copy->unitsize = logic->unitsize;
		memcpy(logic_copy->data, logic->data, logic->length * logic->unitsize);
		(*copy)->payload = logic_copy;
		break;
	case SR_DF_ANALOG:
		analog = packet->payload;
		analog_copy = g_malloc(sizeof(analog));
		analog_copy->channels = g_slist_copy(analog->channels);
		analog_copy->num_samples = analog->num_samples;
		analog_copy->mq = analog->mq;
		analog_copy->unit = analog->unit;
		analog_copy->mqflags = analog->mqflags;
		memcpy(analog_copy->data, analog->data,
				analog->num_samples * sizeof(float));
		(*copy)->payload = analog_copy;
		break;
	default:
		sr_err("Unknown packet type %d", packet->type);
		return SR_ERR;
	}

	return SR_OK;
}

void sr_packet_free(struct sr_datafeed_packet *packet)
{
	const struct sr_datafeed_meta *meta;
	const struct sr_datafeed_logic *logic;
	const struct sr_datafeed_analog *analog;
	struct sr_config *src;
	GSList *l;

	switch (packet->type) {
	case SR_DF_TRIGGER:
	case SR_DF_END:
		/* No payload. */
		break;
	case SR_DF_HEADER:
		/* Payload is a simple struct. */
		g_free((void *)packet->payload);
		break;
	case SR_DF_META:
		meta = packet->payload;
		for (l = meta->config; l; l = l->next) {
			src = l->data;
			g_variant_unref(src->data);
			g_free(src);
		}
		g_slist_free(meta->config);
		g_free((void *)packet->payload);
		break;
	case SR_DF_LOGIC:
		logic = packet->payload;
		g_free(logic->data);
		g_free((void *)packet->payload);
		break;
	case SR_DF_ANALOG:
		analog = packet->payload;
		g_slist_free(analog->channels);
		g_free(analog->data);
		g_free((void *)packet->payload);
		break;
	default:
		sr_err("Unknown packet type %d", packet->type);
	}
	g_free(packet);

}

/** @} */
