/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

#include <libebook/libebook.h>

#include "ebook-test-utils.h"
#include "e-test-server-utils.h"

static ETestServerClosure book_closure =
	{ E_TEST_SERVER_DEPRECATED_ADDRESS_BOOK, NULL, 0 };

static void
test_remove_contacts_sync (ETestServerFixture *fixture,
                           gconstpointer user_data)
{
	EBook *book;
	EContact *contact_final = NULL;
	gchar *uid_1, *uid_2;
	GList *uids = NULL;

	book = E_TEST_SERVER_UTILS_SERVICE (fixture, EBook);

	uid_1 = ebook_test_utils_book_add_contact_from_test_case_verify (book, "simple-1", NULL);
	uid_2 = ebook_test_utils_book_add_contact_from_test_case_verify (book, "simple-2", NULL);
	uids = g_list_prepend (uids, uid_1);
	uids = g_list_prepend (uids, uid_2);
	ebook_test_utils_book_remove_contacts (book, uids);

	contact_final = NULL;
	e_book_get_contact (book, uid_1, &contact_final, NULL);
	g_assert (contact_final == NULL);

	e_book_get_contact (book, uid_2, &contact_final, NULL);
	g_assert (contact_final == NULL);

	test_print ("successfully added and removed contacts\n");

	g_free (uid_1);
	g_free (uid_2);
	g_list_free (uids);
}

static void
test_remove_contacts_async (ETestServerFixture *fixture,
                            gconstpointer user_data)
{
	EBook *book;
	gchar *uid_1, *uid_2;
	GList *uids = NULL;

	book = E_TEST_SERVER_UTILS_SERVICE (fixture, EBook);

	uid_1 = ebook_test_utils_book_add_contact_from_test_case_verify (book, "simple-1", NULL);
	uid_2 = ebook_test_utils_book_add_contact_from_test_case_verify (book, "simple-2", NULL);
	uids = NULL;
	uids = g_list_prepend (uids, uid_1);
	uids = g_list_prepend (uids, uid_2);

	ebook_test_utils_book_async_remove_contacts (
		book, uids,
			ebook_test_utils_callback_quit, fixture->loop);

	g_main_loop_run (fixture->loop);

	g_free (uid_1);
	g_free (uid_2);
	g_list_free (uids);
}

gint
main (gint argc,
      gchar **argv)
{
#if !GLIB_CHECK_VERSION (2, 35, 1)
	g_type_init ();
#endif
	g_test_init (&argc, &argv, NULL);
	g_test_bug_base ("http://bugzilla.gnome.org/");

	g_test_add (
		"/EBook/RemoveContacts/Sync",
		ETestServerFixture,
		&book_closure,
		e_test_server_utils_setup,
		test_remove_contacts_sync,
		e_test_server_utils_teardown);
	g_test_add (
		"/EBook/RemoveContacts/Async",
		ETestServerFixture,
		&book_closure,
		e_test_server_utils_setup,
		test_remove_contacts_async,
		e_test_server_utils_teardown);

	return e_test_server_utils_run ();
}
