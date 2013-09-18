/*
 * Evolution calendar - caldav backend
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the program; if not, see <http://www.gnu.org/licenses/>
 *
 *
 * Authors:
 *		Christian Kellner <gicmo@gnome.org>
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 */

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <glib/gstdio.h>
#include <glib/gi18n-lib.h>
#include <json-glib/json-glib.h>

#include "e-cal-backend-gtasks.h"
#include <gdata/gdata.h>
#include <goa/goa.h>

#define d(x)

#define E_CAL_BACKEND_GTASKS_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_CAL_BACKEND_GTASKS, ECalBackendGTasksPrivate))

#define CALDAV_CTAG_KEY "CALDAV_CTAG"
#define CALDAV_MAX_MULTIGET_AMOUNT 100 /* what's the maximum count of items to fetch within a multiget request */
#define LOCAL_PREFIX "file://"

/* in seconds */
#define DEFAULT_REFRESH_TIME 60

#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)
#define EDC_ERROR_EX(_code, _msg) e_data_cal_create_error (_code, _msg)

typedef enum {

	SLAVE_SHOULD_SLEEP,
	SLAVE_SHOULD_WORK,
	SLAVE_SHOULD_DIE

} SlaveCommand;

/* Private part of the ECalBackendHttp structure */
struct _ECalBackendGTasksPrivate {

	/* The local disk cache */
	ECalBackendStore *store;

	/* should we sync for offline mode? */
	gboolean do_offline;

	/* TRUE after gtasks_open */
	gboolean loaded;
	/* TRUE when server reachable */
	gboolean opened;

	/* lock to indicate a busy state */
	GMutex busy_lock;

	/* cond to synch threads */
	GCond cond;

	/* cond to know the slave gone */
	GCond slave_gone_cond;

	/* BG synch thread */
	const GThread *synch_slave; /* just for a reference, whether thread exists */
	SlaveCommand slave_cmd;
	gboolean slave_busy; /* whether is slave working */
	
	// FIXME removed all soup, proxy and uri
	// FIXME auth variables removed

	/* support for 'getctag' extension */
	gboolean ctag_supported;
	gchar *ctag_to_store;

	/* set to true if thread for ESource::changed is invoked */
	gboolean updating_source;

	guint refresh_id;
	
	/* GOA/libgdata stuff */
	GoaClient *client;
	GoaObject *object;
	GDataGoaAuthorizer *authorizer;
	GDataTasksService *service;
	
};

/* Forward Declarations */
static void	caldav_source_authenticator_init
				(ESourceAuthenticatorInterface *interface);

G_DEFINE_TYPE (ECalBackendGTasks, e_cal_backend_gtasks, E_TYPE_CAL_BACKEND_SYNC)

/* TODO Do not replicate this in every backend */
static icaltimezone *
resolve_tzid (const gchar *tzid,
              gpointer user_data)
{
	ETimezoneCache *timezone_cache;

	timezone_cache = E_TIMEZONE_CACHE (user_data);

	return e_timezone_cache_get_timezone (timezone_cache, tzid);
}

static ECalBackendSyncClass *parent_class = NULL;

static void gtasks_source_changed_cb (ESource *source, ECalBackendGTasks *cbgtasks);

/* ************************************************************************* */
/* Misc. utility functions */

static void
update_slave_cmd (ECalBackendCalDAVPrivate *priv,
                  SlaveCommand slave_cmd)
{
	g_return_if_fail (priv != NULL);

	if (priv->slave_cmd == SLAVE_SHOULD_DIE)
		return;

	priv->slave_cmd = slave_cmd;
}

/* passing NULL as 'href' removes the property */
static void
ecalcomp_set_href (ECalComponent *comp,
                   const gchar *href)
{
	icalcomponent *icomp;

	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_if_fail (icomp != NULL);

	icomp_x_prop_set (icomp, X_E_CALDAV "HREF", href);
}

static gchar *
ecalcomp_get_href (ECalComponent *comp)
{
	icalcomponent *icomp;
	gchar          *str;

	str = NULL;
	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, NULL);

	str =  icomp_x_prop_get (icomp, X_E_CALDAV "HREF");

	return str;
}

/* passing NULL as 'etag' removes the property */
static void
ecalcomp_set_etag (ECalComponent *comp,
                   const gchar *etag)
{
	icalcomponent *icomp;

	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_if_fail (icomp != NULL);

	icomp_x_prop_set (icomp, X_E_CALDAV "ETAG", etag);
}

static gchar *
ecalcomp_get_etag (ECalComponent *comp)
{
	icalcomponent *icomp;
	gchar          *str;

	str = NULL;
	icomp = e_cal_component_get_icalcomponent (comp);
	g_return_val_if_fail (icomp != NULL, NULL);

	str =  icomp_x_prop_get (icomp, X_E_CALDAV "ETAG");

	/* libical 0.48 escapes quotes, thus unescape them */
	if (str && strchr (str, '\\')) {
		gint ii, jj;

		for (ii = 0, jj = 0; str[ii]; ii++) {
			if (str[ii] == '\\') {
				ii++;
				if (!str[ii])
					break;
			}

			str[jj] = str[ii];
			jj++;
		}

		str[jj] = 0;
	}

	return str;
}

/* ************************************************************************* */


/* !TS, call with lock held */
static gboolean
check_state (ECalBackendGTasks *gtasks,
             gboolean *online,
             GError **perror)
{
	*online = FALSE;

	if (!gtasks->priv->loaded) {
		g_propagate_error (perror, EDC_ERROR_EX (OtherError, _("CalDAV backend is not loaded yet")));
		return FALSE;
	}

	if (!e_backend_get_online (E_BACKEND (gtasks))) {

		if (!cbdav->priv->do_offline) {
			g_propagate_error (perror, EDC_ERROR (RepositoryOffline));
			return FALSE;
		}

	} else {
		*online = TRUE;
	}

	return TRUE;
}

typedef struct _GTasksObject GTasksObject;

struct _GTasksObject {

	gchar *href;
	gchar *etag;

	guint status;

	gchar *cdata;
};

static void
gtasks_object_free (GTasksObject *object,
                    gboolean free_object_itself)
{
	g_free (object->href);
	g_free (object->etag);
	g_free (object->cdata);

	if (free_object_itself) {
		g_free (object);
	}
}

static gboolean
gtasks_query_for_uid (ECalBackendGTasks *cbgtasks,
                             const gchar *uid,
                             GCancellable *cancellable,
                             GError **error)
{
	// FIXME do a query write this function in 
	gdata_tasks_service_query_task_by_id (cbgtasks->priv->service, cbgtasks);
	// FIXME create component
	ECalComponent *comp = NULL;
	
	//e_cal_component_set_new_vtype ()
	//e_cal_component_set_summary ();
	//e_cal_component_set_completed ();
	//e_cal_component_set_due ();
	//e_cal_component_set_status ()
		
	// FIXME add it to storage
	e_cal_backend_store_put_component (cbgtasks->priv->store, comp);
	e_cal_backend_notify_component_created (cbgtasks, new_comp);
	//e_cal_backend_notify_component_modified (cal_backend, old_comp, new_comp);
	return result;
}

/* ************************************************************************* */
/* Synchronization foo */

static gboolean extract_timezones (ECalBackendCalDAV *cbdav, icalcomponent *icomp);

struct cache_comp_list
{
	GSList *slist;
};

static gboolean
remove_complist_from_cache_and_notify_cb (gpointer key,
                                          gpointer value,
                                          gpointer data)
{
	GSList *l;
	struct cache_comp_list *ccl = value;
	ECalBackendCalDAV *cbdav = data;

	for (l = ccl->slist; l; l = l->next) {
		ECalComponent *old_comp = l->data;
		ECalComponentId *id;

		id = e_cal_component_get_id (old_comp);
		if (!id) {
			continue;
		}

		if (e_cal_backend_store_remove_component (cbdav->priv->store, id->uid, id->rid)) {
			e_cal_backend_notify_component_removed ((ECalBackend *) cbdav, id, old_comp, NULL);
		}

		e_cal_component_free_id (id);
	}
	remove_cached_attachment (cbdav, (const gchar *) key);

	return FALSE;
}

static void
free_comp_list (gpointer cclist)
{
	struct cache_comp_list *ccl = cclist;

	g_return_if_fail (ccl != NULL);

	g_slist_foreach (ccl->slist, (GFunc) g_object_unref, NULL);
	g_slist_free (ccl->slist);
	g_free (ccl);
}

#define etags_match(_tag1, _tag2) ((_tag1 == _tag2) ? TRUE :                 \
				   g_str_equal (_tag1 != NULL ? _tag1 : "",  \
						_tag2 != NULL ? _tag2 : ""))

