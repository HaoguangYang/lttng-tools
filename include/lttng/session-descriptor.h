/*
 * Copyright (C) 2019 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_SESSION_DESCRIPTOR_H
#define LTTNG_SESSION_DESCRIPTOR_H

#include <lttng/lttng-export.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lttng_session_descriptor;

/*
 * Session descriptor API.
 *
 * A session descriptor is an object describing the immutable configuration
 * options of an LTTng tracing session.
 *
 * When used with the lttng_create_session_ext() function, a session descriptor
 * allows the creation of a tracing session of the following types: regular,
 * snapshot, and live.
 *
 * Certain parameters can be omitted at the time of creation of a session
 * descriptor to use default or auto-generated values selected by the
 * session daemon. For instance, a session's name can be left unspecified,
 * in which case one that is guaranteed not to clash with pre-existing
 * sessions will be automatically be generated by the session daemon.
 *
 * Most session descriptors can be created in either "no output", local, or
 * network output modes. The various output modes supported vary by session
 * type.
 *
 * Regular session creation functions and output modes:
 *   * "no output": lttng_session_descriptor_create()
 *   * local:       lttng_session_descriptor_local_create()
 *   * network:     lttng_session_descriptor_network_create()
 *
 * Snapshot session creation functions and output modes:
 *   * "no output": lttng_session_descriptor_snapshot_create()
 *   * local:       lttng_session_descriptor_snapshot_local_create()
 *   * network:     lttng_session_descriptor_snapshot_network_create()
 *
 * Live session creation functions and output modes:
 *   * "no output": lttng_session_descriptor_live_create()
 *   * network:     lttng_session_descriptor_live_network_create()
 *
 * Local output functions accept a 'path' parameter that must be an absolute
 * path to which the user has write access. When a local output is automatically
 * generated, it adopts the form:
 *   $LTTNG_HOME/DEFAULT_TRACE_DIR_NAME/SESSION_NAME-CREATION_TIME
 *
 * Where CREATION_TIME is time of the creation of the session on the session
 * daemon in the form "yyyymmdd-hhmmss".
 *
 * Network output locations can also be auto-generated by leaving the
 * 'control_url' and 'data_url' output parameters unspecified. In such cases,
 * the session daemon will create a default output targeting a relay daemon
 * at net://127.0.0.1, using the default 'control' and 'data' ports.
 *
 * The format of the 'control_url' and 'data_url' parameters is:
 *   NETPROTO://(HOST | IPADDR)[:CTRLPORT[:DATAPORT]][/TRACEPATH]
 *
 * NETPROTO: Network protocol, amongst:
 *   * net:  TCP over IPv4; the default values of 'CTRLPORT' and 'DATAPORT'
 *           are defined at build time of the lttng toolchain.
 *   * net6: TCP over IPv6: same default ports as the 'net' protocol.
 *   * tcp:  Same as the 'net' protocol.
 *   * tcp6: Same as the 'net6' protocol.
 *
 * HOST | IPADDR:  Hostname or IP address (IPv6 address *must* be enclosed
 *                 in brackets; see RFC 2732).
 *
 * CTRLPORT: Control port.
 *
 * DATAPORT: Data port.
 *
 * TRACEPATH: Path of trace files on the remote file system. This path is
 *            relative to the base output directory set on the relay daemon
 *            end.
 *
 * The 'data_url' parameter is optional:
 *   * This parameter is meaningless for local tracing.
 *   * If 'control_url' is specified and a network protocol is used, the
 *     default data port, and the 'control_url' host will be used.
 */

enum lttng_session_descriptor_status {
	/* Invalid session descriptor parameter. */
	LTTNG_SESSION_DESCRIPTOR_STATUS_INVALID = -1,
	LTTNG_SESSION_DESCRIPTOR_STATUS_OK = 0,
	/* Session descriptor parameter is unset. */
	LTTNG_SESSION_DESCRIPTOR_STATUS_UNSET = 1,
};

/*
 * Create a session descriptor in no-output mode.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_create(const char *name);

/*
 * Create a session descriptor with a local output destination.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'path' must either be an absolute path or it can be left NULL to
 * use the default local output destination.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_local_create(const char *name, const char *path);

/*
 * Create a session descriptor with a remote output destination.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'control_url' and 'data_url' must conform to the URL format
 * described above or can be left NULL to use the default network output.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *lttng_session_descriptor_network_create(
	const char *name, const char *control_url, const char *data_url);

/*
 * Create a snapshot session descriptor without a default output.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_snapshot_create(const char *name);

/*
 * Create a snapshot session descriptor with a local output destination.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'path' must either be an absolute path or it can be left NULL to
 * use the default local output destination as the default snapshot output.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_snapshot_local_create(const char *name, const char *path);

/*
 * Create a snapshot session descriptor with a remote output destination.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'control_url' and 'data_url' must conform to the URL format
 * described above or can be left NULL to use the default network output as
 * the default snapshot output.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_snapshot_network_create(const char *name,
						 const char *control_url,
						 const char *data_url);

/*
 * Create a live session descriptor without an output.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'live_timer_interval_us' parameter is the live timer's period, specified
 * in microseconds.
 *
 * This parameter can't be 0. There is no default value defined for a live
 * timer's period.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_live_create(const char *name, unsigned long long live_timer_interval_us);

/*
 * Create a live session descriptor with a remote output destination.
 *
 * The 'name' parameter can be left NULL to auto-generate a session name.
 *
 * The 'control_url' and 'data_url' must conform to the URL format
 * described above or can be left NULL to use the default network output.
 *
 * The 'live_timer_interval_us' parameter is the live timer's period, specified
 * in microseconds.
 *
 * This parameter can't be 0. There is no default value defined for a live
 * timer's period.
 *
 * Returns an lttng_session_descriptor instance on success, NULL on error.
 */
LTTNG_EXPORT extern struct lttng_session_descriptor *
lttng_session_descriptor_live_network_create(const char *name,
					     const char *control_url,
					     const char *data_url,
					     unsigned long long live_timer_interval_us);

/*
 * Get a session descriptor's session name.
 *
 * The 'name' parameter is used as an output parameter and will point to
 * the session descriptor's session name on success
 * (LTTNG_SESSION_DESCRIPTOR_STATUS_OK). Its content of is left unspecified
 * for other return codes. The pointer returned through 'name' is only
 * guaranteed to remain valid until the next method call on the session
 * descriptor.
 *
 * Returns LTTNG_SESSION_DESCRIPTOR_STATUS_OK on success,
 * LTTNG_SESSION_DESCRIPTOR_STATUS_INVALID if 'descriptor' or 'name' are
 * NULL, and LTTNG_SESSION_DESCRIPTOR_STATUS_UNSET if the descriptor's
 * name parameter is unset.
 */
LTTNG_EXPORT extern enum lttng_session_descriptor_status
lttng_session_descriptor_get_session_name(const struct lttng_session_descriptor *descriptor,
					  const char **name);

/*
 * Destroy a local lttng_session object.
 *
 * This does not destroy the session on the session daemon; it releases
 * the resources allocated by the descriptor object.
 */
LTTNG_EXPORT extern void
lttng_session_descriptor_destroy(struct lttng_session_descriptor *descriptor);

#ifdef __cplusplus
}
#endif

#endif /* LTTNG_SESSION_DESCRIPTOR_H */
