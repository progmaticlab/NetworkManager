/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2004 - 2012 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#include <glib.h>
#include <string.h>

#include "nm-dispatcher.h"
#include "nm-dispatcher-api.h"
#include "NetworkManagerUtils.h"
#include "nm-utils.h"
#include "nm-logging.h"
#include "nm-dbus-manager.h"
#include "nm-dbus-glib-types.h"
#include "nm-glib-compat.h"

static gboolean do_dispatch = TRUE;
static GHashTable *requests = NULL;

static void
dump_object_to_props (GObject *object, GHashTable *hash)
{
	GParamSpec **pspecs;
	guint len = 0, i;

	pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (object), &len);
	for (i = 0; i < len; i++) {
		value_hash_add_object_property (hash,
		                                pspecs[i]->name,
		                                object,
		                                pspecs[i]->name,
		                                pspecs[i]->value_type);
	}
	g_free (pspecs);
}

static void
dump_dhcp4_to_props (NMDHCP4Config *config, GHashTable *hash)
{
	GSList *options, *iter;

	options = nm_dhcp4_config_list_options (config);
	for (iter = options; iter; iter = g_slist_next (iter)) {
		const char *option = (const char *) iter->data;
		const char *val;

		val = nm_dhcp4_config_get_option (config, option);
		value_hash_add_str (hash, option, val);
	}
	g_slist_free (options);
}

static void
dump_dhcp6_to_props (NMDHCP6Config *config, GHashTable *hash)
{
	GSList *options, *iter;

	options = nm_dhcp6_config_list_options (config);
	for (iter = options; iter; iter = g_slist_next (iter)) {
		const char *option = (const char *) iter->data;
		const char *val;

		val = nm_dhcp6_config_get_option (config, option);
		value_hash_add_str (hash, option, val);
	}
	g_slist_free (options);
}

static void
fill_device_props (NMDevice *device,
                   GHashTable *dev_hash,
                   GHashTable *ip4_hash,
                   GHashTable *ip6_hash,
                   GHashTable *dhcp4_hash,
                   GHashTable *dhcp6_hash)
{
	NMIP4Config *ip4_config;
	NMIP6Config *ip6_config;
	NMDHCP4Config *dhcp4_config;
	NMDHCP6Config *dhcp6_config;

	/* If the action is for a VPN, send the VPN's IP interface instead of the device's */
	value_hash_add_str (dev_hash, NMD_DEVICE_PROPS_IP_INTERFACE, nm_device_get_ip_iface (device));
	value_hash_add_str (dev_hash, NMD_DEVICE_PROPS_INTERFACE, nm_device_get_iface (device));
	value_hash_add_uint (dev_hash, NMD_DEVICE_PROPS_TYPE, nm_device_get_device_type (device));
	value_hash_add_uint (dev_hash, NMD_DEVICE_PROPS_STATE, nm_device_get_state (device));
	value_hash_add_object_path (dev_hash, NMD_DEVICE_PROPS_PATH, nm_device_get_path (device));

	ip4_config = nm_device_get_ip4_config (device);
	if (ip4_config)
		dump_object_to_props (G_OBJECT (ip4_config), ip4_hash);

	ip6_config = nm_device_get_ip6_config (device);
	if (ip6_config)
		dump_object_to_props (G_OBJECT (ip6_config), ip6_hash);

	dhcp4_config = nm_device_get_dhcp4_config (device);
	if (dhcp4_config)
		dump_dhcp4_to_props (dhcp4_config, dhcp4_hash);

	dhcp6_config = nm_device_get_dhcp6_config (device);
	if (dhcp6_config)
		dump_dhcp6_to_props (dhcp6_config, dhcp6_hash);
}

static void
fill_vpn_props (NMIP4Config *ip4_config,
                NMIP6Config *ip6_config,
                GHashTable *ip4_hash,
                GHashTable *ip6_hash)
{
	if (ip4_config)
		dump_object_to_props (G_OBJECT (ip4_config), ip4_hash);
	if (ip6_config)
		dump_object_to_props (G_OBJECT (ip6_config), ip6_hash);
}

typedef struct {
	guint request_id;
	DispatcherFunc callback;
	gpointer user_data;
	guint idle_id;
} DispatchInfo;

static void
dispatcher_info_free (DispatchInfo *info)
{
	if (info->idle_id)
		g_source_remove (info->idle_id);
	g_free (info);
}

static void
_ensure_requests (void)
{
	if (G_UNLIKELY (requests == NULL)) {
		requests = g_hash_table_new_full (g_direct_hash,
		                                  g_direct_equal,
		                                  NULL,
		                                  (GDestroyNotify) dispatcher_info_free);
	}
}