/* start_time/end_time is an interval for checking changes. If both greater than zero,
 * only the interval is checked and the removed items are not notified, as they can
 * be still there.
*/
static void
synchronize_cache (ECalBackendGTasks *cbgtasks,
                   time_t start_time,
                   time_t end_time)
{
	CalDAVObject *sobjs, *object;
	GSList *c_objs, *c_iter; /* list of all items known from our cache */
	GTree *c_uid2complist;  /* cache components list (with detached instances) sorted by (master's) uid */
	GHashTable *c_href2uid; /* connection between href and a (master's) uid */
	GSList *hrefs_to_update, *htu; /* list of href-s to update */
	gint i, len;

	if (!check_calendar_changed_on_server (cbdav)) {
		/* no changes on the server, no update required */
		return;
	}

	len    = 0;
	sobjs  = NULL;

	/* get list of server objects */
	if (!caldav_server_list_objects (cbdav, &sobjs, &len, NULL, start_time, end_time))
		return;

	c_objs = e_cal_backend_store_get_components (cbdav->priv->store);

	if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
		printf ("CalDAV - found %d objects on the server, locally stored %d objects\n", len, g_slist_length (c_objs)); fflush (stdout);
	}

	/* do not store changes in cache immediately - makes things significantly quicker */
	e_cal_backend_store_freeze_changes (cbdav->priv->store);

	c_uid2complist = g_tree_new_full ((GCompareDataFunc) g_strcmp0, NULL, g_free, free_comp_list);
	c_href2uid = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);

	/* fill indexed hash and tree with cached components */
	for (c_iter = c_objs; c_iter; c_iter = g_slist_next (c_iter)) {
		ECalComponent *ccomp = E_CAL_COMPONENT (c_iter->data);
		const gchar *uid = NULL;
		struct cache_comp_list *ccl;
		gchar *href;

		e_cal_component_get_uid (ccomp, &uid);
		if (!uid) {
			g_warning ("broken component with NULL Id");
			continue;
		}

		href = ecalcomp_get_href (ccomp);

		if (href == NULL) {
			g_warning ("href of object NULL :(");
			continue;
		}

		ccl = g_tree_lookup (c_uid2complist, uid);
		if (ccl) {
			ccl->slist = g_slist_prepend (ccl->slist, g_object_ref (ccomp));
		} else {
			ccl = g_new0 (struct cache_comp_list, 1);
			ccl->slist = g_slist_append (NULL, g_object_ref (ccomp));

			/* make a copy, which will be used in the c_href2uid too */
			uid = g_strdup (uid);

			g_tree_insert (c_uid2complist, (gpointer) uid, ccl);
		}

		if (g_hash_table_lookup (c_href2uid, href) == NULL) {
			/* uid is from a component or c_uid2complist key, thus will not be
			 * freed before a removal from c_uid2complist, thus do not duplicate it,
			 * rather save memory */
			g_hash_table_insert (c_href2uid, href, (gpointer) uid);
		} else {
			g_free (href);
		}
	}

	/* clear it now, we do not need it later */
	g_slist_foreach (c_objs, (GFunc) g_object_unref, NULL);
	g_slist_free (c_objs);
	c_objs = NULL;

	hrefs_to_update = NULL;

	/* see if we have to update or add some objects */
	for (i = 0, object = sobjs; i < len && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK; i++, object++) {
		ECalComponent *ccomp = NULL;
		gchar *etag = NULL;
		const gchar *uid;
		struct cache_comp_list *ccl;

		if (object->status != 200) {
			/* just continue here, so that the object
			 * doesnt get removed from the cobjs list
			 * - therefore it will be removed */
			continue;
		}

		uid = g_hash_table_lookup (c_href2uid, object->href);
		if (uid) {
			ccl = g_tree_lookup (c_uid2complist, uid);
			if (ccl) {
				GSList *sl;
				for (sl = ccl->slist; sl && !etag; sl = sl->next) {
					ccomp = sl->data;
					if (ccomp)
						etag = ecalcomp_get_etag (ccomp);
				}

				if (!etag)
					ccomp = NULL;
			}
		}

		if (!etag || !etags_match (etag, object->etag)) {
			hrefs_to_update = g_slist_prepend (hrefs_to_update, object->href);
		} else if (uid && ccl) {
			/* all components cover by this uid are up-to-date */
			GSList *p;

			for (p = ccl->slist; p; p = p->next) {
				g_object_unref (p->data);
			}

			g_slist_free (ccl->slist);
			ccl->slist = NULL;
		}

		g_free (etag);
	}

	/* free hash table, as it is not used anymore */
	g_hash_table_destroy (c_href2uid);
	c_href2uid = NULL;

	if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
		printf ("CalDAV - recognized %d items to update\n", g_slist_length (hrefs_to_update)); fflush (stdout);
	}

	htu = hrefs_to_update;
	while (htu && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK) {
		gint count = 0;
		GSList *to_fetch = NULL;

		while (count < CALDAV_MAX_MULTIGET_AMOUNT && htu) {
			to_fetch = g_slist_prepend (to_fetch, htu->data);
			htu = htu->next;
			count++;
		}

		if (to_fetch && cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK) {
			CalDAVObject *up_sobjs = NULL;

			if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
				printf ("CalDAV - going to fetch %d items\n", g_slist_length (to_fetch)); fflush (stdout);
			}

			count = 0;
			if (!caldav_server_list_objects (cbdav, &up_sobjs, &count, to_fetch, 0, 0)) {
				fprintf (stderr, "CalDAV - failed to retrieve bunch of items\n"); fflush (stderr);
				break;
			}

			if (caldav_debug_show (DEBUG_SERVER_ITEMS)) {
				printf ("CalDAV - fetched bunch of %d items\n", count); fflush (stdout);
			}

			/* we are going to update cache */
			/* they are downloaded, so process them */
			for (i = 0, object = up_sobjs; i < count /*&& cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK */; i++, object++) {
				if (object->status == 200 && object->href && object->etag && object->cdata && *object->cdata) {
					icalcomponent *icomp = icalparser_parse_string (object->cdata);

					if (icomp) {
						put_server_comp_to_cache (cbdav, icomp, object->href, object->etag, c_uid2complist);
						icalcomponent_free (icomp);
					}
				}

				/* these free immediately */
				caldav_object_free (object, FALSE);
			}

			/* cache update done for fetched items */
			g_free (up_sobjs);
		}

		/* do not free 'data' itself, it's part of 'sobjs' */
		g_slist_free (to_fetch);
	}

	/* if not interrupted and not using the time range... */
	if (cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK && (!start_time || !end_time)) {
		/* ...remove old (not on server anymore) items from our cache and notify of a removal */
		g_tree_foreach (c_uid2complist, remove_complist_from_cache_and_notify_cb, cbdav);
	}

	if (cbdav->priv->ctag_to_store) {
		/* store only when wasn't interrupted */
		if (cbdav->priv->slave_cmd == SLAVE_SHOULD_WORK && start_time == 0 && end_time == 0) {
			e_cal_backend_store_put_key_value (cbdav->priv->store, CALDAV_CTAG_KEY, cbdav->priv->ctag_to_store);
		}

		g_free (cbdav->priv->ctag_to_store);
		cbdav->priv->ctag_to_store = NULL;
	}

	/* save cache changes to disk finally */
	e_cal_backend_store_thaw_changes (cbdav->priv->store);

	for (i = 0, object = sobjs; i < len; i++, object++) {
		caldav_object_free (object, FALSE);
	}

	g_tree_destroy (c_uid2complist);
	g_slist_free (hrefs_to_update);
	g_free (sobjs);
}

static void
time_to_refresh_gtasks_cb (ESource *source,
                                    gpointer user_data)
{
	ECalBackendCalGTasks *gtasks = user_data;

	g_return_if_fail (E_IS_CAL_BACKEND_GTASKS (cbdav));

	g_cond_signal (&cbdav->priv->cond);
}


/* ************************************************************************* */
/* ********** ECalBackendSync virtual function implementation *************  */

static gchar *
gtasks_get_backend_property (ECalBackend *backend,
                             const gchar *prop_name)
{
	/* FIXME what properties I need to return */
	g_return_val_if_fail (prop_name != NULL, FALSE);

	if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		ESourceGTasks *extension;
		ESource *source;
		GString *caps;
		gchar *usermail;
		const gchar *extension_name;

		caps = g_string_new (
			CAL_STATIC_CAPABILITY_NO_THISANDFUTURE ","
			CAL_STATIC_CAPABILITY_NO_THISANDPRIOR ","
			CAL_STATIC_CAPABILITY_REFRESH_SUPPORTED);

		usermail = get_usermail (E_CAL_BACKEND (backend));
		if (!usermail || !*usermail)
			g_string_append (caps, "," CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS);
		g_free (usermail);

		return g_string_free (caps, FALSE);

	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   g_str_equal (prop_name, CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		return get_usermail (E_CAL_BACKEND (backend));

	} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		ECalComponent *comp;
		gchar *prop_value;

		comp = e_cal_component_new ();

		switch (e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
		case ICAL_VEVENT_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
			break;
		case ICAL_VTODO_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_TODO);
			break;
		case ICAL_VJOURNAL_COMPONENT:
			e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_JOURNAL);
			break;
		default:
			g_object_unref (comp);
			return NULL;
		}

		prop_value = e_cal_component_get_as_string (comp);

		g_object_unref (comp);

		return prop_value;
	}

	/* Chain up to parent's get_backend_property() method. */
	return E_CAL_BACKEND_CLASS (e_cal_backend_gtasks_parent_class)->
		get_backend_property (backend, prop_name);
}

