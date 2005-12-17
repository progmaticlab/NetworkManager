/* NetworkManager Wireless Applet -- Display wireless access points and allow user control
 *
 * Dan Williams <dcbw@redhat.com>
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * (C) Copyright 2005 Red Hat, Inc.
 */


#include <dbus/dbus.h>
#include <glib.h>
#include <iwlib.h>

#include "dbus-helpers.h"
#include "cipher.h"


static dbus_bool_t key_append_helper (DBusMessageIter *iter, const char *key)
{
	DBusMessageIter	subiter;
	int				key_len;

	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);

	key_len = strlen (key);
	g_return_val_if_fail (key_len > 0, FALSE);

	if (!dbus_message_iter_open_container (iter, DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE_AS_STRING, &subiter))
		return FALSE;
	dbus_message_iter_append_fixed_array (&subiter, DBUS_TYPE_BYTE, &key, key_len);
	dbus_message_iter_close_container (iter, &subiter);

	return TRUE;
}

static void we_cipher_append_helper (DBusMessageIter *iter, int we_cipher)
{
	dbus_int32_t	dbus_we_cipher = (dbus_int32_t) we_cipher;

	g_return_if_fail (iter != NULL);

	dbus_message_iter_append_basic (iter, DBUS_TYPE_INT32, &dbus_we_cipher);
}

dbus_bool_t
nmu_security_serialize_wep (DBusMessageIter *iter,
					   const char *key,
					   int auth_alg)
{
	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail ((auth_alg == IW_AUTH_ALG_OPEN_SYSTEM) || (auth_alg == IW_AUTH_ALG_SHARED_KEY), FALSE);

	/* Second arg: hashed key (ARRAY, BYTE) */
	if (!key_append_helper (iter, key))
		return FALSE;

	/* Third arg: WEP authentication algorithm (INT32) */
	dbus_message_iter_append_basic (iter, DBUS_TYPE_INT32, &auth_alg);

	return TRUE;
}

dbus_bool_t
nmu_security_deserialize_wep (DBusMessageIter *iter,
						char **key,
						int *key_len,
						int *auth_alg)
{
	DBusMessageIter	subiter;
	char *			dbus_key;
	int				dbus_key_len;
	dbus_int32_t		dbus_auth_alg;

	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (*key == NULL, FALSE);
	g_return_val_if_fail (key_len != NULL, FALSE);
	g_return_val_if_fail (auth_alg != NULL, FALSE);

	/* Next arg: key (ARRAY, BYTE) */
	g_return_val_if_fail ((dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_ARRAY)
			&& (dbus_message_iter_get_element_type (iter) == DBUS_TYPE_BYTE), FALSE);

	dbus_message_iter_recurse (iter, &subiter);
	dbus_message_iter_get_fixed_array (&subiter, &dbus_key, &dbus_key_len);
	g_return_val_if_fail (dbus_key_len > 0, FALSE);

	/* Next arg: authentication algorithm (INT32) */
	g_return_val_if_fail (dbus_message_iter_next (iter), FALSE);
	g_return_val_if_fail (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_INT32, FALSE);

	dbus_message_iter_get_basic (iter, &dbus_auth_alg);
	g_return_val_if_fail ((dbus_auth_alg == IW_AUTH_ALG_OPEN_SYSTEM)
			|| (dbus_auth_alg == IW_AUTH_ALG_SHARED_KEY), FALSE);

	*key = dbus_key;
	*key_len = dbus_key_len;
	*auth_alg = dbus_auth_alg;
	return TRUE;
}


dbus_bool_t
nmu_security_serialize_wep_with_cipher (DBusMessage *message,
					IEEE_802_11_Cipher *cipher,
					const char *ssid,
					const char *input,
					int auth_alg)
{
	char *			key = NULL;
	dbus_bool_t		result = TRUE;
	DBusMessageIter	iter;

	g_return_val_if_fail (message != NULL, FALSE);
	g_return_val_if_fail (cipher != NULL, FALSE);
	g_return_val_if_fail ((auth_alg == IW_AUTH_ALG_OPEN_SYSTEM) || (auth_alg == IW_AUTH_ALG_SHARED_KEY), FALSE);

	dbus_message_iter_init_append (message, &iter);

	/* First arg: WE Cipher (INT32) */
	we_cipher_append_helper (&iter, ieee_802_11_cipher_get_we_cipher (cipher));

	key = ieee_802_11_cipher_hash (cipher, ssid, input);
	result = nmu_security_serialize_wep (&iter, key, auth_alg);
	g_free (key);

	return result;
}


