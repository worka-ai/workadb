#-------------------------------------------------------------------------
#
# Makefile for src/embed
#
# src/embed/Makefile
#
#-------------------------------------------------------------------------

subdir = src/embed
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

override CPPFLAGS += -I$(top_srcdir)/src -I$(top_srcdir)/src/include -I$(top_srcdir)/src/common -I$(top_srcdir)/src/interfaces/libpq -I$(top_srcdir)/src/timezone -I$(top_builddir)/src/port

OBJS = workadb.o workadb_assets_manifest.o workadb_assets_blob.o initdb_embed.o main_embed.o initdb_shims.o psprintf_shim.o pqexpbuffer.o

TOP_BUILD_ABS := $(abspath $(top_builddir))
BACKEND_OBJFILES = $(shell find $(TOP_BUILD_ABS)/src/backend -name objfiles.txt ! -path "*/main/objfiles.txt" -print) \
	$(TOP_BUILD_ABS)/src/timezone/objfiles.txt
BACKEND_OBJS_LIST = $(TOP_BUILD_ABS)/src/embed/workadb_backend_objs.txt
BACKEND_TMPDIR = $(TOP_BUILD_ABS)/src/embed/workadb_archive_tmp
BACKEND_EXTRACT_DIR = $(BACKEND_TMPDIR)/extract
LIBPGCOMMON_SRV = $(TOP_BUILD_ABS)/src/common/libpgcommon_srv.a
LIBPGPORT_SRV = $(TOP_BUILD_ABS)/src/port/libpgport_srv.a
LIBPGCOMMON = $(TOP_BUILD_ABS)/src/common/libpgcommon.a

all: libworkadb.a

$(BACKEND_OBJS_LIST): $(BACKEND_OBJFILES)
	@cat $(BACKEND_OBJFILES) | tr ' ' '\n' | sed '/^$$/d' | sed 's#^#$(TOP_BUILD_ABS)/#' | sort -u > $@

libworkadb.a: $(OBJS) $(BACKEND_OBJS_LIST) $(LIBPGCOMMON_SRV) $(LIBPGPORT_SRV) $(LIBPGCOMMON)
	rm -f $@
	rm -rf $(BACKEND_TMPDIR)
	mkdir -p $(BACKEND_EXTRACT_DIR)
	(cd $(BACKEND_EXTRACT_DIR) && $(AR) -x $(LIBPGCOMMON_SRV))
	(cd $(BACKEND_EXTRACT_DIR) && $(AR) -x $(LIBPGPORT_SRV))
	(cd $(BACKEND_EXTRACT_DIR) && $(AR) -x $(LIBPGCOMMON) logging.o)
	rm -f $(BACKEND_EXTRACT_DIR)/psprintf.o $(BACKEND_EXTRACT_DIR)/psprintf_srv.o
	set -e; \
	idx=0; \
	while read -r obj; do \
		if [ ! -f "$$obj" ]; then \
			echo "missing object: $$obj" >&2; \
			exit 1; \
		fi; \
		idx=$$((idx + 1)); \
		ln -f "$$obj" "$(BACKEND_TMPDIR)/obj_$$idx.o"; \
		echo "obj_$$idx.o $$obj" >> "$(BACKEND_TMPDIR)/map.txt"; \
	done < $(BACKEND_OBJS_LIST); \
	find $(BACKEND_EXTRACT_DIR) -name '*.o' -print | while read -r obj; do \
		idx=$$((idx + 1)); \
		ln -f "$$obj" "$(BACKEND_TMPDIR)/obj_$$idx.o"; \
		echo "obj_$$idx.o $$obj" >> "$(BACKEND_TMPDIR)/map.txt"; \
	done; \
	expected=$$(wc -l < $(BACKEND_OBJS_LIST) | tr -d ' '); \
	extracted=$$(find $(BACKEND_EXTRACT_DIR) -name '*.o' -print | wc -l | tr -d ' '); \
	actual=$$(find $(BACKEND_TMPDIR) -name 'obj_*.o' -print | wc -l | tr -d ' '); \
	if [ "$$actual" -ne $$((expected + extracted)) ]; then \
		echo "object count mismatch: expected $$((expected + extracted)), got $$actual" >&2; \
		exit 1; \
	fi
	$(AR) $(AROPT) $@ $(OBJS)
	find $(BACKEND_TMPDIR) -name 'obj_*.o' -print | xargs -n 200 $(AR) $(AROPT) $@
	cp "$(BACKEND_TMPDIR)/map.txt" "$(TOP_BUILD_ABS)/src/embed/workadb_object_map.txt"
	rm -rf $(BACKEND_TMPDIR)

pqexpbuffer.o: $(top_srcdir)/src/interfaces/libpq/pqexpbuffer.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DFRONTEND -c $< -o $@

install: installdirs
	$(INSTALL_DATA) libworkadb.a '$(DESTDIR)$(libdir)'
	$(INSTALL_DATA) $(top_srcdir)/src/include/workadb.h '$(DESTDIR)$(includedir)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(libdir)'
	$(MKDIR_P) '$(DESTDIR)$(includedir)'

clean distclean:
	rm -f $(OBJS) libworkadb.a

.PHONY: all install installdirs clean distclean
