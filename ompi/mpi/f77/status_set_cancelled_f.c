/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#include "ompi_config.h"

#include "ompi/mpi/f77/bindings.h"
#include "ompi/mpi/f77/constants.h"

#if OPAL_HAVE_WEAK_SYMBOLS && OMPI_PROFILE_LAYER
#pragma weak PMPI_STATUS_SET_CANCELLED = mpi_status_set_cancelled_f
#pragma weak pmpi_status_set_cancelled = mpi_status_set_cancelled_f
#pragma weak pmpi_status_set_cancelled_ = mpi_status_set_cancelled_f
#pragma weak pmpi_status_set_cancelled__ = mpi_status_set_cancelled_f
#elif OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (PMPI_STATUS_SET_CANCELLED,
                           pmpi_status_set_cancelled,
                           pmpi_status_set_cancelled_,
                           pmpi_status_set_cancelled__,
                           pmpi_status_set_cancelled_f,
                           (MPI_Fint *status, ompi_fortran_logical_t *flag, MPI_Fint *ierr),
                           (status, flag, ierr) )
#endif

#if OPAL_HAVE_WEAK_SYMBOLS
#pragma weak MPI_STATUS_SET_CANCELLED = mpi_status_set_cancelled_f
#pragma weak mpi_status_set_cancelled = mpi_status_set_cancelled_f
#pragma weak mpi_status_set_cancelled_ = mpi_status_set_cancelled_f
#pragma weak mpi_status_set_cancelled__ = mpi_status_set_cancelled_f
#endif

#if ! OPAL_HAVE_WEAK_SYMBOLS && ! OMPI_PROFILE_LAYER
OMPI_GENERATE_F77_BINDINGS (MPI_STATUS_SET_CANCELLED,
                           mpi_status_set_cancelled,
                           mpi_status_set_cancelled_,
                           mpi_status_set_cancelled__,
                           mpi_status_set_cancelled_f,
                           (MPI_Fint *status, ompi_fortran_logical_t *flag, MPI_Fint *ierr),
                           (status, flag, ierr) )
#endif


#if OMPI_PROFILE_LAYER && ! OPAL_HAVE_WEAK_SYMBOLS
#include "ompi/mpi/f77/profile/defines.h"
#endif

void mpi_status_set_cancelled_f(MPI_Fint *status, ompi_fortran_logical_t *flag, MPI_Fint *ierr)
{
    MPI_Status c_status;

    /* This seems silly, but someone will do it */

    if (OMPI_IS_FORTRAN_STATUS_IGNORE(status)) {
        *ierr = OMPI_INT_2_FINT(MPI_SUCCESS);
    } else {
        MPI_Status_f2c( status, &c_status );

        *ierr = OMPI_INT_2_FINT(MPI_Status_set_cancelled(&c_status,
                                                         OMPI_LOGICAL_2_INT(*flag)));

        if (MPI_SUCCESS == OMPI_FINT_2_INT(*ierr)) {
            MPI_Status_c2f(&c_status, status);
        }
    }
}
