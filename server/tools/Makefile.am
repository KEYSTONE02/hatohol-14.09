SUBDIRS = tls

bin_PROGRAMS = hatohol-def-src-file-generator
dist_bin_SCRIPTS = hatohol-voyager \
                   hatohol-db-initiator \
                   hatohol-inspect-info-collector \
                   hatohol-server-type-util

actiondir = $(libexecdir)/$(PACKAGE)/action
dist_action_SCRIPTS = hatohol-actor-mail

MLPL_CFLAGS = -I ../mlpl/src
MLPL_LIBS = ../mlpl/src/libmlpl.la

AM_CXXFLAGS = \
	$(OPT_CXXFLAGS) \
	-I ../src -I ../common $(MLPL_CFLAGS) $(GLIB_CFLAGS) $(MYSQL_CFLAGS) $(LIBSOUP_CFLAGS) $(JSON_GLIB_CFLAGS)

AM_LDFLAGS = ../src/libhatohol.la ../common/libhatohol-common.la $(MLPL_LIBS) $(GLIB_LIBS) $(MYSQL_LIBS)

hatohol_def_src_file_generator_SOURCES = hatohol-def-src-file-generator.cc

# To make hahol_def.py on the build.
noinst_SCRIPTS = \
  hatohol/__init__.py \
  hatohol/hatohol_def.py \
  hatohol/voyager.py \
  hatohol/ActionCreator.py \
  setup.py \
  hatohol-init-user-generator

hatohol/hatohol_def.py: hatohol-def-src-file-generator
	./hatohol-def-src-file-generator py > $@

install-data-hook:
	python setup.py install --root=$(DESTDIR)

EXTRA_DIST = $(noinst_SCRIPTS)