static void
gtasks_shutdown (ECalBackend *backend)
{
	ECalBackendGTasksPrivate *priv;
	ESource *source;

	priv = E_CAL_BACKEND_GTASKS_GET_PRIVATE (backend);

	/* Chain up to parent's shutdown() method. */
	E_CAL_BACKEND_CLASS (e_cal_backend_gtasks_parent_class)->shutdown (backend);

	/* tell the slave to stop before acquiring a lock,
	 * as it can work at the moment, and lock can be locked */
	update_slave_cmd (priv, SLAVE_SHOULD_DIE);

	g_mutex_lock (&priv->busy_lock);

	/* XXX Not sure if this really needs to be part of
	 *     shutdown or if we can just do it in dispose(). */
	source = e_backend_get_source (E_BACKEND (backend));
	if (source) {
		g_signal_handlers_disconnect_by_func (G_OBJECT (source), gtasks_source_changed_cb, backend);

		if (priv->refresh_id) {
			e_source_refresh_remove_timeout (source, priv->refresh_id);
			priv->refresh_id = 0;
		}
	}

	/* stop the slave  */
	while (priv->synch_slave) {
		g_cond_signal (&priv->cond);

		/* wait until the slave died */
		g_cond_wait (&priv->slave_gone_cond, &priv->busy_lock);
	}

	g_mutex_unlock (&priv->busy_lock);
}

static gboolean
initialize_backend (ECalBackendGTasks *cbgtasks,
                    GError **perror)
{
	ESourceAuthentication    *auth_extension;
	ESourceOffline           *offline_extension;
	ESourceGTasks            *gtasks_extension;
	ECalBackend              *backend;
	SoupURI                  *soup_uri;
	ESource                  *source;
	gsize                     len;
	const gchar              *cache_dir;
	const gchar              *extension_name;

	backend = E_CAL_BACKEND (cbgtasks);
	cache_dir = e_cal_backend_get_cache_dir (backend);
	source = e_backend_get_source (E_BACKEND (backend));

	extension_name = E_SOURCE_EXTENSION_AUTHENTICATION;
	auth_extension = e_source_get_extension (source, extension_name);

	extension_name = E_SOURCE_EXTENSION_OFFLINE;
	offline_extension = e_source_get_extension (source, extension_name);

	extension_name = E_SOURCE_EXTENSION_GTASKS_BACKEND;
	webdav_extension = e_source_get_extension (source, extension_name);

	if (!g_signal_handler_find (G_OBJECT (source), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, gtasks_source_changed_cb, cbdav))
		g_signal_connect (G_OBJECT (source), "changed", G_CALLBACK (gtasks_source_changed_cb), gtasks);

	cbgtasks->priv->do_offline = e_source_offline_get_stay_synchronized (offline_extension);

	cbgtasks->priv->auth_required = e_source_authentication_required (auth_extension);

	if (cbdav->priv->store == NULL) {
		/* remove the old cache while migrating to ECalBackendStore */
		e_cal_backend_cache_remove (cache_dir, "cache.xml");
		cbdav->priv->store = e_cal_backend_store_new (
			cache_dir, E_TIMEZONE_CACHE (cbdav));
		e_cal_backend_store_load (cbdav->priv->store);
	}

	/* Set the local attachment store */
	if (g_mkdir_with_parents (cache_dir, 0700) < 0) {
		g_propagate_error (perror, e_data_cal_create_error_fmt (OtherError, _("Cannot create local cache folder '%s'"), cache_dir));
		return FALSE;
	}

	if (!cbdav->priv->synch_slave) {
		GThread *slave;

		update_slave_cmd (cbdav->priv, SLAVE_SHOULD_SLEEP);
		slave = g_thread_new (NULL, caldav_synch_slave_loop, cbdav);

		cbdav->priv->synch_slave = slave;
		g_thread_unref (slave);
	}

	/* FIXME write my own refresh */
	if (cbdav->priv->refresh_id == 0) {
		cbdav->priv->refresh_id = e_source_refresh_add_timeout (
			source, NULL, time_to_refresh_gtasks_cb, cbdav, NULL);
	}

	return TRUE;
}

static gboolean
open_tasks (ECalBackendGTasks *cbgtasks,
               GCancellable *cancellable,
               GError **error)
{
	gboolean server_unreachable = FALSE;
	gboolean success;
	GError *local_error = NULL;

	g_return_val_if_fail (cbdav != NULL, FALSE);

	/* set forward proxy */
	//proxy_settings_changed (cbdav->priv->proxy, cbdav->priv);

	//success = caldav_server_open_calendar (
	//	cbdav, &server_unreachable, cancellable, &local_error);

	/* FIXME all those objects in ->priv */
	cbgtasks->priv->client = goa_client_new_sync (NULL, &local_error);
	/* FIXME first - shouldn't be this abstracted and second - how to get same effect as lookup_by_id */
	cbgtasks->priv->object = goa_client_lookup_by_id (client, "account_1377173897" /* Get this from seahorse */);
	cbgtasks->priv->authorizer = gdata_goa_authorizer_new (object);
	cbgtasks->priv->service = GDATA_SERVICE (gdata_tasks_service_new (GDATA_AUTHORIZER (authorizer)));
	
	/* FIXME how to detect problems with goa/libgdata */
	success = TRUE:
	server_unreachable = FALSE;
	
	if (success) {
		update_slave_cmd (cbgtasks->priv, SLAVE_SHOULD_WORK);
		g_cond_signal (&cbgtasks->priv->cond);

		// FIXME remove is_google thingy - we ain't webdav
		//cbdav->priv->is_google = is_google_uri (cbdav->priv->uri);
	} else if (server_unreachable) {
		cbdav->priv->opened = FALSE;
		e_cal_backend_set_writable (E_CAL_BACKEND (cbdav), FALSE);
		if (local_error) {
			gchar *msg = g_strdup_printf (_("Server is unreachable, calendar is opened in read-only mode.\nError message: %s"), local_error->message);
			e_cal_backend_notify_error (E_CAL_BACKEND (cbdav), msg);
			g_free (msg);
			g_clear_error (&local_error);
			success = TRUE;
		}
	}

	if (local_error != NULL)
		g_propagate_error (error, local_error);

	return success;
}

static void
gtasks_do_open (ECalBackendSync *backend,
                EDataCal *cal,
                GCancellable *cancellable,
                gboolean only_if_exists,
                GError **perror)
{
	ECalBackendCalGTasks *cbgtasks;
	gboolean online;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	g_mutex_lock (&cbgtasks->priv->busy_lock);

	/* let it decide the 'getctag' extension availability again */
	cbgtasks->priv->ctag_supported = TRUE;

	if (!cbgtasks->priv->loaded && !initialize_backend (cbgtasks, perror)) {
		g_mutex_unlock (&cbgtasks->priv->busy_lock);
		return;
	}

	online = e_backend_get_online (E_BACKEND (backend));

	if (!cbgtasks->priv->do_offline && !online) {
		g_mutex_unlock (&cbgtasks->priv->busy_lock);
		g_propagate_error (perror, EDC_ERROR (RepositoryOffline));
		return;
	}

	cbgtasks->priv->loaded = TRUE;
	cbgtasks->priv->opened = TRUE;
	//cbgtasks->priv->is_google = FALSE;

	if (online) {
		GError *local_error = NULL;

		open_tasks (cbdav, cancellable, &local_error);
		/* FIXME create authorizer and cache how it's done in open calendar*/
		// FIXME how we get which account we need? How do we get account id?
		if (g_error_matches (local_error, E_DATA_CAL_ERROR, AuthenticationRequired) || g_error_matches (local_error, E_DATA_CAL_ERROR, AuthenticationFailed)) {
			g_clear_error (&local_error);
			/* FIXME what is caldav_authenticate */
			caldav_authenticate (
				cbdav, FALSE, cancellable, perror);
		}

		if (local_error != NULL)
			g_propagate_error (perror, local_error);

	} else {
		e_cal_backend_set_writable (E_CAL_BACKEND (cbgtasks), FALSE);
	}

	g_mutex_unlock (&cbgtasks->priv->busy_lock);
}

static void
gtasks_refresh (ECalBackendSync *backend,
                EDataCal *cal,
                GCancellable *cancellable,
                GError **perror)
{
	ECalBackendGTasks *cbdav;
	gboolean online;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	g_mutex_lock (&cbgtasks->priv->busy_lock);

	if (!cbgtasks->priv->loaded
	    || cbgtasks->priv->slave_cmd == SLAVE_SHOULD_DIE
	    || !check_state (cbgtasks, &online, NULL)
	    || !online) {
		g_mutex_unlock (&cbdav->priv->busy_lock);
		return;
	}

	update_slave_cmd (cbgtasks->priv, SLAVE_SHOULD_WORK);

	/* wake it up */
	g_cond_signal (&cbgtasks->priv->cond);
	g_mutex_unlock (&cbgtasks->priv->busy_lock);
}

static void
remove_comp_from_cache_cb (gpointer value,
                           gpointer user_data)
{
	ECalComponent *comp = value;
	ECalBackendStore *store = user_data;
	ECalComponentId *id;

	g_return_if_fail (comp != NULL);
	g_return_if_fail (store != NULL);

	id = e_cal_component_get_id (comp);
	g_return_if_fail (id != NULL);

	e_cal_backend_store_remove_component (store, id->uid, id->rid);
	e_cal_component_free_id (id);
}

static gboolean
remove_comp_from_cache (ECalBackendCalDAV *cbdav,
                        const gchar *uid,
                        const gchar *rid)
{
	gboolean res = FALSE;

	if (!rid || !*rid) {
		/* get with detached instances */
		GSList *objects = e_cal_backend_store_get_components_by_uid (cbdav->priv->store, uid);

		if (objects) {
			g_slist_foreach (objects, (GFunc) remove_comp_from_cache_cb, cbdav->priv->store);
			g_slist_foreach (objects, (GFunc) g_object_unref, NULL);
			g_slist_free (objects);

			res = TRUE;
		}
	} else {
		res = e_cal_backend_store_remove_component (cbdav->priv->store, uid, rid);
	}

	return res;
}

