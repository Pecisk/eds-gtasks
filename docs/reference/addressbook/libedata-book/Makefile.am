# The name of the module.
DOC_MODULE = libedata-book

# The top-level SGML file.
DOC_MAIN_SGML_FILE = $(DOC_MODULE)-docs.sgml

# Extra options to supply to gtkdoc-scan
SCAN_OPTIONS = --deprecated-guards="EDS_DISABLE_DEPRECATED"

# Extra options to supply to gtkdoc-mkdb
MKDB_OPTIONS = --sgml-mode --output-format=xml --name-space=e

# The directory containing the source code. Relative to $(srcdir).
DOC_SOURCE_DIR = $(top_srcdir)/addressbook/libedata-book

# Used for dependencies. The docs will be rebuilt if any of these change.
HFILE_GLOB = $(top_srcdir)/addressbook/libedata-book/*.h
CFILE_GLOB = $(top_srcdir)/addressbook/libedata-book/*.c

IGNORE_HFILES = \
	e-data-book-glue.h				\
	e-data-book-factory-glue.h			\
	e-data-book-view-glue.h				\
	e-data-book-types.h				\
	opid.h

GTKDOC_CFLAGS = \
	-I$(top_srcdir) 				\
	-I$(top_srcdir)/addressbook			\
	-I$(top_builddir)				\
	-I$(top_builddir)/addressbook			\
	$(EVOLUTION_ADDRESSBOOK_CFLAGS)			\
	$(CAMEL_CFLAGS)

GTKDOC_LIBS = \
	$(top_builddir)/addressbook/libedata-book/libedata-book-1.2.la	\
	$(top_builddir)/addressbook/libebook/libebook-1.2.la		\
	$(top_builddir)/libebackend/libebackend-1.2.la			\
	$(top_builddir)/libedataserver/libedataserver-1.2.la		\
	$(EVOLUTION_ADDRESSBOOK_LIBS)					\
	$(CAMEL_LIBS)

# This includes the standard gtk-doc make rules, copied by gtkdocize.
include $(top_srcdir)/gtk-doc.make

#TESTS = $(GTKDOC_CHECK)

-include $(top_srcdir)/git.mk
