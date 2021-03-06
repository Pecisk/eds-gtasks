/* Evolution calendar - weather backend source class
 *
 * Copyright (C) 1999-2008 Novell, Inc. (www.novell.com)
 *
 * Authors: David Trowbridge <trowbrds@cs.colorado.edu>
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

#include "e-weather-source.h"

#include <string.h>

#define E_WEATHER_SOURCE_GET_PRIVATE(obj) \
	(G_TYPE_INSTANCE_GET_PRIVATE \
	((obj), E_TYPE_WEATHER_SOURCE, EWeatherSourcePrivate))

struct _EWeatherSourcePrivate {
	GWeatherLocation *location;
	GWeatherInfo *info;

	EWeatherSourceFinished done;
	gpointer finished_data;
};

G_DEFINE_TYPE (EWeatherSource, e_weather_source, G_TYPE_OBJECT)

static void
weather_source_dispose (GObject *object)
{
	EWeatherSourcePrivate *priv;

	priv = E_WEATHER_SOURCE_GET_PRIVATE (object);

	if (priv->location != NULL) {
		gweather_location_unref (priv->location);
		priv->location = NULL;
	}

	g_clear_object (&priv->info);

	/* Chain up to parent's dispose() method. */
	G_OBJECT_CLASS (e_weather_source_parent_class)->dispose (object);
}

static void
e_weather_source_class_init (EWeatherSourceClass *class)
{
	GObjectClass *object_class;

	g_type_class_add_private (class, sizeof (EWeatherSourcePrivate));

	object_class = G_OBJECT_CLASS (class);
	object_class->dispose = weather_source_dispose;
}

static void
e_weather_source_init (EWeatherSource *source)
{
	source->priv = E_WEATHER_SOURCE_GET_PRIVATE (source);
}

EWeatherSource *
e_weather_source_new (const gchar *location)
{
	GWeatherLocation *world, *glocation;
	EWeatherSource *source;
	gchar **tokens;

	/* Old location is formatted as ccf/AAA[/BBB] - AAA is the 3-letter
	 * station code for identifying the providing station (subdirectory
	 * within the crh data repository).  BBB is an optional additional
	 * station ID for the station within the CCF file. If not present,
	 * BBB is assumed to be the same station as AAA.
	 *
	 * But the new location is code/name, where code is 4-letter code.
	 * So if we got the old format, then migrate to the new one if
	 * possible. */

	if (location == NULL)
		return NULL;

	world = gweather_location_new_world (FALSE);

	if (strncmp (location, "ccf/", 4) == 0)
		location += 4;

	tokens = g_strsplit (location, "/", 2);

	glocation = gweather_location_find_by_station_code (world, tokens[0]);
	if (glocation != NULL)
		gweather_location_ref (glocation);

	gweather_location_unref (world);
	g_strfreev (tokens);

	if (glocation == NULL)
		return NULL;

	source = g_object_new (E_TYPE_WEATHER_SOURCE, NULL);
	source->priv->location = gweather_location_ref (glocation);

	return source;
}

static void
weather_source_updated_cb (GWeatherInfo *info,
                           EWeatherSource *source)
{
	g_return_if_fail (E_IS_WEATHER_SOURCE (source));
	g_return_if_fail (source->priv->done != NULL);

	/* An invalid GWeatherInfo is as good as NULL. */
	if (info != NULL && !gweather_info_is_valid (info))
		info = NULL;

	source->priv->done (info, source->priv->finished_data);
}

void
e_weather_source_parse (EWeatherSource *source,
                        EWeatherSourceFinished done,
                        gpointer data)
{
	g_return_if_fail (E_IS_WEATHER_SOURCE (source));
	g_return_if_fail (done != NULL);

	/* FIXME Take a GAsyncReadyCallback instead of a custom callback,
	 *       and write an e_weather_source_parse_finish() function. */

	source->priv->finished_data = data;
	source->priv->done = done;

	if (source->priv->info == NULL) {
		source->priv->info = gweather_info_new (
			source->priv->location,
			GWEATHER_FORECAST_LIST);
		g_signal_connect (
			source->priv->info, "updated",
			G_CALLBACK (weather_source_updated_cb), source);
	} else {
		gweather_info_update (source->priv->info);
	}
}