static gint
sort_master_first (gconstpointer a,
                   gconstpointer b)
{
	icalcomponent *ca, *cb;

	ca = e_cal_component_get_icalcomponent ((ECalComponent *) a);
	cb = e_cal_component_get_icalcomponent ((ECalComponent *) b);

	if (!ca) {
		if (!cb)
			return 0;
		else
			return -1;
	} else if (!cb) {
		return 1;
	}

	return icaltime_compare (icalcomponent_get_recurrenceid (ca), icalcomponent_get_recurrenceid (cb));
}

/* Returns new icalcomponent, with all detached instances stored in a cache.
 * The cache lock should be locked when called this function.
*/
static icalcomponent *
get_comp_from_cache (ECalBackendGTasks *cbgtasks,
                     const gchar *uid,
                     const gchar *rid,
                     gchar **href,
                     gchar **etag)
{
	icalcomponent *icalcomp = NULL;

	if (rid == NULL || !*rid) {
		/* get with detached instances */
		GSList *objects = e_cal_backend_store_get_components_by_uid (cbgtasks->priv->store, uid);

		if (!objects) {
			return NULL;
		}

		if (g_slist_length (objects) == 1) {
			ECalComponent *comp = objects->data;

			/* will be unreffed a bit later */
			if (comp)
				icalcomp = icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp));
		} else {
			/* if we have detached recurrences, return a VCALENDAR */
			icalcomp = e_cal_util_new_top_level ();

			objects = g_slist_sort (objects, sort_master_first);

			/* add all detached recurrences and the master object */
			g_slist_foreach (objects, add_detached_recur_to_vcalendar_cb, icalcomp);
		}

		/* every component has set same href and etag, thus it doesn't matter where it will be read */
		if (href)
			*href = ecalcomp_get_href (objects->data);
		if (etag)
			*etag = ecalcomp_get_etag (objects->data);

		g_slist_foreach (objects, (GFunc) g_object_unref, NULL);
		g_slist_free (objects);
	} else {
		/* get the exact object */
		ECalComponent *comp = e_cal_backend_store_get_component (cbgtasks->priv->store, uid, rid);

		if (comp) {
			icalcomp = icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp));
			if (href)
				*href = ecalcomp_get_href (comp);
			if (etag)
				*etag = ecalcomp_get_etag (comp);
			g_object_unref (comp);
		}
	}

	return icalcomp;
}

static void
remove_property (gpointer prop,
                 gpointer icomp)
{
	icalcomponent_remove_property (icomp, prop);
	icalproperty_free (prop);
}

static void
strip_unneeded_x_props (icalcomponent *icomp)
{
	icalproperty *prop;
	GSList *to_remove = NULL;

	g_return_if_fail (icomp != NULL);
	g_return_if_fail (icalcomponent_isa (icomp) != ICAL_VCALENDAR_COMPONENT);

	for (prop = icalcomponent_get_first_property (icomp, ICAL_X_PROPERTY);
	     prop;
	     prop = icalcomponent_get_next_property (icomp, ICAL_X_PROPERTY)) {
		if (g_str_has_prefix (icalproperty_get_x_name (prop), X_E_CALDAV)) {
			to_remove = g_slist_prepend (to_remove, prop);
		}
	}

	for (prop = icalcomponent_get_first_property (icomp, ICAL_XLICERROR_PROPERTY);
	     prop;
	     prop = icalcomponent_get_next_property (icomp, ICAL_XLICERROR_PROPERTY)) {
		to_remove = g_slist_prepend (to_remove, prop);
	}

	g_slist_foreach (to_remove, remove_property, icomp);
	g_slist_free (to_remove);
}

static gboolean
is_stored_on_server (ECalBackendCalDAV *cbdav,
                     const gchar *uri)
{
	SoupURI *my_uri, *test_uri;
	gboolean res;

	g_return_val_if_fail (E_IS_CAL_BACKEND_CALDAV (cbdav), FALSE);
	g_return_val_if_fail (cbdav->priv->uri != NULL, FALSE);
	g_return_val_if_fail (uri != NULL, FALSE);

	my_uri = soup_uri_new (cbdav->priv->uri);
	g_return_val_if_fail (my_uri != NULL, FALSE);

	test_uri = soup_uri_new (uri);
	if (!test_uri) {
		soup_uri_free (my_uri);
		return FALSE;
	}

	res = my_uri->host && test_uri->host && g_ascii_strcasecmp (my_uri->host, test_uri->host) == 0;

	soup_uri_free (my_uri);
	soup_uri_free (test_uri);

	return res;
}

static void
remove_files (const gchar *dir,
              const gchar *fileprefix)
{
	GDir *d;

	g_return_if_fail (dir != NULL);
	g_return_if_fail (fileprefix != NULL);
	g_return_if_fail (*fileprefix != '\0');

	d = g_dir_open (dir, 0, NULL);
	if (d) {
		const gchar *entry;
		gint len = strlen (fileprefix);

		while ((entry = g_dir_read_name (d)) != NULL) {
			if (entry && strncmp (entry, fileprefix, len) == 0) {
				gchar *path;

				path = g_build_filename (dir, entry, NULL);
				if (!g_file_test (path, G_FILE_TEST_IS_DIR))
					g_unlink (path);
				g_free (path);
			}
		}
		g_dir_close (d);
	}
}

/* callback for icalcomponent_foreach_tzid */
typedef struct {
	ECalBackendStore *store;
	icalcomponent *vcal_comp;
	icalcomponent *icalcomp;
} ForeachTzidData;

static void
add_timezone_cb (icalparameter *param,
                 gpointer data)
{
	icaltimezone *tz;
	const gchar *tzid;
	icalcomponent *vtz_comp;
	ForeachTzidData *f_data = (ForeachTzidData *) data;
	ETimezoneCache *cache;

	tzid = icalparameter_get_tzid (param);
	if (!tzid)
		return;

	tz = icalcomponent_get_timezone (f_data->vcal_comp, tzid);
	if (tz)
		return;

	cache = e_cal_backend_store_ref_timezone_cache (f_data->store);

	tz = icalcomponent_get_timezone (f_data->icalcomp, tzid);
	if (tz == NULL)
		tz = icaltimezone_get_builtin_timezone_from_tzid (tzid);
	if (tz == NULL)
		tz = e_timezone_cache_get_timezone (cache, tzid);

	vtz_comp = icaltimezone_get_component (tz);

	if (tz != NULL && vtz_comp != NULL)
		icalcomponent_add_component (
			f_data->vcal_comp,
			icalcomponent_new_clone (vtz_comp));

	g_object_unref (cache);
}

static void
add_timezones_from_component (ECalBackendCalDAV *cbdav,
                              icalcomponent *vcal_comp,
                              icalcomponent *icalcomp)
{
	ForeachTzidData f_data;

	g_return_if_fail (cbdav != NULL);
	g_return_if_fail (vcal_comp != NULL);
	g_return_if_fail (icalcomp != NULL);

	f_data.store = cbdav->priv->store;
	f_data.vcal_comp = vcal_comp;
	f_data.icalcomp = icalcomp;

	icalcomponent_foreach_tzid (icalcomp, add_timezone_cb, &f_data);
}

/* also removes X-EVOLUTION-CALDAV from all the components */
static gchar *
pack_cobj (ECalBackendCalDAV *cbdav,
           icalcomponent *icomp)
{
	icalcomponent *calcomp;
	gchar          *objstr;

	if (icalcomponent_isa (icomp) != ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *cclone;

		calcomp = e_cal_util_new_top_level ();

		cclone = icalcomponent_new_clone (icomp);
		strip_unneeded_x_props (cclone);
		convert_to_inline_attachment (cbdav, cclone);
		icalcomponent_add_component (calcomp, cclone);
		add_timezones_from_component (cbdav, calcomp, cclone);
	} else {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));

		calcomp = icalcomponent_new_clone (icomp);
		for (subcomp = icalcomponent_get_first_component (calcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (calcomp, my_kind)) {
			strip_unneeded_x_props (subcomp);
			convert_to_inline_attachment (cbdav, subcomp);
			add_timezones_from_component (cbdav, calcomp, subcomp);
		}
	}

	objstr = icalcomponent_as_ical_string_r (calcomp);
	icalcomponent_free (calcomp);

	g_assert (objstr);

	return objstr;

}

