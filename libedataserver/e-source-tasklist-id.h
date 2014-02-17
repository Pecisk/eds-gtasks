/*
 * e-source-security.h
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

#if !defined (__LIBEDATASERVER_H_INSIDE__) && !defined (LIBEDATASERVER_COMPILATION)
#error "Only <libedataserver/libedataserver.h> should be included directly."
#endif

#ifndef E_SOURCE_TASKLIST_ID_H
#define E_SOURCE_TASKLIST_ID_H

#include <libedataserver/e-source-extension.h>

/* Standard GObject macros */
#define E_TYPE_SOURCE_TASKLIST_ID \
	(e_source_tasklist_id_get_type ())
#define E_SOURCE_TASKLIST_ID(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST \
	((obj), E_TYPE_SOURCE_TASKLIST_ID, ESourceTasklistID))
#define E_SOURCE_TASKLIST_ID_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_CAST \
	((cls), E_TYPE_SOURCE_TASKLIST_ID, ESourceTasklistIDClass))
#define E_IS_SOURCE_TASKLIST_ID(obj) \
	(G_TYPE_CHECK_INSTANCE_TYPE \
	((obj), E_TYPE_SOURCE_TASKLIST_ID))
#define E_IS_SOURCE_TASKLIST_ID_CLASS(cls) \
	(G_TYPE_CHECK_CLASS_TYPE \
	((cls), E_TYPE_SOURCE_TASKLIST_ID))
#define E_SOURCE_TASKLIST_ID_GET_CLASS(obj) \
	(G_TYPE_INSTANCE_GET_CLASS \
	((obj), E_TYPE_SOURCE_TASKLIST_ID, ESourceTasklistIDClass))

/**
 * E_SOURCE_EXTENSION_TASKLIST_ID:
 *
 * Pass this extension name to e_source_get_extension() to access
 * #ESourceTasklistID.  This is also used as a group name in key files.
 *
 * Since: 3.6
 **/
#define E_SOURCE_EXTENSION_TASKLIST_ID "TasklistID"

G_BEGIN_DECLS

typedef struct _ESourceTasklistID ESourceTasklistID;
typedef struct _ESourceTasklistIDClass ESourceTasklistIDClass;
typedef struct _ESourceTasklistIDPrivate ESourceTasklistIDPrivate;

/**
 * ESourceSecurity:
 *
 * Contains only private data that should be read and manipulated using the
 * functions below.
 *
 * Since: 3.6
 **/
struct _ESourceTasklistID {
	ESourceExtension parent;
	ESourceTasklistIDPrivate *priv;
};

struct _ESourceTasklistIDClass {
	ESourceExtensionClass parent_class;
};

GType		e_source_tasklist_id_get_type	(void) G_GNUC_CONST;
const gchar *	e_source_tasklist_id_get_id	(ESourceTasklistID *extension);
gchar *		e_source_tasklist_id_dup_id	(ESourceTasklistID *extension);
void		e_source_tasklist_id_set_id	(ESourceTasklistID *extension,
						 const gchar *id);
const gchar *	e_source_tasklist_id_get_title	(ESourceTasklistID *extension);
gchar *		e_source_tasklist_id_dup_title	(ESourceTasklistID *extension);
void		e_source_tasklist_id_set_title	(ESourceTasklistID *extension,
						 const gchar *title);

G_END_DECLS

#endif /* E_SOURCE_TASKLIST_ID_H */
