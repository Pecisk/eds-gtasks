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

#include "e-cal-backend-gtasks.h"
#include <gdata/gdata.h>

#define E_CAL_BACKEND_GTASKS_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_CAL_BACKEND_GTASKS, ECalBackendGTasksPrivate))

#define EDC_ERROR(_code) e_data_cal_create_error (_code, NULL)
#define EDC_ERROR_EX(_code, _msg) e_data_cal_create_error (_code, _msg)

/* in seconds */
#define DEFAULT_REFRESH_TIME 60

/* Private part of the ECalBackendGTasks structure */
struct _ECalBackendGTasksPrivate {
	/* id of tasklist */
	gchar *tasklist_id;

	/* The local disk cache */
	ECalBackendStore *store;

	/* TRUE after gtasks_open indicates that backend is initialised and loaded */
	gboolean loaded;
	/* TRUE when server reachable */
	gboolean opened;

	guint refresh_id;
	guint is_loading;

	/* gdata stuff */
	//GDataGoaAuthorizer *authorizer;
	//GDataTasksService *service;
};

G_DEFINE_TYPE (ECalBackendGTasks, e_cal_backend_gtasks, E_TYPE_CAL_BACKEND_SYNC)

static void e_cal_backend_gtasks_dispose (GObject *object);
static void e_cal_backend_gtasks_finalize (GObject *object);
static void e_cal_backend_gtasks_constructed (GObject *object);

static gchar * gtasks_get_backend_property (ECalBackend *backend, const gchar *prop_name);
static void gtasks_start_view (ECalBackend *backend, EDataCalView *query);
static gboolean initialize_backend (ECalBackendGTasks *cbgtasks, GError **perror);
static gboolean open_tasks (ECalBackendGTasks *cbgtasks, GCancellable *cancellable, GError **error);
static void gtasks_do_open (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, gboolean only_if_exists, GError **perror);
static void gtasks_refresh (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, GError **perror);
/* manipulations with objects */
//static gboolean gtasks_query_for_uid (ECalBackendGTasks *cbgtasks, const gchar *uid, GCancellable *cancellable, GError **error);
static void gtasks_get_object (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *uid, const gchar *rid, gchar **object, GError **perror);
static void gtasks_get_object_list (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *sexp_string, GSList **objects, GError **perror);
static void gtasks_create_objects (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const GSList *in_calobjs, GSList **uids, GSList **new_components, GError **perror);
static void gtasks_modify_objects (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const GSList *in_calobjs, GSList **uids, GSList **new_components, GError **perror);
static void gtasks_remove_objects (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const GSList *in_calobjs, GSList **uids, GSList **new_components, GError **perror);
static void gtasks_receive_objects (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *calobj, GError **perror);
static void gtasks_send_objects (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *calobj, GSList **users, gchar **modified_calobj, GError **perror);
/* timezone and free_busy not implemented */
static void gtasks_add_timezone (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const gchar *tzobj, GError **error);
static void gtasks_get_free_busy (ECalBackendSync *backend, EDataCal *cal, GCancellable *cancellable, const GSList *users, time_t start, time_t end, GSList **freebusy, GError **error);
/* callbacks */
static void gtasks_source_changed_cb (ESource *source, ECalBackendGTasks *cbgtasks);
static void gtasks_time_to_refresh_cb (ESource *source, gpointer user_data);
static void gtasks_sync_store_cb (GIOSchedulerJob *job, GCancellable *cancellable, ECalBackendGTasks *cbgtasks);
//static void gtasks_write_task_to_component (ECalComponent *comp, GDataTasksTask *task);
static void gtasks_notify_online_cb (ECalBackend *backend, GParamSpec *pspec);
static gboolean gtasks_begin_retrieval_cb (GIOSchedulerJob *job, GCancellable *cancellable, ECalBackendGTasks *backend);

/* ************************************************ GObject stuff */

static void
e_cal_backend_gtasks_init (ECalBackendGTasks *gtasks)
{
	gtasks->priv->loaded = FALSE;
	gtasks->priv->opened = FALSE;
	g_signal_connect (gtasks, "notify::online", G_CALLBACK (gtasks_notify_online_cb), NULL);
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

	g_type_class_add_private (class, sizeof (ECalBackendGTasksPrivate));

	object_class->dispose  = e_cal_backend_gtasks_dispose;
	object_class->finalize = e_cal_backend_gtasks_finalize;
	object_class->constructed = e_cal_backend_gtasks_constructed;

	backend_class->get_backend_property = gtasks_get_backend_property;
	backend_class->start_view = gtasks_start_view;
	
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


}