static void
sanitize_component (ECalBackend *cb,
                    ECalComponent *comp)
{
	ECalComponentDateTime dt;
	icaltimezone *zone;

	/* Check dtstart, dtend and due's timezone, and convert it to local
	 * default timezone if the timezone is not in our builtin timezone
	 * list */
	e_cal_component_get_dtstart (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = e_timezone_cache_get_timezone (
			E_TIMEZONE_CACHE (cb), dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_dtstart (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);

	e_cal_component_get_dtend (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = e_timezone_cache_get_timezone (
			E_TIMEZONE_CACHE (cb), dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_dtend (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);

	e_cal_component_get_due (comp, &dt);
	if (dt.value && dt.tzid) {
		zone = e_timezone_cache_get_timezone (
			E_TIMEZONE_CACHE (cb), dt.tzid);
		if (!zone) {
			g_free ((gchar *) dt.tzid);
			dt.tzid = g_strdup ("UTC");
			e_cal_component_set_due (comp, &dt);
		}
	}
	e_cal_component_free_datetime (&dt);
	e_cal_component_abort_sequence (comp);
}

static gboolean
cache_contains (ECalBackendCalDAV *cbdav,
                const gchar *uid,
                const gchar *rid)
{
	gboolean res;
	ECalComponent *comp;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (uid != NULL, FALSE);

	g_return_val_if_fail (cbdav->priv->store != NULL, FALSE);

	comp = e_cal_backend_store_get_component (cbdav->priv->store, uid, rid);
	res = comp != NULL;

	if (comp)
		g_object_unref (comp);

	return res;
}

/* Returns subcomponent of icalcomp, which is a master object, or icalcomp itself, if it's not a VCALENDAR;
 * Do not free returned pointer, it'll be freed together with the icalcomp.
*/
static icalcomponent *
get_master_comp (ECalBackendCalDAV *cbdav,
                 icalcomponent *icalcomp)
{
	icalcomponent *master = icalcomp;

	g_return_val_if_fail (cbdav != NULL, NULL);
	g_return_val_if_fail (icalcomp != NULL, NULL);

	if (icalcomponent_isa (icalcomp) == ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));

		master = NULL;

		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = icalcomponent_get_next_component (icalcomp, my_kind)) {
			struct icaltimetype sub_rid = icalcomponent_get_recurrenceid (subcomp);

			if (icaltime_is_null_time (sub_rid)) {
				master = subcomp;
				break;
			}
		}
	}

	return master;
}

static gboolean
remove_instance (ECalBackendCalDAV *cbdav,
                 icalcomponent *icalcomp,
                 struct icaltimetype rid,
                 ECalObjModType mod,
                 gboolean also_exdate)
{
	icalcomponent *master = icalcomp;
	gboolean res = FALSE;

	g_return_val_if_fail (icalcomp != NULL, res);
	g_return_val_if_fail (!icaltime_is_null_time (rid), res);

	/* remove an instance only */
	if (icalcomponent_isa (icalcomp) == ICAL_VCALENDAR_COMPONENT) {
		icalcomponent *subcomp;
		icalcomponent_kind my_kind = e_cal_backend_get_kind (E_CAL_BACKEND (cbdav));
		gint left = 0;
		gboolean start_first = FALSE;

		master = NULL;

		/* remove old instance first */
		for (subcomp = icalcomponent_get_first_component (icalcomp, my_kind);
		     subcomp;
		     subcomp = start_first ? icalcomponent_get_first_component (icalcomp, my_kind) : icalcomponent_get_next_component (icalcomp, my_kind)) {
			struct icaltimetype sub_rid = icalcomponent_get_recurrenceid (subcomp);

			start_first = FALSE;

			if (icaltime_is_null_time (sub_rid)) {
				master = subcomp;
				left++;
			} else if (icaltime_compare (sub_rid, rid) == 0) {
				icalcomponent_remove_component (icalcomp, subcomp);
				icalcomponent_free (subcomp);
				if (master) {
					break;
				} else {
					/* either no master or master not as the first component, thus rescan */
					left = 0;
					start_first = TRUE;
				}
			} else {
				left++;
			}
		}

		/* whether left at least one instance or a master object */
		res = left > 0;
	} else {
		res = TRUE;
	}

	if (master && also_exdate) {
		e_cal_util_remove_instances (master, rid, mod);
	}

	return res;
}

static icalcomponent *
replace_master (ECalBackendCalDAV *cbdav,
                icalcomponent *old_comp,
                icalcomponent *new_master)
{
	icalcomponent *old_master;
	if (icalcomponent_isa (old_comp) != ICAL_VCALENDAR_COMPONENT) {
		icalcomponent_free (old_comp);
		return new_master;
	}

	old_master = get_master_comp (cbdav, old_comp);
	if (!old_master) {
		/* no master, strange */
		icalcomponent_free (new_master);
	} else {
		icalcomponent_remove_component (old_comp, old_master);
		icalcomponent_free (old_master);

		icalcomponent_add_component (old_comp, new_master);
	}

	return old_comp;
}

/* the resulting component should be unreffed when done with it;
 * the fallback_comp is cloned, if used */
static ECalComponent *
get_ecalcomp_master_from_cache_or_fallback (ECalBackendCalDAV *cbdav,
                                            const gchar *uid,
                                            const gchar *rid,
                                            ECalComponent *fallback_comp)
{
	ECalComponent *comp = NULL;
	icalcomponent *icalcomp;

	g_return_val_if_fail (cbdav != NULL, NULL);
	g_return_val_if_fail (uid != NULL, NULL);

	icalcomp = get_comp_from_cache (cbdav, uid, rid, NULL, NULL);
	if (icalcomp) {
		icalcomponent *master = get_master_comp (cbdav, icalcomp);

		if (master) {
			comp = e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master));
		}

		icalcomponent_free (icalcomp);
	}

	if (!comp && fallback_comp)
		comp = e_cal_component_clone (fallback_comp);

	return comp;
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_create_objects (ECalBackendCalDAV *cbdav,
                   const GSList *in_calobjs,
                   GSList **uids,
                   GSList **new_components,
                   GCancellable *cancellable,
                   GError **perror)
{
	ECalComponent            *comp;
	gboolean                  online, did_put = FALSE;
	struct icaltimetype current;
	icalcomponent *icalcomp;
	const gchar *in_calobj = in_calobjs->data;
	const gchar *comp_uid;

	if (!check_state (cbdav, &online, perror))
		return;

	/* We make the assumption that the in_calobjs list we're passed is always exactly one element long, since we haven't specified "bulk-adds"
	 * in our static capability list. This simplifies a lot of the logic, especially around asynchronous results. */
	if (in_calobjs->next != NULL) {
		g_propagate_error (perror, e_data_cal_create_error (UnsupportedMethod, _("CalDAV does not support bulk additions")));
		return;
	}

	comp = e_cal_component_new_from_string (in_calobj);

	if (comp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	icalcomp = e_cal_component_get_icalcomponent (comp);
	if (icalcomp == NULL) {
		g_object_unref (comp);
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	comp_uid = icalcomponent_get_uid (icalcomp);
	if (!comp_uid) {
		gchar *new_uid;

		new_uid = e_cal_component_gen_uid ();
		if (!new_uid) {
			g_object_unref (comp);
			g_propagate_error (perror, EDC_ERROR (InvalidObject));
			return;
		}

		icalcomponent_set_uid (icalcomp, new_uid);
		comp_uid = icalcomponent_get_uid (icalcomp);

		g_free (new_uid);
	}

	/* check the object is not in our cache */
	if (cache_contains (cbdav, comp_uid, NULL)) {
		g_object_unref (comp);
		g_propagate_error (perror, EDC_ERROR (ObjectIdAlreadyExists));
		return;
	}

	/* Set the created and last modified times on the component */
	current = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_created (comp, &current);
	e_cal_component_set_last_modified (comp, &current);

	/* sanitize the component*/
	sanitize_component ((ECalBackend *) cbdav, comp);

	if (online) {
		CalDAVObject object;

		object.href  = ecalcomp_gen_href (comp);
		object.etag  = NULL;
		object.cdata = pack_cobj (cbdav, icalcomp);

		did_put = caldav_server_put_object (cbdav, &object, icalcomp, cancellable, perror);

		caldav_object_free (&object, FALSE);
	} else {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_CREATED); */
	}

	if (did_put) {
		if (uids)
			*uids = g_slist_prepend (*uids, g_strdup (comp_uid));

		if (new_components)
			*new_components = g_slist_prepend(*new_components, get_ecalcomp_master_from_cache_or_fallback (cbdav, comp_uid, NULL, comp));
	}

	g_object_unref (comp);
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_modify_objects (ECalBackendCalDAV *cbdav,
                   const GSList *calobjs,
                   ECalObjModType mod,
                   GSList **old_components,
                   GSList **new_components,
                   GCancellable *cancellable,
                   GError **error)
{
	ECalComponent            *comp;
	icalcomponent            *cache_comp;
	gboolean                  online, did_put = FALSE;
	ECalComponentId		 *id;
	struct icaltimetype current;
	gchar *href = NULL, *etag = NULL;
	const gchar *calobj = calobjs->data;

	if (new_components)
		*new_components = NULL;

	if (!check_state (cbdav, &online, error))
		return;

	/* We make the assumption that the calobjs list we're passed is always exactly one element long, since we haven't specified "bulk-modifies"
	 * in our static capability list. This simplifies a lot of the logic, especially around asynchronous results. */
	if (calobjs->next != NULL) {
		g_propagate_error (error, e_data_cal_create_error (UnsupportedMethod, _("CalDAV does not support bulk modifications")));
		return;
	}

	comp = e_cal_component_new_from_string (calobj);

	if (comp == NULL) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	if (!e_cal_component_get_icalcomponent (comp) ||
	    icalcomponent_isa (e_cal_component_get_icalcomponent (comp)) != e_cal_backend_get_kind (E_CAL_BACKEND (cbdav))) {
		g_object_unref (comp);
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	/* Set the last modified time on the component */
	current = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_last_modified (comp, &current);

	/* sanitize the component */
	sanitize_component ((ECalBackend *) cbdav, comp);

	id = e_cal_component_get_id (comp);
	if (id == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_INVALID_OBJECT,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_INVALID_OBJECT));
		return;
	}

	/* fetch full component from cache, it will be pushed to the server */
	cache_comp = get_comp_from_cache (cbdav, id->uid, NULL, &href, &etag);

	if (cache_comp == NULL) {
		e_cal_component_free_id (id);
		g_object_unref (comp);
		g_free (href);
		g_free (etag);
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	if (!online) {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_MODIFIED);*/
	}

	if (old_components) {
		*old_components = NULL;

		if (e_cal_component_is_instance (comp)) {
			/* set detached instance as the old object, if any */
			ECalComponent *old_instance = e_cal_backend_store_get_component (cbdav->priv->store, id->uid, id->rid);

			/* This will give a reference to 'old_component' */
			if (old_instance) {
				*old_components = g_slist_prepend (*old_components, e_cal_component_clone (old_instance));
				g_object_unref (old_instance);
			}
		}

		if (!*old_components) {
			icalcomponent *master = get_master_comp (cbdav, cache_comp);

			if (master) {
				/* set full component as the old object */
				*old_components = g_slist_prepend (*old_components, e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master)));
			}
		}
	}

	switch (mod) {
	case E_CAL_OBJ_MOD_ONLY_THIS:
	case E_CAL_OBJ_MOD_THIS:
		if (e_cal_component_is_instance (comp)) {
			icalcomponent *new_comp = e_cal_component_get_icalcomponent (comp);

			/* new object is only this instance */
			if (new_components)
				*new_components = g_slist_prepend (*new_components, e_cal_component_clone (comp));

			/* add the detached instance */
			if (icalcomponent_isa (cache_comp) == ICAL_VCALENDAR_COMPONENT) {
				/* do not modify the EXDATE, as the component will be put back */
				remove_instance (cbdav, cache_comp, icalcomponent_get_recurrenceid (new_comp), mod, FALSE);
			} else {
				/* this is only a master object, thus make is a VCALENDAR component */
				icalcomponent *icomp;

				icomp = e_cal_util_new_top_level ();
				icalcomponent_add_component (icomp, cache_comp);

				/* no need to free the cache_comp, as it is inside icomp */
				cache_comp = icomp;
			}

			if (cache_comp && cbdav->priv->is_google) {
				icalcomponent_set_sequence (cache_comp, icalcomponent_get_sequence (cache_comp) + 1);
				icalcomponent_set_sequence (new_comp, icalcomponent_get_sequence (new_comp) + 1);
			}

			/* add the detached instance finally */
			icalcomponent_add_component (cache_comp, icalcomponent_new_clone (new_comp));
		} else {
			cache_comp = replace_master (cbdav, cache_comp, icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp)));
		}
		break;
	case E_CAL_OBJ_MOD_ALL:
		cache_comp = replace_master (cbdav, cache_comp, icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp)));
		break;
	case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
	case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
		break;
	}

	if (online) {
		CalDAVObject object;

		object.href  = href;
		object.etag  = etag;
		object.cdata = pack_cobj (cbdav, cache_comp);

		did_put = caldav_server_put_object (cbdav, &object, cache_comp, cancellable, error);

		caldav_object_free (&object, FALSE);
		href = NULL;
		etag = NULL;
	} else {
		/* mark component as out of synch */
		/*ecalcomp_set_synch_state (comp, ECALCOMP_LOCALLY_MODIFIED);*/
	}

	if (did_put) {
		if (new_components && !*new_components) {
			/* read the comp from cache again, as some servers can modify it on put */
			*new_components = g_slist_prepend (*new_components, get_ecalcomp_master_from_cache_or_fallback (cbdav, id->uid, id->rid, NULL));
		}
	}

	e_cal_component_free_id (id);
	icalcomponent_free (cache_comp);
	g_object_unref (comp);
	g_free (href);
	g_free (etag);
}

