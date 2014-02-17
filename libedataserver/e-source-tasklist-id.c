/*
 * e-source-tasklist-id.c
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
 */

/**
 * SECTION: e-source-tasklist-id
 * @include: libedataserver/libedataserver.h
 * @short_description: #ESource extension for Google tasklist ID and title
 *
 * The #ESourceSecurity extension tracks settings for establishing a
 * secure connection with a remote server.
 *
 * Access the extension as follows:
 *
 * |[
 *   #include <libedataserver/libedataserver.h>
 *
 *   ESourceSecurity *extension;
 *
 *   extension = e_source_get_extension (source, E_SOURCE_EXTENSION_SECURITY);
 * ]|
 **/

#include "e-source-tasklist-id.h"

#include <libedataserver/e-data-server-util.h>

#define E_SOURCE_TASKLIST_ID_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_SOURCE_TASKLIST_ID, ESourceTasklistIDPrivate))

#define SECURE_METHOD "tls"

struct _ESourceTasklistIDPrivate {
	GMutex property_lock;
	gchar *id;
	gchar *title;
};

enum {
	PROP_0,
	PROP_ID,
	PROP_TITLE
};

G_DEFINE_TYPE (
	ESourceTasklistID,
	e_source_tasklist_id,
	E_TYPE_SOURCE_EXTENSION)

static void
source_tasklist_id_set_property (GObject *object,
                              guint property_id,
                              const GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ID:
			e_source_tasklist_id_set_id (
				E_SOURCE_TASKLIST_ID (object),
				g_value_get_string (value));
			return;

		case PROP_TITLE:
			e_source_tasklist_id_set_title (
				E_SOURCE_TASKLIST_ID (object),
				g_value_get_string (value));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_tasklist_id_get_property (GObject *object,
                              guint property_id,
                              GValue *value,
                              GParamSpec *pspec)
{
	switch (property_id) {
		case PROP_ID:
			g_value_take_string (
				value,
				e_source_tasklist_id_dup_id (
				E_SOURCE_TASKLIST_ID (object)));
			return;

		case PROP_TITLE:
			g_value_take_string (
				value,
				e_source_tasklist_id_dup_title (
				E_SOURCE_TASKLIST_ID (object)));
			return;
	}

	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
}

static void
source_tasklist_id_finalize (GObject *object)
{
	ESourceTasklistIDPrivate *priv;

	priv = E_SOURCE_TASKLIST_ID_GET_PRIVATE (object);

	g_mutex_clear (&priv->property_lock);

	g_free (priv->id);
	g_free (priv->title);

	/* Chain up to parent's finalize() method. */
	G_OBJECT_CLASS (e_source_tasklist_id_parent_class)->finalize (object);
}

static void
e_source_tasklist_id_class_init (ESourceTasklistIDClass *class)
{
	GObjectClass *object_class;
	ESourceExtensionClass *extension_class;

	g_type_class_add_private (class, sizeof (ESourceTasklistIDPrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->set_property = source_tasklist_id_set_property;
	object_class->get_property = source_tasklist_id_get_property;
	object_class->finalize = source_tasklist_id_finalize;

	extension_class = E_SOURCE_EXTENSION_CLASS (class);
	extension_class->name = E_SOURCE_EXTENSION_TASKLIST_ID;

	g_object_class_install_property (
		object_class,
		PROP_ID,
		g_param_spec_string (
			"id",
			"ID",
			"Tasklist ID",
			"none",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));

	g_object_class_install_property (
		object_class,
		PROP_TITLE,
		g_param_spec_string (
			"title",
			"Title",
			"Title of tasklist",
			"none",
			G_PARAM_READWRITE |
			G_PARAM_CONSTRUCT |
			G_PARAM_STATIC_STRINGS |
			E_SOURCE_PARAM_SETTING));
}

static void
e_source_tasklist_id_init (ESourceTasklistID *extension)
{
	extension->priv = E_SOURCE_TASKLIST_ID_GET_PRIVATE (extension);
	g_mutex_init (&extension->priv->property_lock);
}

const gchar *
e_source_tasklist_id_get_id (ESourceTasklistID *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_TASKLIST_ID (extension), NULL);

	return extension->priv->id;
}

gchar *
e_source_tasklist_id_dup_id (ESourceTasklistID *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_TASKLIST_ID (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_tasklist_id_get_id (extension);
	duplicate = g_strdup (protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

void
e_source_tasklist_id_set_id (ESourceTasklistID *extension,
                              const gchar *id)
{
	GObject *object;

	g_return_if_fail (E_IS_SOURCE_TASKLIST_ID (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (extension->priv->id &&
	    g_strcmp0 (extension->priv->id, id) == 0) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_free (extension->priv->id);
	extension->priv->id = e_util_strdup_strip (id);

	if (extension->priv->id == NULL)
		extension->priv->id = g_strdup ("none");

	g_mutex_unlock (&extension->priv->property_lock);

	object = G_OBJECT (extension);
	g_object_freeze_notify (object);
	g_object_notify (object, "id");
	g_object_notify (object, "title");
	g_object_thaw_notify (object);
}

const gchar *
e_source_tasklist_id_get_title (ESourceTasklistID *extension)
{
	g_return_val_if_fail (E_IS_SOURCE_TASKLIST_ID (extension), NULL);

	return extension->priv->title;
}

gchar *
e_source_tasklist_id_dup_title (ESourceTasklistID *extension)
{
	const gchar *protected;
	gchar *duplicate;

	g_return_val_if_fail (E_IS_SOURCE_TASKLIST_ID (extension), NULL);

	g_mutex_lock (&extension->priv->property_lock);

	protected = e_source_tasklist_id_get_title (extension);
	duplicate = g_strdup (protected);

	g_mutex_unlock (&extension->priv->property_lock);

	return duplicate;
}

void
e_source_tasklist_id_set_title (ESourceTasklistID *extension,
                              const gchar *title)
{
	GObject *object;

	g_return_if_fail (E_IS_SOURCE_TASKLIST_ID (extension));

	g_mutex_lock (&extension->priv->property_lock);

	if (extension->priv->title &&
	    g_strcmp0 (extension->priv->title, title) == 0) {
		g_mutex_unlock (&extension->priv->property_lock);
		return;
	}

	g_free (extension->priv->title);
	extension->priv->title = e_util_strdup_strip (title);

	if (extension->priv->title == NULL)
		extension->priv->title = g_strdup ("none");

	g_mutex_unlock (&extension->priv->property_lock);

	object = G_OBJECT (extension);
	g_object_freeze_notify (object);
	g_object_notify (object, "id");
	g_object_notify (object, "title");
	g_object_thaw_notify (object);
}