static void
dispatcher_info_cleanup (DispatchInfo *info)
{
	g_hash_table_remove (requests, GUINT_TO_POINTER (info->request_id));
}

static const char *
dispatch_result_to_string (DispatchResult result)
{
	switch (result) {
	case DISPATCH_RESULT_UNKNOWN:
		return "unknown";
	case DISPATCH_RESULT_SUCCESS:
		return "success";
	case DISPATCH_RESULT_EXEC_FAILED:
		return "exec failed";
	case DISPATCH_RESULT_FAILED:
		return "failed";
	case DISPATCH_RESULT_TIMEOUT:
		return "timed out";
	}
	g_assert_not_reached ();
}

static void
dispatcher_results_process (GPtrArray *results)
{
	guint i;

	g_return_if_fail (results != NULL);

	for (i = 0; i < results->len; i++) {
		GValueArray *item = g_ptr_array_index (results, i);
		GValue *tmp;
		const char *script, *err;
		DispatchResult result;

		if (item->n_values != 3) {
			nm_log_dbg (LOGD_DISPATCH, "Unexpected number of items in "
			            "dispatcher result (got %d, expectd 3)",
			            item->n_values);
			continue;
		}

		/* Script */
		tmp = g_value_array_get_nth (item, 0);
		if (G_VALUE_TYPE (tmp) != G_TYPE_STRING) {
			nm_log_dbg (LOGD_DISPATCH, "Dispatcher result %d element 0 invalid type %s",
			            i, G_VALUE_TYPE_NAME (tmp));
			continue;
		}
		script = g_value_get_string (tmp);

		/* Result */
		tmp = g_value_array_get_nth (item, 1);
		if (G_VALUE_TYPE (tmp) != G_TYPE_UINT) {
			nm_log_dbg (LOGD_DISPATCH, "Dispatcher result %d element 1 invalid type %s",
			            i, G_VALUE_TYPE_NAME (tmp));
			continue;
		}
		result = g_value_get_uint (tmp);

		/* Error */
		tmp = g_value_array_get_nth (item, 2);
		if (G_VALUE_TYPE (tmp) != G_TYPE_STRING) {
			nm_log_dbg (LOGD_DISPATCH, "Dispatcher result %d element 2 invalid type %s",
			            i, G_VALUE_TYPE_NAME (tmp));
			continue;
		}
		err = g_value_get_string (tmp);

		if (result == DISPATCH_RESULT_SUCCESS)
			nm_log_dbg (LOGD_DISPATCH, "Dispatcher script \"%s\" succeeded", script);
		else {
			nm_log_warn (LOGD_DISPATCH, "Dispatcher script \"%s\" failed with %s: %s",
			             script, dispatch_result_to_string (result), err);
		}
	}
}

static void
free_results (GPtrArray *results)
{
	g_return_if_fail (results != NULL);
	g_ptr_array_foreach (results, (GFunc) g_value_array_free, NULL);
	g_ptr_array_free (results, TRUE);
}

static void
dispatcher_done_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer user_data)
{
	DispatchInfo *info = user_data;
	GError *error = NULL;
	GPtrArray *results = NULL;

	if (dbus_g_proxy_end_call (proxy, call, &error,
	                           DISPATCHER_TYPE_RESULT_ARRAY, &results,
	                           G_TYPE_INVALID)) {
		dispatcher_results_process (results);
		free_results (results);
	} else {
		g_assert (error);
		nm_log_warn (LOGD_DISPATCH, "Dispatcher failed: (%d) %s", error->code, error->message);
	}

	if (info->callback)
		info->callback (info->request_id, info->user_data);

	g_clear_error (&error);
	g_object_unref (proxy);
}

static const char *action_table[] = {
	[DISPATCHER_ACTION_HOSTNAME]     = NMD_ACTION_HOSTNAME,
	[DISPATCHER_ACTION_PRE_UP]       = NMD_ACTION_PRE_UP,
	[DISPATCHER_ACTION_UP]           = NMD_ACTION_UP,
	[DISPATCHER_ACTION_PRE_DOWN]     = NMD_ACTION_PRE_DOWN,
	[DISPATCHER_ACTION_DOWN]         = NMD_ACTION_DOWN,
	[DISPATCHER_ACTION_VPN_PRE_UP]   = NMD_ACTION_VPN_PRE_UP,
	[DISPATCHER_ACTION_VPN_UP]       = NMD_ACTION_VPN_UP,
	[DISPATCHER_ACTION_VPN_PRE_DOWN] = NMD_ACTION_VPN_PRE_DOWN,
	[DISPATCHER_ACTION_VPN_DOWN]     = NMD_ACTION_VPN_DOWN,
	[DISPATCHER_ACTION_DHCP4_CHANGE] = NMD_ACTION_DHCP4_CHANGE,
	[DISPATCHER_ACTION_DHCP6_CHANGE] = NMD_ACTION_DHCP6_CHANGE,
};