/* a busy_lock is supposed to be locked already, when calling this function */
static void
do_remove_objects (ECalBackendCalDAV *cbdav,
                   const GSList *ids,
                   ECalObjModType mod,
                   GSList **old_components,
                   GSList **new_components,
                   GCancellable *cancellable,
                   GError **perror)
{
	icalcomponent            *cache_comp;
	gboolean                  online;
	gchar *href = NULL, *etag = NULL;
	const gchar *uid = ((ECalComponentId *) ids->data)->uid;
	const gchar *rid = ((ECalComponentId *) ids->data)->rid;

	if (new_components)
		*new_components = NULL;

	if (!check_state (cbdav, &online, perror))
		return;

	/* We make the assumption that the ids list we're passed is always exactly one element long, since we haven't specified "bulk-removes"
	 * in our static capability list. This simplifies a lot of the logic, especially around asynchronous results. */
	if (ids->next != NULL) {
		g_propagate_error (perror, e_data_cal_create_error (UnsupportedMethod, _("CalDAV does not support bulk removals")));
		return;
	}

	cache_comp = get_comp_from_cache (cbdav, uid, NULL, &href, &etag);

	if (cache_comp == NULL) {
		g_propagate_error (perror, EDC_ERROR (ObjectNotFound));
		return;
	}

	if (old_components) {
		ECalComponent *old = e_cal_backend_store_get_component (cbdav->priv->store, uid, rid);

		if (old) {
			*old_components = g_slist_prepend (*old_components, e_cal_component_clone (old));
			g_object_unref (old);
		} else {
			icalcomponent *master = get_master_comp (cbdav, cache_comp);
			if (master) {
				*old_components = g_slist_prepend (*old_components, e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master)));
			}
		}
	}

	switch (mod) {
	case E_CAL_OBJ_MOD_ONLY_THIS:
	case E_CAL_OBJ_MOD_THIS:
		if (rid && *rid) {
			/* remove one instance from the component */
			if (remove_instance (cbdav, cache_comp, icaltime_from_string (rid), mod, mod != E_CAL_OBJ_MOD_ONLY_THIS)) {
				if (new_components) {
					icalcomponent *master = get_master_comp (cbdav, cache_comp);
					if (master) {
						*new_components = g_slist_prepend (*new_components, e_cal_component_new_from_icalcomponent (icalcomponent_new_clone (master)));
					}
				}
			} else {
				/* this was the last instance, thus delete whole component */
				rid = NULL;
				remove_comp_from_cache (cbdav, uid, NULL);
			}
		} else {
			/* remove whole object */
			remove_comp_from_cache (cbdav, uid, NULL);
		}
		break;
	case E_CAL_OBJ_MOD_ALL:
		remove_comp_from_cache (cbdav, uid, NULL);
		break;
	case E_CAL_OBJ_MOD_THIS_AND_PRIOR:
	case E_CAL_OBJ_MOD_THIS_AND_FUTURE:
		break;
	}

	if (online) {
		CalDAVObject caldav_object;

		caldav_object.href  = href;
		caldav_object.etag  = etag;
		caldav_object.cdata = NULL;

		if (mod == E_CAL_OBJ_MOD_THIS && rid && *rid) {
			caldav_object.cdata = pack_cobj (cbdav, cache_comp);

			caldav_server_put_object (cbdav, &caldav_object, cache_comp, cancellable, perror);
		} else
			caldav_server_delete_object (cbdav, &caldav_object, cancellable, perror);

		caldav_object_free (&caldav_object, FALSE);
		href = NULL;
		etag = NULL;
	} else {
		/* mark component as out of synch */
		/*if (mod == E_CAL_OBJ_MOD_THIS && rid && *rid)
			ecalcomp_set_synch_state (cache_comp_master, ECALCOMP_LOCALLY_MODIFIED);
		else
			ecalcomp_set_synch_state (cache_comp_master, ECALCOMP_LOCALLY_DELETED);*/
	}
	remove_cached_attachment (cbdav, uid);

	icalcomponent_free (cache_comp);
	g_free (href);
	g_free (etag);
}

static void
extract_objects (icalcomponent *icomp,
                 icalcomponent_kind ekind,
                 GSList **objects,
                 GError **error)
{
	icalcomponent         *scomp;
	icalcomponent_kind     kind;

	kind = icalcomponent_isa (icomp);

	if (kind == ekind) {
		*objects = g_slist_prepend (NULL, icomp);
		return;
	}

	if (kind != ICAL_VCALENDAR_COMPONENT) {
		g_propagate_error (error, EDC_ERROR (InvalidObject));
		return;
	}

	*objects = NULL;
	scomp = icalcomponent_get_first_component (icomp, ekind);

	while (scomp) {
		/* Remove components from toplevel here */
		*objects = g_slist_prepend (*objects, scomp);
		icalcomponent_remove_component (icomp, scomp);

		scomp = icalcomponent_get_next_component (icomp, ekind);
	}
}

static gboolean
extract_timezones (ECalBackendCalDAV *cbdav,
                   icalcomponent *icomp)
{
	ETimezoneCache *timezone_cache;
	GSList *timezones = NULL, *iter;
	icaltimezone *zone;
	GError *err = NULL;

	g_return_val_if_fail (cbdav != NULL, FALSE);
	g_return_val_if_fail (icomp != NULL, FALSE);

	timezone_cache = E_TIMEZONE_CACHE (cbdav);

	extract_objects (icomp, ICAL_VTIMEZONE_COMPONENT, &timezones, &err);
	if (err) {
		g_error_free (err);
		return FALSE;
	}

	zone = icaltimezone_new ();
	for (iter = timezones; iter; iter = iter->next) {
		if (icaltimezone_set_component (zone, iter->data)) {
			e_timezone_cache_add_timezone (timezone_cache, zone);
		} else {
			icalcomponent_free (iter->data);
		}
	}

	icaltimezone_free (zone, TRUE);
	g_slist_free (timezones);

	return TRUE;
}

