## Copyright (C) 1996-2025 The Squid Software Foundation and contributors
##
## Squid software is distributed under GPLv2+ license and includes
## contributions from numerous individuals and organizations.
## Please see the COPYING and CONTRIBUTORS files for details.
##

include $(top_srcdir)/src/Common.am

libexec_PROGRAMS = basic_sspi_auth

basic_sspi_auth_SOURCES = \
	basic_sspi_auth.cc \
	valid.cc \
	valid.h
basic_sspi_auth_CXXFLAGS = \
	$(AM_CXXFLAGS) \
	-Wl,--enable-auto-import
basic_sspi_auth_LDADD = \
	$(top_builddir)/lib/sspi/libsspwin32.la \
	$(top_builddir)/lib/libmiscencoding.la \
	$(COMPAT_LIB) \
	-lnetapi32 -ladvapi32 \
	$(LIBNETTLE_LIBS) \
	$(XTRA_LIBS)

man_MANS = basic_sspi_auth.8
EXTRA_DIST = basic_sspi_auth.8 required.m4