static const char *
action_to_string (DispatcherAction action)
{
	g_assert (action >= 0 && action < G_N_ELEMENTS (action_table));
	return action_table[action];
}

static gboolean
dispatcher_idle_cb (gpointer user_data)
{
	DispatchInfo *info = user_data;

	info->idle_id = 0;
	if (info->callback)
		info->callback (info->request_id, info->user_data);
	dispatcher_info_cleanup (info);
	return G_SOURCE_REMOVE;
}

static gboolean
_dispatcher_call (DispatcherAction action,
                  gboolean blocking,
                  NMConnection *connection,
                  NMDevice *device,
                  const char *vpn_iface,
                  NMIP4Config *vpn_ip4_config,
                  NMIP6Config *vpn_ip6_config,
                  DispatcherFunc callback,
                  gpointer user_data,
                  guint *out_call_id)
{
	DBusGProxy *proxy;
	DBusGConnection *g_connection;
	GHashTable *connection_hash;
	GHashTable *connection_props;
	GHashTable *device_props;
	GHashTable *device_ip4_props;
	GHashTable *device_ip6_props;
	GHashTable *device_dhcp4_props;
	GHashTable *device_dhcp6_props;
	GHashTable *vpn_ip4_props;
	GHashTable *vpn_ip6_props;
	DispatchInfo *info = NULL;
	gboolean success = FALSE;
	GError *error = NULL;
	static guint request_counter = 1;

	g_assert (!blocking || (!callback && !user_data));

	_ensure_requests ();

	/* All actions except 'hostname' require a device */
	if (action == DISPATCHER_ACTION_HOSTNAME) {
		nm_log_dbg (LOGD_DISPATCH, "dispatching action '%s'",
		            action_to_string (action));
	} else {
		g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

		nm_log_dbg (LOGD_DISPATCH, "(%s) dispatching action '%s'",
		            nm_device_get_iface (device), action_to_string (action));
	}

	/* VPN actions require at least an IPv4 config (for now) */
	if (action == DISPATCHER_ACTION_VPN_UP)
		g_return_val_if_fail (vpn_ip4_config != NULL, FALSE);

	if (do_dispatch == FALSE) {
		if (blocking == FALSE && (out_call_id || callback)) {
			info = g_malloc0 (sizeof (*info));
			info->request_id = request_counter++;
			info->callback = callback;
			info->user_data = user_data;
			info->idle_id = g_idle_add (dispatcher_idle_cb, info);
		}
		nm_log_dbg (LOGD_DISPATCH, "ignoring request; no scripts in " NMD_SCRIPT_DIR);
		success = TRUE;
		goto done;
	}

	g_connection = nm_dbus_manager_get_connection (nm_dbus_manager_get ());
	proxy = dbus_g_proxy_new_for_name (g_connection,
	                                   NM_DISPATCHER_DBUS_SERVICE,
	                                   NM_DISPATCHER_DBUS_PATH,
	                                   NM_DISPATCHER_DBUS_IFACE);
	if (!proxy) {
		nm_log_err (LOGD_DISPATCH, "could not get dispatcher proxy!");
		return FALSE;
	}

	if (connection) {
		connection_hash = nm_connection_to_hash (connection, NM_SETTING_HASH_FLAG_NO_SECRETS);

		connection_props = value_hash_create ();
		value_hash_add_object_path (connection_props,
		                            NMD_CONNECTION_PROPS_PATH,
		                            nm_connection_get_path (connection));
	} else {
		connection_hash = value_hash_create ();
		connection_props = value_hash_create ();
	}

	device_props = value_hash_create ();
	device_ip4_props = value_hash_create ();
	device_ip6_props = value_hash_create ();
	device_dhcp4_props = value_hash_create ();
	device_dhcp6_props = value_hash_create ();
	vpn_ip4_props = value_hash_create ();
	vpn_ip6_props = value_hash_create ();

	/* hostname actions only send the hostname */
	if (action != DISPATCHER_ACTION_HOSTNAME) {
		fill_device_props (device,
		                   device_props,
		                   device_ip4_props,
		                   device_ip6_props,
		                   device_dhcp4_props,
		                   device_dhcp6_props);
		if (vpn_ip4_config || vpn_ip6_config)
			fill_vpn_props (vpn_ip4_config, vpn_ip6_config, vpn_ip4_props, vpn_ip6_props);
	}

	/* Send the action to the dispatcher */
	if (blocking) {
		GPtrArray *results = NULL;

		success = dbus_g_proxy_call_with_timeout (proxy, "Action",
		                                          30000,
		                                          &error,
		                                          G_TYPE_STRING, action_to_string (action),
		                                          DBUS_TYPE_G_MAP_OF_MAP_OF_VARIANT, connection_hash,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, connection_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, device_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, device_ip4_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, device_ip6_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp4_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp6_props,
		                                          G_TYPE_STRING, vpn_iface ? vpn_iface : "",
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip4_props,
		                                          DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip6_props,
		                                          G_TYPE_BOOLEAN, nm_logging_enabled (LOGL_DEBUG, LOGD_DISPATCH),
		                                          G_TYPE_INVALID,
		                                          DISPATCHER_TYPE_RESULT_ARRAY, &results,
		                                          G_TYPE_INVALID);
		if (success) {
			dispatcher_results_process (results);
			free_results (results);
		} else {
			nm_log_warn (LOGD_DISPATCH, "Dispatcher failed: (%d) %s", error->code, error->message);
			g_error_free (error);
		}
	} else {
		info = g_malloc0 (sizeof (*info));
		info->request_id = request_counter++;
		info->callback = callback;
		info->user_data = user_data;
		dbus_g_proxy_begin_call_with_timeout (proxy, "Action",
		                                      dispatcher_done_cb,
		                                      info,
		                                      (GDestroyNotify) dispatcher_info_cleanup,
		                                      30000,
		                                      G_TYPE_STRING, action_to_string (action),
		                                      DBUS_TYPE_G_MAP_OF_MAP_OF_VARIANT, connection_hash,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, connection_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_ip4_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_ip6_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp4_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp6_props,
		                                      G_TYPE_STRING, vpn_iface ? vpn_iface : "",
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip4_props,
		                                      DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip6_props,
		                                      G_TYPE_BOOLEAN, nm_logging_enabled (LOGL_DEBUG, LOGD_DISPATCH),
		                                      G_TYPE_INVALID);
		success = TRUE;
	}

	g_hash_table_destroy (connection_hash);
	g_hash_table_destroy (connection_props);
	g_hash_table_destroy (device_props);
	g_hash_table_destroy (device_ip4_props);
	g_hash_table_destroy (device_ip6_props);
	g_hash_table_destroy (device_dhcp4_props);
	g_hash_table_destroy (device_dhcp6_props);
	g_hash_table_destroy (vpn_ip4_props);
	g_hash_table_destroy (vpn_ip6_props);

done:
	if (success && info) {
		/* Track the request in case of cancelation */
		g_hash_table_insert (requests, GUINT_TO_POINTER (info->request_id), info);
		if (out_call_id)
			*out_call_id = info->request_id;
	} else if (out_call_id)
		*out_call_id = 0;

	return success;
}

