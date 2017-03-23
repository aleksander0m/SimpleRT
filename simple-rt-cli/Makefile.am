

bin_PROGRAMS = g-simple-rt
dist_bin_SCRIPTS = g-simple-rt-iface-up.sh

g_simple_rt_CPPFLAGS = \
	-I${top_srcdir} \
	-I${top_builddir} \
	$(GLIB_CFLAGS) \
	$(GUDEV_CFLAGS) \
	$(LIBUSB_CFLAGS) \
	-DBINDIR_PATH=\""$(bindir)"\" \
	$(NULL)

g_simple_rt_SOURCES = \
	g-simple-rt.c \
	$(NULL)

g_simple_rt_LDADD = \
	$(LIBUSB_LIBS) \
	$(GUDEV_LIBS) \
	$(GLIB_LIBS) \
	$(NULL)
