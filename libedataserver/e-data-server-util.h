/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Authors: Rodrigo Moya <rodrigo@ximian.com>
 */

#if !defined (__LIBEDATASERVER_H_INSIDE__) && !defined (LIBEDATASERVER_COMPILATION)
#error "Only <libedataserver/libedataserver.h> should be included directly."
#endif

#ifndef E_DATA_SERVER_UTIL_H
#define E_DATA_SERVER_UTIL_H

#include <sys/types.h>
#include <gio/gio.h>

G_BEGIN_DECLS

struct tm;

const gchar *	e_get_user_cache_dir		(void);
const gchar *	e_get_user_config_dir		(void);
const gchar *	e_get_user_data_dir		(void);

gchar *		e_util_strdup_strip		(const gchar *string);
gchar *		e_util_strstrcase		(const gchar *haystack,
						 const gchar *needle);
gchar *		e_util_unicode_get_utf8		(const gchar *text,
						 gunichar *out);
const gchar *	e_util_utf8_strstrcase		(const gchar *haystack,
						 const gchar *needle);
const gchar *	e_util_utf8_strstrcasedecomp	(const gchar *haystack,
						 const gchar *needle);
gint		e_util_utf8_strcasecmp		(const gchar *s1,
						 const gchar *s2);
gchar *		e_util_utf8_remove_accents	(const gchar *str);
gchar *		e_util_utf8_make_valid		(const gchar *str);
gchar *		e_util_utf8_data_make_valid	(const gchar *data,
						 gsize data_bytes);
gchar *         e_util_utf8_normalize           (const gchar *str);
const gchar *   e_util_ensure_gdbus_string	(const gchar *str,
						 gchar **gdbus_str);
guint64		e_util_gthread_id		(GThread *thread);
void		e_filename_make_safe		(gchar *string);
gchar *		e_filename_mkdir_encoded	(const gchar *basepath,
						 const gchar *fileprefix,
						 const gchar *filename,
						 gint fileindex);

gsize		e_utf8_strftime			(gchar *string,
						 gsize max,
						 const gchar *fmt,
						 const struct tm *tm);
gsize		e_strftime			(gchar *string,
						 gsize max,
						 const gchar *fmt,
						 const struct tm *tm);

gchar **	e_util_slist_to_strv		(const GSList *strings);
GSList *	e_util_strv_to_slist		(const gchar * const *strv);
void		e_util_free_nullable_object_slist
						(GSList *objects);
void		e_queue_transfer		(GQueue *src_queue,
						 GQueue *dst_queue);
GWeakRef *	e_weak_ref_new			(gpointer object);
void		e_weak_ref_free			(GWeakRef *weak_ref);

gboolean	e_file_recursive_delete_sync	(GFile *file,
						 GCancellable *cancellable,
						 GError **error);
void		e_file_recursive_delete		(GFile *file,
						 gint io_priority,
						 GCancellable *cancellable,
						 GAsyncReadyCallback callback,
						 gpointer user_data);
gboolean	e_file_recursive_delete_finish	(GFile *file,
						 GAsyncResult *result,
						 GError **error);

/* Useful GBinding transform functions */
gboolean	e_binding_transform_enum_value_to_nick
						(GBinding *binding,
						 const GValue *source_value,
						 GValue *target_value,
						 gpointer not_used);
gboolean	e_binding_transform_enum_nick_to_value
						(GBinding *binding,
						 const GValue *source_value,
						 GValue *target_value,
						 gpointer not_used);

gboolean	e_enum_from_string		(GType enum_type,
						 const gchar *string,
						 gint *enum_value);
const gchar *	e_enum_to_string		(GType enum_type,
						 gint enum_value);

typedef struct _EAsyncClosure EAsyncClosure;

EAsyncClosure *	e_async_closure_new		(void);
GAsyncResult *	e_async_closure_wait		(EAsyncClosure *closure);
void		e_async_closure_free		(EAsyncClosure *closure);
void		e_async_closure_callback	(GObject *object,
						 GAsyncResult *result,
						 gpointer closure);

#ifdef G_OS_WIN32
const gchar *	e_util_get_prefix		(void) G_GNUC_CONST;
const gchar *	e_util_get_cp_prefix		(void) G_GNUC_CONST;
const gchar *	e_util_get_localedir		(void) G_GNUC_CONST;
gchar *		e_util_replace_prefix		(const gchar *configure_time_prefix,
						 const gchar *runtime_prefix,
						 const gchar *configure_time_path);
#endif

/* utility functions for easier processing of named parameters */

/**
 * ENamedParameters:
 *
 * Since: 3.8
 **/
struct _ENamedParameters;
typedef struct _ENamedParameters ENamedParameters;

GType           e_named_parameters_get_type     (void) G_GNUC_CONST;
ENamedParameters *
		e_named_parameters_new		(void);
ENamedParameters *
		e_named_parameters_new_strv	(const gchar * const *strv);
void		e_named_parameters_free		(ENamedParameters *parameters);
void		e_named_parameters_clear	(ENamedParameters *parameters);
void		e_named_parameters_assign	(ENamedParameters *parameters,
						 const ENamedParameters *from);
void		e_named_parameters_set		(ENamedParameters *parameters,
						 const gchar *name,
						 const gchar *value);
const gchar *	e_named_parameters_get		(const ENamedParameters *parameters,
						 const gchar *name);
gchar **	e_named_parameters_to_strv	(const ENamedParameters *parameters);
gboolean	e_named_parameters_test		(const ENamedParameters *parameters,
						 const gchar *name,
						 const gchar *value,
						 gboolean case_sensitively);

#ifndef EDS_DISABLE_DEPRECATED
void		e_util_free_string_slist	(GSList *strings);
void		e_util_free_object_slist	(GSList *objects);
GSList *	e_util_copy_string_slist	(GSList *copy_to,
						 const GSList *strings);
GSList *	e_util_copy_object_slist	(GSList *copy_to,
						 const GSList *objects);
gint		e_data_server_util_get_dbus_call_timeout
						(void);
void		e_data_server_util_set_dbus_call_timeout
						(gint timeout_msec);

#endif /* EDS_DISABLE_DEPRECATED */

G_END_DECLS

#endif /* E_DATA_SERVER_UTIL_H */