dbus_bool_t
nmu_security_serialize_wpa_psk (DBusMessageIter *iter,
					const char *key,
					int wpa_version,
					int key_mgt)
{
	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail ((wpa_version == IW_AUTH_WPA_VERSION_WPA) || (wpa_version == IW_AUTH_WPA_VERSION_WPA2), FALSE);
	g_return_val_if_fail ((key_mgt == IW_AUTH_KEY_MGMT_802_1X) || (key_mgt == IW_AUTH_KEY_MGMT_PSK), FALSE);

	/* Second arg: hashed key (ARRAY, BYTE) */
	if (!key_append_helper (iter, key))
		return FALSE;

	/* Third arg: WPA version (INT32) */
	dbus_message_iter_append_basic (iter, DBUS_TYPE_INT32, &wpa_version);

	/* Fourth arg: WPA key management (INT32) */
	dbus_message_iter_append_basic (iter, DBUS_TYPE_INT32, &key_mgt);

	return TRUE;
}

dbus_bool_t
nmu_security_deserialize_wpa_psk (DBusMessageIter *iter,
						char **key,
						int *key_len,
						int *wpa_version,
						int *key_mgt)
{
	DBusMessageIter	subiter;
	char *			dbus_key;
	int				dbus_key_len;
	dbus_int32_t		dbus_wpa_version;
	dbus_int32_t		dbus_key_mgt;

	g_return_val_if_fail (iter != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (*key == NULL, FALSE);
	g_return_val_if_fail (key_len != NULL, FALSE);
	g_return_val_if_fail (wpa_version != NULL, FALSE);
	g_return_val_if_fail (key_mgt != NULL, FALSE);

	/* Next arg: key (ARRAY, BYTE) */
	g_return_val_if_fail ((dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_ARRAY)
			&& (dbus_message_iter_get_element_type (iter) == DBUS_TYPE_BYTE), FALSE);

	dbus_message_iter_recurse (iter, &subiter);
	dbus_message_iter_get_fixed_array (&subiter, &dbus_key, &dbus_key_len);
	g_return_val_if_fail (dbus_key_len > 0, FALSE);

	/* Next arg: WPA version (INT32) */
	g_return_val_if_fail (dbus_message_iter_next (iter), FALSE);
	g_return_val_if_fail (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_INT32, FALSE);

	dbus_message_iter_get_basic (iter, &dbus_wpa_version);
	g_return_val_if_fail ((dbus_wpa_version == IW_AUTH_WPA_VERSION_WPA)
			|| (dbus_wpa_version == IW_AUTH_WPA_VERSION_WPA2), FALSE);

	/* Next arg: WPA key management (INT32) */
	g_return_val_if_fail (dbus_message_iter_next (iter), FALSE);
	g_return_val_if_fail (dbus_message_iter_get_arg_type (iter) == DBUS_TYPE_INT32, FALSE);

	dbus_message_iter_get_basic (iter, &dbus_key_mgt);
	g_return_val_if_fail ((dbus_key_mgt == IW_AUTH_KEY_MGMT_802_1X)
			|| (dbus_key_mgt == IW_AUTH_KEY_MGMT_PSK), FALSE);

	*key = dbus_key;
	*key_len = dbus_key_len;
	*wpa_version = dbus_wpa_version;
	*key_mgt = dbus_key_mgt;
	return TRUE;
}

dbus_bool_t
nmu_security_serialize_wpa_psk_with_cipher (DBusMessage *message,
					IEEE_802_11_Cipher *cipher,
					const char *ssid,
					const char *input,
					int wpa_version,
					int key_mgt)
{
	char *			key = NULL;
	dbus_bool_t		result = TRUE;
	DBusMessageIter	iter;

	g_return_val_if_fail (message != NULL, FALSE);
	g_return_val_if_fail (cipher != NULL, FALSE);
	g_return_val_if_fail ((wpa_version == IW_AUTH_WPA_VERSION_WPA) || (wpa_version == IW_AUTH_WPA_VERSION_WPA2), FALSE);
	g_return_val_if_fail ((key_mgt == IW_AUTH_KEY_MGMT_802_1X) || (key_mgt == IW_AUTH_KEY_MGMT_PSK), FALSE);

	dbus_message_iter_init_append (message, &iter);

	/* First arg: WE Cipher (INT32) */
	we_cipher_append_helper (&iter, ieee_802_11_cipher_get_we_cipher (cipher));

	key = ieee_802_11_cipher_hash (cipher, ssid, input);
	result = nmu_security_serialize_wpa_psk (&iter, key, wpa_version, key_mgt);
	g_free (key);

	return result;
}


/*
 * nmu_create_dbus_error_message
 *
 * Make a pretty DBus error message
 *
 */
DBusMessage *
nmu_create_dbus_error_message (DBusMessage *message,
                               const char *exception,
                               const char *format,
                               ...)
{
	DBusMessage *	reply;
	va_list		args;
	char *		errmsg;

	errmsg = g_malloc0 (513);
	va_start (args, format);
	vsnprintf (errmsg, 512, format, args);
	va_end (args);

	reply = dbus_message_new_error (message, exception, errmsg);
	g_free (errmsg);

	return reply;
}


