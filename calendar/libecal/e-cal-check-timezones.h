/*
 * Copyright (C) 2008 Novell, Inc.
 *
 * Authors: Patrick Ohly <patrick.ohly@gmx.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#if !defined (__LIBECAL_H_INSIDE__) && !defined (LIBECAL_COMPILATION)
#error "Only <libecal/libecal.h> should be included directly."
#endif

#ifndef E_CAL_CHECK_TIMEZONES_H
#define E_CAL_CHECK_TIMEZONES_H

#include <libical/ical.h>
#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gboolean	e_cal_client_check_timezones	(icalcomponent *comp,
						 GList *comps,
						 icaltimezone *(*tzlookup) (const gchar *tzid, gconstpointer ecalclient, GCancellable *cancellable, GError **error),
						 gconstpointer ecalclient,
						 GCancellable *cancellable,
						 GError **error);

icaltimezone *	e_cal_client_tzlookup		(const gchar *tzid,
						 gconstpointer ecalclient,
						 GCancellable *cancellable,
						 GError **error);

icaltimezone *	e_cal_client_tzlookup_icomp
						(const gchar *tzid,
						 gconstpointer custom,
						 GCancellable *cancellable,
						 GError **error);

const gchar *	e_cal_match_tzid		(const gchar *tzid);

#ifndef EDS_DISABLE_DEPRECATED

gboolean	e_cal_check_timezones		(icalcomponent *comp,
						 GList *comps,
						 icaltimezone * (*tzlookup) (const gchar *tzid, gconstpointer custom, GError **error),
						 gconstpointer custom,
						 GError **error);

icaltimezone *	e_cal_tzlookup_ecal		(const gchar *tzid,
						 gconstpointer custom,
						 GError **error);

icaltimezone *	e_cal_tzlookup_icomp		(const gchar *tzid,
						 gconstpointer custom,
						 GError **error);

#endif /* EDS_DISABLE_DEPRECATED */

G_END_DECLS

#endif /* E_CAL_CHECK_TIMEZONES_H */