static void
process_object (ECalBackendCalDAV *cbdav,
                ECalComponent *ecomp,
                gboolean online,
                icalproperty_method method,
                GCancellable *cancellable,
                GError **error)
{
	ESourceRegistry *registry;
	ECalBackend              *backend;
	struct icaltimetype       now;
	gchar *new_obj_str;
	gboolean is_declined, is_in_cache;
	ECalObjModType mod;
	ECalComponentId *id = e_cal_component_get_id (ecomp);
	GError *err = NULL;

	backend = E_CAL_BACKEND (cbdav);

	if (id == NULL) {
		g_set_error_literal (
			error, E_CAL_CLIENT_ERROR,
			E_CAL_CLIENT_ERROR_INVALID_OBJECT,
			e_cal_client_error_to_string (
			E_CAL_CLIENT_ERROR_INVALID_OBJECT));
		return;
	}

	registry = e_cal_backend_get_registry (E_CAL_BACKEND (cbdav));

	/* ctime, mtime */
	now = icaltime_current_time_with_zone (icaltimezone_get_utc_timezone ());
	e_cal_component_set_created (ecomp, &now);
	e_cal_component_set_last_modified (ecomp, &now);

	/* just to check whether component exists in a cache */
	is_in_cache = cache_contains (cbdav, id->uid, NULL) || cache_contains (cbdav, id->uid, id->rid);

	new_obj_str = e_cal_component_get_as_string (ecomp);
	mod = e_cal_component_is_instance (ecomp) ? E_CAL_OBJ_MOD_THIS : E_CAL_OBJ_MOD_ALL;

	switch (method) {
	case ICAL_METHOD_PUBLISH:
	case ICAL_METHOD_REQUEST:
	case ICAL_METHOD_REPLY:
		is_declined = e_cal_backend_user_declined (
			registry, e_cal_component_get_icalcomponent (ecomp));
		if (is_in_cache) {
			if (!is_declined) {
				GSList *new_components = NULL, *old_components = NULL;
				GSList new_obj_strs = {0,};

				new_obj_strs.data = new_obj_str;
				do_modify_objects (cbdav, &new_obj_strs, mod,
						  &old_components, &new_components, cancellable, &err);
				if (!err && new_components && new_components->data) {
					if (!old_components || !old_components->data) {
						e_cal_backend_notify_component_created (backend, new_components->data);
					} else {
						e_cal_backend_notify_component_modified (backend, old_components->data, new_components->data);
					}
				}

				e_util_free_nullable_object_slist (old_components);
				e_util_free_nullable_object_slist (new_components);
			} else {
				GSList *new_components = NULL, *old_components = NULL;
				GSList ids = {0,};

				ids.data = id;
				do_remove_objects (cbdav, &ids, mod, &old_components, &new_components, cancellable, &err);
				if (!err && old_components && old_components->data) {
					if (new_components && new_components->data) {
						e_cal_backend_notify_component_modified (backend, old_components->data, new_components->data);
					} else {
						e_cal_backend_notify_component_removed (backend, id, old_components->data, NULL);
					}
				}

				e_util_free_nullable_object_slist (old_components);
				e_util_free_nullable_object_slist (new_components);
			}
		} else if (!is_declined) {
			GSList *new_components = NULL;
			GSList new_objs = {0,};

			new_objs.data = new_obj_str;

			do_create_objects (cbdav, &new_objs, NULL, &new_components, cancellable, &err);

			if (!err) {
				if (new_components && new_components->data)
					e_cal_backend_notify_component_created (backend, new_components->data);
			}

			e_util_free_nullable_object_slist (new_components);
		}
		break;
	case ICAL_METHOD_CANCEL:
		if (is_in_cache) {
			GSList *new_components = NULL, *old_components = NULL;
			GSList ids = {0,};

			ids.data = id;
			do_remove_objects (cbdav, &ids, E_CAL_OBJ_MOD_THIS, &old_components, &new_components, cancellable, &err);
			if (!err && old_components && old_components->data) {
				if (new_components && new_components->data) {
					e_cal_backend_notify_component_modified (backend, old_components->data, new_components->data);
				} else {
					e_cal_backend_notify_component_removed (backend, id, old_components->data, NULL);
				}
			}

			e_util_free_nullable_object_slist (old_components);
			e_util_free_nullable_object_slist (new_components);
		} else {
			err = EDC_ERROR (ObjectNotFound);
		}
		break;

	default:
		err = EDC_ERROR (UnsupportedMethod);
		break;
	}

	e_cal_component_free_id (id);
	g_free (new_obj_str);

	if (err)
		g_propagate_error (error, err);
}

static void
do_receive_objects (ECalBackendSync *backend,
                    EDataCal *cal,
                    GCancellable *cancellable,
                    const gchar *calobj,
                    GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	icalcomponent            *icomp;
	icalcomponent_kind        kind;
	icalproperty_method       tmethod;
	gboolean                  online;
	GSList                   *objects, *iter;
	GError *err = NULL;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	if (!check_state (cbdav, &online, perror))
		return;

	icomp = icalparser_parse_string (calobj);

	/* Try to parse cal object string */
	if (icomp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidObject));
		return;
	}

	kind = e_cal_backend_get_kind (E_CAL_BACKEND (backend));
	extract_objects (icomp, kind, &objects, &err);

	if (err) {
		icalcomponent_free (icomp);
		g_propagate_error (perror, err);
		return;
	}

	/* Extract optional timezone compnents */
	extract_timezones (cbdav, icomp);

	tmethod = icalcomponent_get_method (icomp);

	for (iter = objects; iter && !err; iter = iter->next) {
		icalcomponent       *scomp;
		ECalComponent       *ecomp;
		icalproperty_method  method;

		scomp = (icalcomponent *) iter->data;
		ecomp = e_cal_component_new ();

		e_cal_component_set_icalcomponent (ecomp, scomp);

		if (icalcomponent_get_first_property (scomp, ICAL_METHOD_PROPERTY)) {
			method = icalcomponent_get_method (scomp);
		} else {
			method = tmethod;
		}

		process_object (cbdav, ecomp, online, method, cancellable, &err);
		g_object_unref (ecomp);
	}

	g_slist_free (objects);

	icalcomponent_free (icomp);

	if (err)
		g_propagate_error (perror, err);
}

#define gtasks_busy_stub(_func_name, _params, _call_func, _call_params)	\
static void								\
_func_name _params							\
{									\
	ECalBackendGTasks        *cbgtasks;				\
	SlaveCommand		  old_slave_cmd;			\
	gboolean		  was_slave_busy;			\
									\
	cbgtasks = E_CAL_BACKEND_CALDAV (backend);				\
									\
	/* this is done before locking */				\
	old_slave_cmd = cbgtasks->priv->slave_cmd;				\
	was_slave_busy = cbgtasks->priv->slave_busy;			\
	if (was_slave_busy) {						\
		/* let it pause its work and do our job */		\
		update_slave_cmd (cbgtasks->priv, SLAVE_SHOULD_SLEEP);	\
	}								\
									\
	g_mutex_lock (&cbgtasks->priv->busy_lock);				\
	_call_func _call_params;					\
									\
	/* this is done before unlocking */				\
	if (was_slave_busy) {						\
		update_slave_cmd (cbgtasks->priv, old_slave_cmd);		\
		g_cond_signal (&cbgtasks->priv->cond);			\
	}								\
									\
	g_mutex_unlock (&cbgtasks->priv->busy_lock);			\
}

gtasks_busy_stub (
        gtasks_create_objects,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *in_calobjs,
                  GSList **uids,
                  GSList **new_components,
                  GError **perror),
        do_create_objects,
                  (cbdav,
                  in_calobjs,
                  uids,
                  new_components,
                  cancellable,
                  perror))

gtasks_busy_stub (
        gtasks_modify_objects,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *calobjs,
                  ECalObjModType mod,
                  GSList **old_components,
                  GSList **new_components,
                  GError **perror),
        do_modify_objects,
                  (cbdav,
                  calobjs,
                  mod,
                  old_components,
                  new_components,
                  cancellable,
                  perror))

gtasks_busy_stub (
        gtasks_remove_objects,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *ids,
                  ECalObjModType mod,
                  GSList **old_components,
                  GSList **new_components,
                  GError **perror),
        do_remove_objects,
                  (cbdav,
                  ids,
                  mod,
                  old_components,
                  new_components,
                  cancellable,
                  perror))

gtasks_busy_stub (
        gtasks_receive_objects,
                  (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const gchar *calobj,
                  GError **perror),
        do_receive_objects,
                  (backend,
                  cal,
                  cancellable,
                  calobj,
                  perror))

static void
gtasks_send_objects (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *calobj,
                     GSList **users,
                     gchar **modified_calobj,
                     GError **perror)
{
	*users = NULL;
	*modified_calobj = g_strdup (calobj);
}