static void
e_cal_backend_gtasks_dispose (GObject *object)
{
	ECalBackendGTasksPrivate *priv;

	priv = E_CAL_BACKEND_GTASKS_GET_PRIVATE (object);

	/* FIXME which objects also need clearing */
	g_clear_object (&priv->store);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_cal_backend_gtasks_parent_class)->dispose (object);
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
	G_OBJECT_CLASS (e_cal_backend_gtasks_parent_class)->finalize (object);
}

static void
e_cal_backend_gtasks_constructed (GObject *object)
{
	/* FIXME What constructed do? */
	/* Chain up to parent's constructed() method. */
	G_OBJECT_CLASS (e_cal_backend_gtasks_parent_class)->constructed (object);
}

/* ************************************************* ECalBackend stuff */

static gchar *
gtasks_get_backend_property (ECalBackend *backend,
                             const gchar *prop_name)
{
	///* FIXME what properties I need to return */
	//g_return_val_if_fail (prop_name != NULL, FALSE);

	//if (g_str_equal (prop_name, CLIENT_BACKEND_PROPERTY_CAPABILITIES)) {
		//ESourceGTasks *extension;
		//ESource *source;
		//GString *caps;
		//gchar *usermail;
		//const gchar *extension_name;

		//caps = g_string_new (
			//CAL_STATIC_CAPABILITY_NO_THISANDFUTURE ","
			//CAL_STATIC_CAPABILITY_NO_THISANDPRIOR ","
			//CAL_STATIC_CAPABILITY_REFRESH_SUPPORTED);

		//usermail = get_usermail (E_CAL_BACKEND (backend));
		//if (!usermail || !*usermail)
			//g_string_append (caps, "," CAL_STATIC_CAPABILITY_NO_EMAIL_ALARMS);
		//g_free (usermail);

		//return g_string_free (caps, FALSE);

	//} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_CAL_EMAIL_ADDRESS) ||
		   //g_str_equal (prop_name, CAL_BACKEND_PROPERTY_ALARM_EMAIL_ADDRESS)) {
		//return get_usermail (E_CAL_BACKEND (backend));

	//} else if (g_str_equal (prop_name, CAL_BACKEND_PROPERTY_DEFAULT_OBJECT)) {
		//ECalComponent *comp;
		//gchar *prop_value;

		//comp = e_cal_component_new ();

		//switch (e_cal_backend_get_kind (E_CAL_BACKEND (backend))) {
		//case ICAL_VEVENT_COMPONENT:
			//e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_EVENT);
			//break;
		//case ICAL_VTODO_COMPONENT:
			//e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_TODO);
			//break;
		//case ICAL_VJOURNAL_COMPONENT:
			//e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_JOURNAL);
			//break;
		//default:
			//g_object_unref (comp);
			//return NULL;
		//}

		//prop_value = e_cal_component_get_as_string (comp);

		//g_object_unref (comp);

		//return prop_value;
	//}

	/* Chain up to parent's get_backend_property() method. */
	//return E_CAL_BACKEND_CLASS (e_cal_backend_gtasks_parent_class)->get_backend_property (backend, prop_name);
	return NULL;
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
		e_cal_backend_store_get_components_occuring_in_range (cbgtasks->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbgtasks->priv->store);

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

/* ********************************************* ECalBackendSync stuff */

static gboolean
initialize_backend (ECalBackendGTasks *cbgtasks,
                    GError **perror)
{
	ECalBackend              *backend;
	ESource                  *source;
	gsize                     len;
	const gchar              *cache_dir;
	const gchar              *extension_name;

	backend = E_CAL_BACKEND (cbgtasks);
	cache_dir = e_cal_backend_get_cache_dir (backend);
	source = e_backend_get_source (E_BACKEND (backend));

	if (!g_signal_handler_find (G_OBJECT (source), G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0, NULL, gtasks_source_changed_cb, cbgtasks))
		g_signal_connect (G_OBJECT (source), "changed", G_CALLBACK (gtasks_source_changed_cb), cbgtasks);

	if (cbgtasks->priv->store == NULL) {
		/* remove the old cache while migrating to ECalBackendStore */
		e_cal_backend_cache_remove (cache_dir, "cache.xml");
		cbgtasks->priv->store = e_cal_backend_store_new (
			cache_dir, E_TIMEZONE_CACHE (cbgtasks));
		e_cal_backend_store_load (cbgtasks->priv->store);
	}

	if (cbgtasks->priv->refresh_id == 0) {
		cbgtasks->priv->refresh_id = e_source_refresh_add_timeout (
			source, NULL, gtasks_time_to_refresh_cb, cbgtasks, NULL);
	}

	return TRUE;
}

gboolean
gtasks_load (ECalBackendGTasks *cbgtasks, GCancellable *cancellable, GError **error)
{	
	GDataFeed *feed = gdata_tasks_get_tasks_by_tasklist_id (cbgtasks->priv->service, "tasklist-id", cancellable, error);
	// If we don't get feed, return FALSE
	// If there's no GError it's internal
	if (feed == NULL)
		return FALSE;

	GList *entries = gdata_feed_get_entries (feed);
	while (entries != NULL) {
		GDataTasksTask *task = GDATA_TASKS_TASK (entries->data);
		// FIXME if old_object can't be found, remove it from backend store
		// FIXME if old_object has older updated, refresh information
		while (old_objects != NULL) {
				ECalComponent *old_component = E_CAL_COMPONENT (old_objects->data);
				const gchar *old_component_id;
				e_cal_component_get_uid (old_component, old_component_id);
				if (g_str_equal (gdata_entry_get_id (GDATA_ENTRY (task)), old_component_id) {
					found = TRUE;
					if (gdata_entry_get_updated (GDATA_ENTRY (task)) == e_cal_component_get_last_modified (comp))
						return;
					/* if they are not equal, we sync from server */
					ECalComponent *new_component = e_cal_component_new (void);
					gtasks_write_task_to_component (new_component, task);
					e_cal_backend_notify_component_modified (cbgtasks, old_component, new_component);
				}
			old_objects = old_objects->next;
		}
		/* FIXME if we haven't found server entry, we create new one */
		if (found == FALSE) {
			ECalComponent *new_component = e_cal_component_new (void);
			gtasks_write_task_to_component (new_component, task);
			// add it to storage
			e_cal_backend_store_put_component (cbgtasks->priv->store, new_component);
			// notify view that component is created
			e_cal_backend_notify_component_created (cbgtasks, new_component);
		}
		// FIXME when to remove?
		//e_cal_backend_notify_component_removed ()
		old_objects = old_objects_start;
		entries = entries->next;
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

	g_return_val_if_fail (cbgtasks != NULL, FALSE);

	/* Creating service using authorizer */
	cbgtasks->priv->service = gdata_tasks_service_new (GDATA_AUTHORIZER (cbgtasks->priv->authorizer));

	/* Can't create service */
	g_return_val_if_fail (cbgtasks->priv->service != NULL, FALSE);

	success = gtasks_load (cbgtasks, cancellable, &local_error);

	/* We get FALSE only if it's internal error. For unreachable service we get TRUE with error */
	if (!success) {
		e_cal_backend_set_writable (E_CAL_BACKEND (cbgtasks), FALSE);
		if (local_error) {
			gchar *msg = g_strdup_printf (_("Server is unreachable, tasklist is opened in read-only mode.\nError message: %s"), local_error->message);
			e_cal_backend_notify_error (E_CAL_BACKEND (cbgtasks), msg);
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
	ECalBackendGTasks *cbgtasks;
	gboolean online;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	/* FIXME what is loaded */
	if (!cbgtasks->priv->loaded && !initialize_backend (cbgtasks, perror))
		return;

	online = e_backend_get_online (E_BACKEND (backend));

	/* Backend initialised and loaded */
	cbgtasks->priv->loaded = TRUE;

	if (online) {
		GError *local_error = NULL;
		// FIXME open_tasks means we download all tasks and create local compositions
		cbgtasks->priv->opened  = open_tasks (cbgtasks, cancellable, &local_error);
		// FIXME how we get which account we need? How do we get account id?
		// If we fail due of auth, what we do then?
		if (local_error != NULL)
			g_propagate_error (perror, local_error);

	} else {
		e_cal_backend_set_writable (E_CAL_BACKEND (cbgtasks), FALSE);
	}

}

static void
gtasks_refresh (ECalBackendSync *backend,
                EDataCal *cal,
                GCancellable *cancellable,
                GError **perror)
{
	ECalBackendGTasks *cbgtasks;
	ECalBackendGTasksPrivate *priv;
	ESource *source;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);
	priv = cbgtasks->priv;

	if (!priv->opened || priv->is_loading)
		return;

	source = e_backend_get_source (E_BACKEND (cbgtasks));
	g_return_if_fail (source != NULL);

	e_source_refresh_force_timeout (source);
}

/* *************************** get objects functions */

static gboolean
gtasks_query_for_uid (ECalBackendGTasks *cbgtasks,
                             const gchar *uid,
                             GCancellable *cancellable,
                             GError **error)
{
	//// FIXME do a query write this function in gdata 
	//GDataTasksTask *task = gdata_tasks_service_query_task_by_ids (cbgtasks->priv->service, uid, "id-of-tasklist", cancellable, error);

	//ECalComponent *comp = NULL;	
	//comp = e_cal_component_new ();

	//e_cal_component_set_new_vtype (comp, E_CAL_COMPONENT_TODO);

	//// FIXME can I pass NULL or do I should do ""
	//const gchar* notes = gdata_tasks_get_notes (task);
	//e_cal_component_set_summary (comp, {notes, NULL});

	//if (gdata_tasks_get_completed (tasks) != -1)
		//e_cal_component_set_completed (comp, icaltime_from_timet_with_zone (gdata_task_get_completed (task), 0, 0));
	//if (gdata_tasks_get_due (tasks) != -1)
		//e_cal_component_set_due (comp, icaltime_from_timet_with_zone (gdata_task_get_due (task), 0, 0));

	//if (g_str_equal (gdata_tasks_get_status (task), "completed")) {
		//e_cal_component_set_status (comp, ICAL_STATUS_COMPLETED);
	//} else {
		//// FIXME shouldn't this be NONE? Verify
		//e_cal_component_set_status (comp, ICAL_STATUS_NEEDSACTION);
	//}

	//// add it to storage
	//e_cal_backend_store_put_component (cbgtasks->priv->store, comp);
	//// notify view that component is created
	//e_cal_backend_notify_component_created (cbgtasks, comp);
	////e_cal_backend_notify_component_modified (cal_backend, old_comp, new_comp);
	//return result;
	return FALSE;
}

static void
gtasks_get_object (ECalBackendSync *backend,
                   EDataCal *cal,
                   GCancellable *cancellable,
                   const gchar *uid,
                   const gchar *rid,
                   gchar **object,
                   GError **error)
{
	ECalBackendGTasks *cbgtasks;
	ECalComponent *comp = NULL;
	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	*object = NULL;

	if (!cbgtasks->priv->store) {
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));
		return;
	}

	comp = e_cal_backend_store_get_components_by_uid_as_ical_string (cbgtasks->priv->store, uid);

	if (!*object && e_backend_get_online (E_BACKEND (backend))) {
		/* try to fetch from the server, maybe the event was received only recently */
		if (gtasks_query_for_uid (cbgtasks, uid, cancellable, NULL)) {
			comp = e_cal_backend_store_get_components_by_uid_as_ical_string (cbgtasks->priv->store, uid);
		}
	}

	if (!*object)
		g_propagate_error (error, EDC_ERROR (ObjectNotFound));

	*object = e_cal_component_get_as_string (comp);
}

static void
gtasks_get_object_list (ECalBackendSync *backend,
                        EDataCal *cal,
                        GCancellable *cancellable,
                        const gchar *sexp_string,
                        GSList **objects,
                        GError **perror)
{
	ECalBackendGTasks *cbgtasks;
	ECalBackendSExp *sexp;
	ETimezoneCache *cache;
	gboolean do_search;
	GSList *list, *iter;
	time_t occur_start = -1, occur_end = -1;
	gboolean prunning_by_time;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

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
	// FIXME how to handle timezone cache here, I have to implement interface?
	// I have to implement actions around it
	cache = E_TIMEZONE_CACHE (backend);

	list = prunning_by_time ?
		e_cal_backend_store_get_components_occuring_in_range (cbgtasks->priv->store, occur_start, occur_end)
		: e_cal_backend_store_get_components (cbgtasks->priv->store);

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

/* ************************************** functions which return NOT_SUPPORTED */
static void
gtasks_add_timezone (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *tzobj,
                     GError **error)
{
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support timezones");
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
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support timezones");
	g_propagate_error (error, local_error);
}

/* Not supported currently, needs implementation */

void gtasks_create_objects (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *in_calobjs,
                  GSList **uids,
                  GSList **new_components,
                  GError **error)
{
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support creation of objects.");
	g_propagate_error (error, local_error);
}

static void
gtasks_modify_objects (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *in_calobjs,
                  GSList **uids,
                  GSList **new_components,
                  GError **error)
{
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support modification of objects.");
	g_propagate_error (error, local_error);
}

static void
gtasks_remove_objects (ECalBackendSync *backend,
                  EDataCal *cal,
                  GCancellable *cancellable,
                  const GSList *in_calobjs,
                  GSList **uids,
                  GSList **new_components,
                  GError **error)
{
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't support removal of objects.");
	g_propagate_error (error, local_error);
}

static void
gtasks_receive_objects (ECalBackendSync *backend,
                        EDataCal *cal,
                        GCancellable *cancellable,
                        const gchar *calobj,
                        GError **error)
{
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't recieving of objects.");
	g_propagate_error (error, local_error);
}

static void
gtasks_send_objects (ECalBackendSync *backend,
                     EDataCal *cal,
                     GCancellable *cancellable,
                     const gchar *calobj,
                     GSList **users,
                     gchar **modified_calobj,
                     GError **error)
{
	//*users = NULL;
	//*modified_calobj = g_strdup (calobj);
	GError *local_error = NULL;
	g_set_error (&local_error, E_CLIENT_ERROR, E_CLIENT_ERROR_NOT_SUPPORTED, "Google Tasks backend doesn't sending objects.");
	g_propagate_error (error, local_error);
}

/* ********************************************* callback functions */

static void
gtasks_source_changed_cb (ESource *source,
                          ECalBackendGTasks *cbgtasks)
{
	/* FIXME this should be enough, but double check, it resets everyting */
	/* Also you need reload everying */
	g_return_if_fail (E_IS_CAL_BACKEND_GTASKS (cbgtasks));
	g_object_ref (cbgtasks);

	initialize_backend (cbgtasks, NULL);

	g_io_scheduler_push_job (
        (GIOSchedulerJobFunc) gtasks_begin_retrieval_cb,
        g_object_ref (cbgtasks),
        (GDestroyNotify) g_object_unref,
        G_PRIORITY_DEFAULT, NULL);
	
	g_object_unref (cbgtasks);
}

static void
gtasks_time_to_refresh_cb (ESource *source,
                                    gpointer user_data)
{
	ECalBackendGTasks *cbgtasks = user_data;

	g_return_if_fail (E_IS_CAL_BACKEND_GTASKS (cbgtasks));

	if (!e_backend_get_online (E_BACKEND (cbgtasks)))
		return;

	g_io_scheduler_push_job (
		(GIOSchedulerJobFunc) gtasks_sync_store_cb,
		g_object_ref (cbgtasks),
		(GDestroyNotify) g_object_unref,
		G_PRIORITY_DEFAULT, NULL);
}

static void
gtasks_sync_store_cb (GIOSchedulerJob *job,
                    GCancellable *cancellable,
                    ECalBackendGTasks *cbgtasks)
{
	//GSList *old_objects;
	
	///* FIXME get tasklist id */
	//GDataFeed *feed = gdata_tasks_service_get_tasks_by_tasklist_id (cbgtasks->priv->service, "tasklist-id", NULL, NULL);
	//GList *entries = gdata_feed_get_entries (feed);
	
	///* Currently stored objects */
	//old_objects = e_cal_backend_store_get_components (cbgtasks->priv->store);
	
	///* So we can return at the start of single linked list */
	//GSList *old_objects_start = old_objects;
	
	///* boolean value to check if entry is found in storage */
	//gboolean found = FALSE;
	
	//while (entries != NULL) {
		//GDataTasksTask *task = GDATA_TASKS_TASK (entries->data);
		//// FIXME if old_object can't be found, remove it from backend store
		//// FIXME if old_object has older updated, refresh information
		//while (old_objects != NULL) {
				//ECalComponent *old_component = E_CAL_COMPONENT (old_objects->data);
				//const gchar *old_component_id;
				//e_cal_component_get_uid (old_component, old_component_id);
				//if (g_str_equal (gdata_entry_get_id (GDATA_ENTRY (task)), old_component_id) {
					//found = TRUE;
					//if (gdata_entry_get_updated (GDATA_ENTRY (task)) == e_cal_component_get_last_modified (comp))
						//return;
					///* if they are not equal, we sync from server */
					//ECalComponent *new_component = e_cal_component_new (void);
					//gtasks_write_task_to_component (new_component, task);
					//e_cal_backend_notify_component_modified (cal_backend, old_component, new_component);
				//}
			//old_objects = old_objects->next;
		//}
		///* FIXME if we haven't found server entry, we create new one */
		//if (found == FALSE) {
			//ECalComponent *new_component = e_cal_component_new (void);
			//gtasks_write_task_to_component (new_component, task);
			//// add it to storage
			//e_cal_backend_store_put_component (cbgtasks->priv->store, new_component);
			//// notify view that component is created
			//e_cal_backend_notify_component_created (cbgtasks, new_component);
		//}
		//// FIXME when to remove?
		////e_cal_backend_notify_component_removed ()
		//old_objects = old_objects_start;
		//entries = entries->next;
	//}
}

//static void
//gtasks_write_task_to_component (ECalComponent *comp, GDataTasksTask *task) {
	///* Description */
	//ECalComponentText *desc = g_new (ECalComponentText, 1);
	//desc->value = g_strdup (gdata_tasks_get_notes (task));
	//GSList *desc_list = NULL;
	//g_slist_append (desc_list, desc);
	//e_cal_component_set_description_list (comp, desc_list);

	///* Summary */
	//ECalComponentText *summary = g_new (ECalComponentText, 1);
	//summary->value = g_strdup (gdata_tasks_get_title (task));
	//e_cal_component_set_summary (comp, summary);

	///* Completed */
	//if (gdata_tasks_get_completed (tasks) != -1)
	//e_cal_component_set_completed (comp, icaltime_from_timet_with_zone (gdata_task_get_completed (task), 0, 0));

	///* Due */
	//if (gdata_tasks_get_due (tasks) != -1)
	//e_cal_component_set_due (comp, icaltime_from_timet_with_zone (gdata_task_get_due (task), 0, 0));

	///* Status */
	//if (g_str_equal (gdata_tasks_get_status (task), "completed")) {
	//e_cal_component_set_status (comp, ICAL_STATUS_COMPLETED);
	//} else {
		//// FIXME shouldn't this be NONE? Verify
		//e_cal_component_set_status (comp, ICAL_STATUS_NEEDSACTION);
	//}
//}

/* Callback when backend gets alerted about */
static void
gtasks_notify_online_cb (ECalBackend *backend,
                         GParamSpec *pspec)
{
	ECalBackendGTasks *cbgtasks;
	gboolean online;

	cbgtasks = E_CAL_BACKEND_GTASKS (backend);

	online = e_backend_get_online (E_BACKEND (backend));

	if (!cbgtasks->priv->loaded)
		return;

	if (online) {
		// FIXME start to retrieve data
	} else {
		// FIXME stop retrieving
	}
}

static gboolean
gtasks_begin_retrieval_cb (GIOSchedulerJob *job,
                    GCancellable *cancellable,
                    ECalBackendGTasks *backend)
{
	//const gchar *uri;
	//GError *error = NULL;

	//if (!e_backend_get_online (E_BACKEND (backend)))
		//return FALSE;

	//if (backend->priv->is_loading)
		//return FALSE;

	//d (g_message ("Starting retrieval...\n"));

	//backend->priv->is_loading = TRUE;

	//uri = cal_backend_http_ensure_uri (backend);
	//open_tasks (backend, cancellable, uri, &error);

	//if (g_error_matches (error, SOUP_HTTP_ERROR, SOUP_STATUS_UNAUTHORIZED)) {
		//g_clear_error (&error);
		//e_backend_authenticate_sync (
			//E_BACKEND (backend),
			//E_SOURCE_AUTHENTICATOR (backend),
			//cancellable, &error);
	//}

	//backend->priv->is_loading = FALSE;

	///* Ignore cancellations. */
	//if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		//g_error_free (error);

	//} else if (error != NULL) {
		//e_cal_backend_notify_error (
			//E_CAL_BACKEND (backend),
			//error->message);
		//empty_cache (backend);
		//g_error_free (error);
	//}

	//d (g_message ("Retrieval really done.\n"));

	return FALSE;
}
