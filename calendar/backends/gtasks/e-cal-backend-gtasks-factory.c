/*
 * Evolution calendar - google tasks backend factory
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
 *		Peteris Krisjanis <pecisk@gmail.com>
 *
 * Copyright (C) 2013 Peteris Krisjanis
 *
 */

#include <config.h>

#include "e-cal-backend-gtasks.h"

#define FACTORY_NAME "gtasks"

typedef ECalBackendFactory ECalBackendGTasksTodosFactory;
typedef ECalBackendFactoryClass ECalBackendGTasksTodosFactoryClass;

/* Module Entry Points */
void e_module_load (GTypeModule *type_module);
void e_module_unload (GTypeModule *type_module);

/* Forward Declarations */
GType e_cal_backend_gtasks_todos_factory_get_type (void);

G_DEFINE_DYNAMIC_TYPE (
	ECalBackendGTasksTodosFactory,
	e_cal_backend_gtasks_todos_factory,
	E_TYPE_CAL_BACKEND_FACTORY)

static void
e_cal_backend_gtasks_todos_factory_class_init (ECalBackendFactoryClass *class)
{
	class->factory_name = FACTORY_NAME;
	class->component_kind = ICAL_VTODO_COMPONENT;
	class->backend_type = E_TYPE_CAL_BACKEND_GTASKS;
}

static void
e_cal_backend_gtasks_todos_factory_class_finalize (ECalBackendFactoryClass *class)
{
}

static void
e_cal_backend_gtasks_todos_factory_init (ECalBackendFactory *factory)
{
}

G_MODULE_EXPORT void
e_module_load (GTypeModule *type_module)
{
	e_cal_backend_gtasks_todos_factory_register_type (type_module);
}

G_MODULE_EXPORT void
e_module_unload (GTypeModule *type_module)
{
}