/**
 * nm_dispatcher_call:
 * @action: the %DispatcherAction
 * @connection: the #NMConnection the action applies to
 * @device: the #NMDevice the action applies to
 * @callback: a caller-supplied callback to execute when done
 * @user_data: caller-supplied pointer passed to @callback
 * @out_call_id: on success, a call identifier which can be passed to
 * nm_dispatcher_call_cancel()
 *
 * This method always invokes the dispatcher action asynchronously.  To ignore
 * the result, pass %NULL to @callback.
 *
 * Returns: %TRUE if the action was dispatched, %FALSE on failure
 */
gboolean
nm_dispatcher_call (DispatcherAction action,
                    NMConnection *connection,
                    NMDevice *device,
                    DispatcherFunc callback,
                    gpointer user_data,
                    guint *out_call_id)
{
	return _dispatcher_call (action, FALSE, connection, device, NULL, NULL,
	                         NULL, callback, user_data, out_call_id);
}

/**
 * nm_dispatcher_call_sync():
 * @action: the %DispatcherAction
 * @connection: the #NMConnection the action applies to
 * @device: the #NMDevice the action applies to
 *
 * This method always invokes the dispatcher action synchronously and it may
 * take a long time to return.
 *
 * Returns: %TRUE if the action was dispatched, %FALSE on failure
 */
gboolean
nm_dispatcher_call_sync (DispatcherAction action,
                         NMConnection *connection,
                         NMDevice *device)
{
	return _dispatcher_call (action, TRUE, connection, device, NULL, NULL,
	                         NULL, NULL, NULL, NULL);
}