static void
gtasks_get_object (ECalBackendSync *backend,
                   EDataCal *cal,
                   GCancellable *cancellable,
                   const gchar *uid,
                   const gchar *rid,
                   gchar **object,
                   GError **perror)
{
	ECalBackendCalGTasks *cbgtasks;
	ECalComponent *comp = NULL;
	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	*object = NULL;

	if (!priv->store) {
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	comp = e_cal_backend_store_get_components_by_uid_as_ical_string (priv->store, uid);

	if (!*object && e_backend_get_online (E_BACKEND (backend))) {
		/* try to fetch from the server, maybe the event was received only recently */
		if (gtasks_query_for_uid (cbgtasks, uid, cancellable, NULL)) {
			comp = e_cal_backend_store_get_components_by_uid_as_ical_string (priv->store, uid);
		}
	}

	if (!*object)
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));

	*object = e_cal_component_get_as_string (comp);
}

/* FIXME returning NOT_SUPPORTED */
static void
gtasks_add_timezone (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *tzobj,
                     GError **error)
{
	GError **local_error;
	g_set_error (local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support timezones");
	g_propagate_error (error, local_error);
}

static void
gtasks_get_free_busy (ECalBackendSync *backend,
                      EDataCal *cal,
                      GCancellable *cancellable,
                      const GSList *users,
                      time_t start,
                      time_t end,
                      GSList **freebusy,
                      GError **error)
{
	GError **local_error;
	g_set_error (local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support timezones");
	g_propagate_error (error, local_error);
}
}

static void
gtasks_get_object_list (ECalBackendSync *backend,
                        EDataCal *cal,
                        GCancellable *cancellable,
                        const gchar *sexp_string,
                        GSList **objects,
                        GError **perror)
{
	ECalBackendCalDAV        *cbdav;
	ECalBackendSExp	 *sexp;
	ETimezoneCache *cache;
	gboolean                  do_search;
	GSList			 *list, *iter;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;

	cbdav = E_CAL_BACKEND_CALDAV (backend);

	sexp = e_cal_backend_sexp_new (sexp_string);

	if (sexp == NULL) {
		g_propagate_error (perror, EDC_ERROR (InvalidQuery));
		return;
	}

	if (g_str_equal (sexp_string, "#t")) {
		do_search = FALSE;
	} else {
		do_search = TRUE;
	}

	*objects = NULL;

	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (sexp, &occur_start, &occur_end);

	cache = E_TIMEZONE_CACHE (backend);

	list = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (cbdav->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbdav->priv->store);

	for (iter = list; iter; iter = g_slist_next (iter)) {
		ECalComponent *comp = E_CAL_COMPONENT (iter->data);

		if (!do_search ||
		    e_cal_backend_sexp_match_comp (sexp, comp, cache)) {
			*objects = g_slist_prepend (*objects, e_cal_component_get_as_string (comp));
		}

		g_object_unref (comp);
	}

	g_object_unref (sexp);
	g_slist_free (list);
}

static void
gtasks_start_view (ECalBackend *backend,
                   EDataCalView *query)
{
	ECalBackendGTasks *cbgtasks;
	ECalBackendSExp	 *sexp;
	ETimezoneCache *cache;
	gboolean do_search;
	GSList *list, *iter;
	const gchar *sexp_string;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;
	
	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	sexp = e_data_cal_view_get_sexp (query);
	sexp_string = e_cal_backend_sexp_text (sexp);

	if (g_str_equal (sexp_string, "#t")) {
		do_search = FALSE;
	} else {
		do_search = TRUE;
	}
	prunning_by_time = e_cal_backend_sexp_evaluate_occur_times (
		sexp,
		&occur_start,
		&occur_end);

	cache = E_TIMEZONE_CACHE (backend);

	list = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (cbdav->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbdav->priv->store);

	for (iter = list; iter; iter = g_slist_next (iter)) {
		ECalComponent *comp = E_CAL_COMPONENT (iter->data);

		if (!do_search ||
		    e_cal_backend_sexp_match_comp (sexp, comp, cache)) {
			e_data_cal_view_notify_components_added_1 (query, comp);
		}

		g_object_unref (comp);
	}

	g_slist_free (list);

	e_data_cal_view_notify_complete (query, NULL /* Success */);
}

/* FIXME where it should be binded as callback */
static void
gtasks_notify_online_cb (ECalBackend *backend,
                         GParamSpec *pspec)
{
	ECalBackendGTasks        *cbgtasks;
	gboolean online;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	/*g_mutex_lock (&cbgtasks->priv->busy_lock);*/

	online = e_backend_get_online (E_BACKEND (backend));

	if (!cbdav->priv->loaded) {
		/*g_mutex_unlock (&cbgtasks->priv->busy_lock);*/
		return;
	}

	if (online) {
		/* Wake up the slave thread */
		update_slave_cmd (cbgtasks->priv, SLAVE_SHOULD_WORK);
		g_cond_signal (&cbgtasks->priv->cond);
	} else {
		soup_session_abort (cbgtasks->priv->session);
		update_slave_cmd (cbgtasks->priv, SLAVE_SHOULD_SLEEP);
	}

	/*g_mutex_unlock (&cbgtasks->priv->busy_lock);*/
}

static gpointer
gtasks_source_changed_thread (gpointer data)
{
	ECalBackendGTasks *gtasks = data;
	SlaveCommand old_slave_cmd;
	gboolean old_slave_busy;

	g_return_val_if_fail (gtasks != NULL, NULL);

	old_slave_cmd = gtasks->priv->slave_cmd;
	old_slave_busy = gtasks->priv->slave_busy;
	if (old_slave_busy) {
		update_slave_cmd (gtasks->priv, SLAVE_SHOULD_SLEEP);
		g_mutex_lock (&gtasks->priv->busy_lock);
	}

	initialize_backend (gtasks, NULL);

	/* always wakeup thread, even when it was sleeping */
	g_cond_signal (&gtasks->priv->cond);

	if (old_slave_busy) {
		update_slave_cmd (gtasks->priv, old_slave_cmd);
		g_mutex_unlock (&gtasks->priv->busy_lock);
	}

	gtasks->priv->updating_source = FALSE;

	g_object_unref (gtasks);

	return NULL;
}

static void
gtasks_source_changed_cb (ESource *source,
                          ECalBackendGTasks *cbgtasks)
{
	GThread *thread;

	g_return_if_fail (source != NULL);
	g_return_if_fail (cbdav != NULL);

	if (cbgtasks->priv->updating_source)
		return;

	cbgtasks->priv->updating_source = TRUE;

	thread = g_thread_new (NULL, caldav_source_changed_thread, g_object_ref (cbgtasks));
	g_thread_unref (thread);
}

/* FIXME how authenticator will work in case of GOA, do I need that */
/* Removing for now */

/* ************************************************************************* */
/* ***************************** GObject Foo ******************************* */

static void
e_cal_backend_gtasks_dispose (GObject *object)
{
	ECalBackendGTasksPrivate *priv;

	priv = E_CAL_BACKEND_GTASKS_GET_PRIVATE (object);

	/* FIXME what's dispose do */
	//g_clear_object (&priv->store);
	//g_clear_object (&priv->session);
	//g_clear_object (&priv->proxy);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
e_cal_backend_gtasks_finalize (GObject *object)
{
	ECalBackendGTasksPrivate *priv;

	priv = E_CAL_BACKEND_GTASKS_GET_PRIVATE (object);

	/* FIXME what is this slave sync */
	//g_mutex_clear (&priv->busy_lock);
	//g_cond_clear (&priv->cond);
	//g_cond_clear (&priv->slave_gone_cond);

	/* FIXME remember to free gchar* but not const gchar* */

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
e_cal_backend_gtasks_constructed (GObject *object)
{
	/* FIXME What constructed do? */
	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_cal_backend_gtasks_parent_class)->
		constructed (object);
}

static void
e_cal_backend_gtasks_init (ECalBackendGTasks *cbdav)
{
	
}

static void
e_cal_backend_gtasks_class_init (ECalBackendGTasksClass *class)
{
	GObjectClass *object_class;
	ECalBackendClass *backend_class;
	ECalBackendSyncClass *sync_class;

	object_class = (GObjectClass *) class;
	backend_class = (ECalBackendClass *) class;
	sync_class = (ECalBackendSyncClass *) class;

	/* FIXME */
	//caldav_debug_init ();

	parent_class = (ECalBackendSyncClass *) g_type_class_peek_parent (class);
	g_type_class_add_private (class, sizeof (ECalBackendGTasksPrivate));

	object_class->dispose  = e_cal_backend_gtasks_dispose;
	object_class->finalize = e_cal_backend_gtasks_finalize;
	object_class->constructed = e_cal_backend_gtasks_constructed;

	backend_class->get_backend_property = gtasks_get_backend_property;
	backend_class->shutdown = gtasks_shutdown;

	sync_class->open_sync			= gtasks_do_open;
	sync_class->refresh_sync		= gtasks_refresh;

	sync_class->create_objects_sync		= gtasks_create_objects;
	sync_class->modify_objects_sync		= gtasks_modify_objects;
	sync_class->remove_objects_sync		= gtasks_remove_objects;

	sync_class->receive_objects_sync	= gtasks_receive_objects;
	sync_class->send_objects_sync		= gtasks_send_objects;
	// FIXME understood
	sync_class->get_object_sync			= gtasks_get_object;
	sync_class->get_object_list_sync	= gtasks_get_object_list;
	// returns E_CLIENT_ERROR_NOT_SUPPORTED
	sync_class->add_timezone_sync		= gtasks_add_timezone;
	sync_class->get_free_busy_sync		= gtasks_get_free_busy;

	backend_class->start_view		= gtasks_start_view;
}
