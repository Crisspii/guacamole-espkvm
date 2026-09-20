AUTOMAKE_OPTIONS = foreign

ACLOCAL_AMFLAGS = -I m4

AM_CPPFLAGS = $(CURL_CFLAGS) $(JSONC_CFLAGS)
AM_CFLAGS = -Werror -Wall -pedantic

lib_LTLIBRARIES = libguac-client-espkvm.la

# All source files of libguac-client-espkvm
libguac_client_espkvm_la_SOURCES = \
    src/espkvm.c

# Libraries required by ESPKVM protocol client
libguac_client_espkvm_la_LIBADD = \
    $(CURL_LIBS) \
    $(JSONC_LIBS)

# libtool versioning information
libguac_client_espkvm_la_LDFLAGS = \
    -version-info 0:0:0
