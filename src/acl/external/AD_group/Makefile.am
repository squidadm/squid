## Copyright (C) 1996-2025 The Squid Software Foundation and contributors
##
## Squid software is distributed under GPLv2+ license and includes
## contributions from numerous individuals and organizations.
## Please see the COPYING and CONTRIBUTORS files for details.
##

include $(top_srcdir)/src/Common.am

libexec_PROGRAMS = ext_ad_group_acl
man_MANS = ext_ad_group_acl.8

ext_ad_group_acl_SOURCES = ext_ad_group_acl.cc

# TODO: test for these libraries in required.m4
ext_ad_group_acl_LDADD = \
	$(top_builddir)/lib/libmiscencoding.la \
	$(COMPAT_LIB) \
	-lnetapi32 \
	-ladvapi32 \
	-lole32 \
	-loleaut32 \
	-lntdll \
	-lactiveds \
	-ladsiid \
	$(XTRA_LIBS)

EXTRA_DIST = required.m4 ext_ad_group_acl.8
