# SPDX-License-Identifier: GPL-2.0-only

EXTRA_DIST = common.hpp

noinst_PROGRAMS = validate_xml extract_xml pretty_xml
validate_xml_SOURCES = validate_xml.cpp
validate_xml_CPPFLAGS = $(libxml2_CFLAGS) $(AM_CPPFLAGS)
validate_xml_LDADD = $(libxml2_LIBS)

extract_xml_SOURCES = extract_xml.cpp
extract_xml_CPPFLAGS = $(libxml2_CFLAGS) $(AM_CPPFLAGS)
extract_xml_LDADD = $(libxml2_LIBS)

pretty_xml_SOURCES = pretty_xml.cpp
pretty_xml_CPPFLAGS = $(libxml2_CFLAGS) $(AM_CPPFLAGS)
pretty_xml_LDADD = $(libxml2_LIBS)

all-local:
	@if [ x"$(srcdir)" != x"$(builddir)" ]; then \
		for script in $(EXTRA_DIST); do \
			cp -f $(srcdir)/$$script $(builddir); \
		done; \
	fi

clean-local:
	@if [ x"$(srcdir)" != x"$(builddir)" ]; then \
		for script in $(EXTRA_DIST); do \
			rm -f $(builddir)/$$script; \
		done; \
	fi