/**
 * nm_dispatcher_call_vpn():
 * @action: the %DispatcherAction
 * @connection: the #NMConnection the action applies to
 * @parent_device: the parent #NMDevice of the VPN connection
 * @vpn_iface: the IP interface of the VPN tunnel, if any
 * @vpn_ip4_config: the #NMIP4Config of the VPN connection
 * @vpn_ip6_config: the #NMIP6Config of the VPN connection
 * @callback: a caller-supplied callback to execute when done
 * @user_data: caller-supplied pointer passed to @callback
 * @out_call_id: on success, a call identifier which can be passed to
 * nm_dispatcher_call_cancel()
 *
 * This method always invokes the dispatcher action asynchronously.  To ignore
 * the result, pass %NULL to @callback.
 *
 * Returns: %TRUE if the action was dispatched, %FALSE on failure
 */
gboolean
nm_dispatcher_call_vpn (DispatcherAction action,
                        NMConnection *connection,
                        NMDevice *parent_device,
                        const char *vpn_iface,
                        NMIP4Config *vpn_ip4_config,
                        NMIP6Config *vpn_ip6_config,
                        DispatcherFunc callback,
                        gpointer user_data,
                        guint *out_call_id)
{
	return _dispatcher_call (action, FALSE, connection, parent_device, vpn_iface,
	                         vpn_ip4_config, vpn_ip6_config, callback, user_data, out_call_id);
}

/**
 * nm_dispatcher_call_vpn_sync():
 * @action: the %DispatcherAction
 * @connection: the #NMConnection the action applies to
 * @parent_device: the parent #NMDevice of the VPN connection
 * @vpn_iface: the IP interface of the VPN tunnel, if any
 * @vpn_ip4_config: the #NMIP4Config of the VPN connection
 * @vpn_ip6_config: the #NMIP6Config of the VPN connection
 *
 * This method always invokes the dispatcher action synchronously and it may
 * take a long time to return.
 *
 * Returns: %TRUE if the action was dispatched, %FALSE on failure
 */
gboolean
nm_dispatcher_call_vpn_sync (DispatcherAction action,
                             NMConnection *connection,
                             NMDevice *parent_device,
                             const char *vpn_iface,
                             NMIP4Config *vpn_ip4_config,
                             NMIP6Config *vpn_ip6_config)
{
	return _dispatcher_call (action, TRUE, connection, parent_device, vpn_iface,
	                         vpn_ip4_config, vpn_ip6_config, NULL, NULL, NULL);
}

void
nm_dispatcher_call_cancel (guint call_id)
{
	DispatchInfo *info;

	_ensure_requests ();

	/* Canceling just means the callback doesn't get called, so set the
	 * DispatcherInfo's callback to NULL.
	 */
	info = g_hash_table_lookup (requests, GUINT_TO_POINTER (call_id));
	if (info)
		info->callback = NULL;
	else
		g_return_if_reached ();
}

typedef struct {
	const char *dir;
	GFileMonitor *monitor;
	gboolean has_scripts;
} Monitor;

static Monitor monitors[3] = {
	{ NMD_SCRIPT_DIR,   NULL, TRUE },
	{ NMD_PRE_UP_DIR,   NULL, TRUE },
	{ NMD_PRE_DOWN_DIR, NULL, TRUE }
};

static void
dispatcher_dir_changed (GFileMonitor *monitor,
                        GFile *file,
                        GFile *other_file,
                        GFileMonitorEvent event_type,
                        Monitor *item)
{
	GDir *dir;
	guint i;

	/* Default to dispatching on any errors */
	item->has_scripts = TRUE;

	dir = g_dir_open (item->dir, 0, NULL);
	if (dir) {
		item->has_scripts = !!g_dir_read_name (dir);
		g_dir_close (dir);
	}

	/* Recheck all dirs for scripts and update global variable */
	do_dispatch = FALSE;
	for (i = 0; i < G_N_ELEMENTS (monitors); i++)
		do_dispatch |= monitors[i].has_scripts;
}

void
nm_dispatcher_init (void)
{
	GFile *file;
	guint i;

	for (i = 0; i < G_N_ELEMENTS (monitors); i++) {
		file = g_file_new_for_path (monitors[i].dir);
		monitors[i].monitor = g_file_monitor_directory (file, G_FILE_MONITOR_NONE, NULL, NULL);
		if (monitors[i].monitor) {
			g_signal_connect (monitors[i].monitor, "changed", G_CALLBACK (dispatcher_dir_changed), &monitors[i]);
			dispatcher_dir_changed (monitors[i].monitor, file, NULL, 0, &monitors[i]);
		}
		g_object_unref (file);
	}
}

