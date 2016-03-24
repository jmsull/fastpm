#include <string.h>
#include <mpi.h>

#include <fastpm/libfastpm.h>
#include <fastpm/prof.h>
#include <fastpm/logging.h>

#include "pmpfft.h"
#include "pmghosts.h"
#include "pmstore.h"

static void 
apply_force_kernel(PM * pm, FastPMFloat * from, FastPMFloat * to, int dir) 
{
    /* This is the force in fourier space. - i k[dir] / k2 */

#pragma omp parallel 
    {
        PMKIter kiter;

        for(pm_kiter_init(pm, &kiter);
            !pm_kiter_stop(&kiter);
            pm_kiter_next(&kiter)) {
            int d;
            double k_finite = kiter.k_finite[dir][kiter.iabs[dir]];
            double kk_finite = 0;
            for(d = 0; d < 3; d++) {
                kk_finite += kiter.kk_finite[d][kiter.iabs[d]];
            }
            ptrdiff_t ind = kiter.ind;
            /* - i k[d] / k2 */
            if(LIKELY(kk_finite > 0)) {
                to[ind + 0] =   from[ind + 1] * (k_finite / kk_finite);
                to[ind + 1] = - from[ind + 0] * (k_finite / kk_finite);
            } else {
                to[ind + 0] = 0;
                to[ind + 1] = 0;
            }
        }
    }
}

void 
pm_calculate_forces(PMStore * p, PM * pm, FastPMFloat * delta_k, double density_factor)
{

    CLOCK(ghosts);
    PMGhostData * pgd = pm_ghosts_create(pm, p, PACK_POS, NULL); 
    LEAVE(ghosts);

    FastPMFloat * canvas = pm_alloc(pm);

    /* Watch out: this paints number of particles per cell. when pm_nc_factor is not 1, 
     * it is less than the density (a cell is smaller than the mean seperation between particles. 
     * We thus have to boost the density by density_factor.
     * */
    CLOCK(paint);
    pm_paint(pm, canvas, p, p->np + pgd->nghosts, density_factor);
    LEAVE(paint);
 
    CLOCK(r2c);
    pm_r2c(pm, canvas, delta_k);
    LEAVE(r2c);

    /* calculate the forces save them to p->acc */

    int d;
    ptrdiff_t i;
    int ACC[] = {PACK_ACC_X, PACK_ACC_Y, PACK_ACC_Z};
    for(d = 0; d < 3; d ++) {
        CLOCK(transfer);
        apply_force_kernel(pm, delta_k, canvas, d);
        LEAVE(transfer);

        CLOCK(c2r);
        pm_c2r(pm, canvas);
        LEAVE(c2r);

        CLOCK(readout);
#pragma omp parallel for
        for(i = 0; i < p->np + pgd->nghosts; i ++) {
            p->acc[i][d] = pm_readout_one(pm, canvas, p, i);
        }
        LEAVE(readout);

        CLOCK(reduce);
        pm_ghosts_reduce(pgd, ACC[d]); 
        LEAVE(reduce);
    }

    pm_free(pm, canvas);

    pm_ghosts_free(pgd);
}    

/* measure the linear scale power spectrum up to Nmax, 
 * returns 1.0 if no such scale. k == 0 is skipped. */
double
pm_calculate_linear_power(PM * pm, FastPMFloat * delta_k, int Nmax)
{
    double sum = 0;
    double N   = 0;
    double Norm = 0;

#pragma omp parallel 
    {
        PMKIter kiter;
        for(pm_kiter_init(pm, &kiter);
            !pm_kiter_stop(&kiter);
            pm_kiter_next(&kiter)) {
            /* Always use a fixed kmax */
            double kkmax = kiter.kk[0][Nmax];
            int d;
            double kk = 0.;
            for(d = 0; d < 3; d++) {
                double kk1 = kiter.kk[d][kiter.iabs[d]];
                if(kk1 > kkmax) {
                    goto next;
                }
                kk += kk1;
            }
            ptrdiff_t ind = kiter.ind;

            double real = delta_k[ind + 0];
            double imag = delta_k[ind + 1];
            double value = real * real + imag * imag;
            if(kk > 0.0001 * kkmax && kk < kkmax) {
                int w = 2;
                if(kiter.iabs[2] == 0) w = 1;
                #pragma omp atomic
                sum += w * value;
                #pragma omp atomic
                N += w;
            }
            if(kk == 0) {
                Norm = value;
            }
            next:
            continue;
        }
    }

    MPI_Allreduce(MPI_IN_PLACE, &Norm, 1, MPI_DOUBLE, MPI_SUM, pm->Comm2D);
    MPI_Allreduce(MPI_IN_PLACE, &N, 1, MPI_DOUBLE, MPI_SUM, pm->Comm2D);
    MPI_Allreduce(MPI_IN_PLACE, &sum, 1, MPI_DOUBLE, MPI_SUM, pm->Comm2D);

    double rt;
    if(N > 1) {
        rt = sum / N * pm->Volume / Norm;
    } else {
        rt = 1.0;
    }
/*    fastpm_info("norm factor = Norm / pm->Norm = %g power=%g\n", sqrt(Norm) / pm->Norm, rt); */
    return rt;
}

