#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2010      Los Alamos National Security, LLC.
#                         All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# main library setup
noinst_LTLIBRARIES = libmca_shmem.la
libmca_shmem_la_SOURCES =

# header setup
nobase_opal_HEADERS =
nobase_nodist_opal_HEADERS =

# local files
headers = shmem.h shmem_types.h 
nodist_headers = 
libmca_shmem_la_SOURCES += $(headers)

# Conditionally install the header files
if WANT_INSTALL_HEADERS
nobase_opal_HEADERS += $(headers)
nobase_nodist_opal_HEADERS += $(nodist_headers)
opaldir = $(includedir)/openmpi/opal/mca/shmem
else
opaldir = $(includedir)
endif

include base/Makefile.am

distclean-local:
	rm -f base/static-components.h
