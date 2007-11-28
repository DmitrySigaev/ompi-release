# -*- shell-script -*-
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
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#


# OMPI_CHECK_SCTP(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
# check if SCTP support can be found.  sets prefix_{CPPFLAGS,
# LDFLAGS, LIBS} as needed and runs action-if-found if there is
# support, otherwise executes action-if-not-found
AC_DEFUN([OMPI_CHECK_SCTP],[
    AC_ARG_WITH([sctp],
	[AC_HELP_STRING([--with-sctp(=DIR)],
		[Build SCTP support, searching for libraries in DIR])])
    AC_ARG_WITH([sctp-libdir],
	[AC_HELP_STRING([--with-sctp-libdir=DIR],
		[Search for SCTP libraries in DIR])])
    
    btl_sctp_CFLAGS="`echo $CFLAGS`"
#only try to build this on Linux, Mac OS X, or some BSD variant    
    ompi_sctp_try_to_build="no"
    case "$host" in
    *linux*)
	ompi_sctp_try_to_build="yes"
	;;
    *bsd*)
        # only add -DFREEBSD once to get extra sin_len field
        btl_sctp_CFLAGS="`echo $btl_sctp_CFLAGS | sed 's/-DFREEBSD//g'`"
        btl_sctp_CFLAGS="$btl_sctp_CFLAGS -DFREEBSD"
	ompi_sctp_try_to_build="yes"
	AC_MSG_WARN([Adding -DFREEBSD to set extra sin_len field in sockaddr.])
	;;
# Mac OS X support for SCTP NKE. Adjustments should look like *bsd*...
    *darwin*)
        # only add -DFREEBSD once to get extra sin_len field
        btl_sctp_CFLAGS="`echo $btl_sctp_CFLAGS | sed 's/-DFREEBSD//g'`"
        btl_sctp_CFLAGS="$btl_sctp_CFLAGS -DFREEBSD"
	ompi_sctp_try_to_build="yes"
	AC_MSG_WARN([Adding -DFREEBSD to set extra sin_len field in sockaddr.])
	;;
    *)
	AC_MSG_WARN([Only build sctp BTL on Linux, Mac OS X, and BSD variants])
	;;
    esac

    AS_IF([test "$with_sctp" != "no" -a "$ompi_sctp_try_to_build" = "yes"],
	[AS_IF([test ! -z "$with_sctp" -a "$with_sctp" != "yes"],
		[ompi_check_sctp_dir="$with_sctp"])
	    AS_IF([test ! -z "$with_sctp_libdir" -a "$with_sctp_libdir" != "yes"],
		[ompi_check_sctp_libdir="$with_sctp_libdir"])

# TODO how to structure this if some OS's have the SCTP API calls in libc and some
#   in libsctp ?  For now, assume libsctp and have user make softlink to libc if 
#   libsctp does not exist...
	    OMPI_CHECK_PACKAGE([$1],
		[netinet/sctp.h],
		[sctp],
		[sctp_recvmsg],
		[],
		[$ompi_check_sctp_dir],
		[$ompi_check_sctp_libdir],
		[ompi_check_sctp_happy="yes"],
		[ompi_check_sctp_happy="no"])
	    ],
	[ompi_check_sctp_happy="no"])

    AS_IF([test "$ompi_check_sctp_happy" = "yes"],
	[$2],
	[AS_IF([test ! -z "$with_sctp" -a "$with_sctp" != "no"],
		[AC_MSG_ERROR([SCTP support requested but not found.  Aborting])])
	    $3])
    ])


# MCA_btl_sctp_CONFIG([action-if-found], [action-if-not-found])
# -----------------------------------------------------------
AC_DEFUN([MCA_btl_sctp_CONFIG],[
    OMPI_CHECK_SCTP([btl_sctp],
                   [btl_sctp_happy="yes"],
                   [btl_sctp_happy="no"])

    AS_IF([test "$btl_sctp_happy" = "yes"],
          [btl_sctp_WRAPPER_EXTRA_LDFLAGS="$btl_sctp_LDFLAGS"
           btl_sctp_WRAPPER_EXTRA_LIBS="$btl_sctp_LIBS"
           btl_sctp_WRAPPER_EXTRA_CPPFLAGS="$btl_sctp_CPPFLAGS"
           btl_sctp_WRAPPER_EXTRA_CFLAGS="$btl_sctp_CFLAGS"
           $1],
          [$2])


    # substitute in the things needed to build sctp
    AC_SUBST([btl_sctp_CFLAGS])
    AC_SUBST([btl_sctp_CPPFLAGS])
    AC_SUBST([btl_sctp_LDFLAGS])
    AC_SUBST([btl_sctp_LIBS])
])dnl