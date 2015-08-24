/************************** SVN Revision Information **************************
 **    $Id$    **
******************************************************************************/
 
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>



#include "make_conf.h"
#include "params.h"
#include "rmgtypedefs.h"
#include "typedefs.h"
#include "RmgTimer.h"

#include "prototypes_on.h"
#include "init_var.h"
#include "my_scalapack.h"
#include "transition.h"
#include "blas.h"
#include "Kbpsi.h"

/* Flag for projector mixing */
static int mix_steps;
double *work1;                    /* Smoothing grids */

static void get_qnm_res(double *work_theta);
static void get_nonortho_res(STATE *, double *, STATE *);
static void get_dnmpsi(STATE *);


void OrbitalOptimize(STATE * states, STATE * states1, double *vxc, double *vh,
            double *vnuc, double *rho, double *rhoc, double * vxc_old, double * vh_old)
{
    int istate, ione = 1;
    double t1;
    int st1, ixx, iyy, izz;
    int item;

    double hxgrid, hygrid, hzgrid;

    hxgrid = Rmg_G->get_hxgrid(1);
    hygrid = Rmg_G->get_hygrid(1);
    hzgrid = Rmg_G->get_hzgrid(1);



    ct.meanres = 0.;
    ct.minres = 100000.0;
    ct.maxres = 0.;

    /* Grab some memory */
    work1 = work_memory;

    RmgTimer *RT = new RmgTimer("3-OrbitalOptimize");



    RmgTimer *RT1a = new RmgTimer("3-OrbitalOptimize: distribute");
    distribute_to_global(vtot_c, vtot_global);
    delete(RT1a);




    /* calculate  Theta * S * |states[].psiR > and stored in  states1[].psiR 
     *  sum_j S(phi)_j*(Theta)_ji 
     */

    /*begin shuchun wang */
    RmgTimer *RT12 = new RmgTimer("3-OrbitalOptimize: dcopya");
    dcopy(&pct.psi_size, states[ct.state_begin].psiR, &ione,
            states_tem[ct.state_begin].psiR, &ione);
    delete(RT12);

    /*  add q_n,m|beta_n><beta_m|psi> on states_res.psiR */



    RmgTimer *RT3 = new RmgTimer("3-OrbitalOptimize: nonortho");
    get_nonortho_res(states, theta, states1);
    delete(RT3);
    RmgTimer *RT4 = new RmgTimer("3-OrbitalOptimize: qnm");
    get_qnm_res(theta);

    my_barrier();
    delete(RT4);
    /* end shuchun wang */




    /* Loop over states istate (correction of state istate) */
    RmgTimer *RT2a = new RmgTimer("3-OrbitalOptimize: mask");

    for (istate = ct.state_begin; istate < ct.state_end; istate++)
    {
        app_mask(istate, states1[istate].psiR, 0);
    }

    delete(RT2a);

    /* Loop over states istate to compute the whole matrix Hij 
       and all the vectors H|psi> */
    /* calculate the H |phi> on this processor and stored in states1[].psiR[] */

    RmgTimer *RTa = new RmgTimer("3-OrbitalOptimize: Hpsi");
    for (st1 = ct.state_begin; st1 < ct.state_end; st1++)
    {
        ixx = states[st1].ixmax - states[st1].ixmin + 1;
        iyy = states[st1].iymax - states[st1].iymin + 1;
        izz = states[st1].izmax - states[st1].izmin + 1;

        /* Generate 2*V*psi and store it  in orbit_tem */
        genvlocpsi(states[st1].psiR, st1, orbit_tem, vtot_global, states);
        t1 = -1.0;
        daxpy(&states[st1].size, &t1, orbit_tem, &ione, states1[st1].psiR, &ione);

        /* Pack psi into smoothing array */
        /*		pack_ptos(sg_orbit, states[st1].psiR, ixx, iyy, izz); 
         */
        /* A operating on psi stored in work2 */
        /*		app_cil(sg_orbit, orbit_tem, ixx, iyy, izz, get_hxgrid(), get_hygrid(), get_hzgrid()); 
         */
        app10_del2(states[st1].psiR, orbit_tem, ixx, iyy, izz, hxgrid, hygrid, hzgrid);

        t1 = 1.0;
        daxpy(&states[st1].size, &t1, orbit_tem, &ione, states1[st1].psiR, &ione);


        /*                                                                     
         * Add the contribution of the non-local potential to the 
         * residual 
         */
        /* Get the non-local operator acting on psi and store in nvtot */
        item = (st1 - ct.state_begin) * pct.n_ion_center * ct.max_nl;


    }                           /* end for st1 = .. */
    RmgTimer *RT5 = new RmgTimer("3-OrbitalOptimize: dnm");
        get_dnmpsi(states1);
    delete(RT5);


    for (istate = ct.state_begin; istate < ct.state_end; istate++)
    {
        app_mask(istate, states1[istate].psiR, 0);
    }

    delete(RTa);
     

    /*
     *    precond(states1[ct.state_begin].psiR);
     *    t1 = 0.1; 
     *    daxpy(&pct.psi_size, &t1, states1[ct.state_begin].psiR, &ione, states[ct.state_begin].psiR, &ione); 
     */


    /*  SD, Pulay or KAIN method for Linear and Nonlinear equations */

    RmgTimer *RT6a = new RmgTimer("3-OrbitalOptimize: scale");
    for (istate = ct.state_begin; istate < ct.state_end; istate++)
    {
        t1 = -1.0;
        dscal(&states1[istate].size, &t1, states1[istate].psiR, &ione);
    }

    delete(RT6a);
    if (ct.restart_mix == 1 || ct.move_centers_at_this_step == 1)
    {
        mix_steps = 0;
        if (pct.gridpe == 0)
            printf("\n restart the orbital mixing at step %d \n", ct.scf_steps);
    }


    RmgTimer *RT6 = new RmgTimer("3-OrbitalOptimize: mixing+precond");

    switch (ct.mg_method)
    {
        case 0:
            sd(ct.scf_steps, pct.psi_size, states[ct.state_begin].psiR, states1[ct.state_begin].psiR);
            break;
        case 1:
            pulay(mix_steps, pct.psi_size, states[ct.state_begin].psiR,
                    states1[ct.state_begin].psiR, ct.mg_steps, 1);
            break;
        case 2:
            kain(mix_steps, pct.psi_size, states[ct.state_begin].psiR,
                    states1[ct.state_begin].psiR, ct.mg_steps);
            break;
        case 3:
            pulay_weighted(mix_steps, pct.psi_size, states[ct.state_begin].psiR,
                    states1[ct.state_begin].psiR, ct.mg_steps, 100, 0.5, 1);
            break;
        default:
            printf("\n Undefined mg_method \n ");
            fflush(NULL);
            exit(0);
    }


    mix_steps++;

    delete(RT6);

    RmgTimer *RT7 = new RmgTimer("3-OrbitalOptimize: ortho_norm");
    ortho_norm_local(states); 

    delete(RT7);

   // normalize_orbits(states);





#if     DEBUG
    print_status(states, vh, vxc, vnuc, vh_old, "before leaving OrbitalOptimize.c  ");
    print_state_sum(states1);
#endif
    delete(RT);

} 

/*-----------------------------------------------------------------*/

static void get_nonortho_res(STATE * states, double *work_theta, STATE * states1)
{
    int i;
    int idx, st1, st2;
    double theta_ion;
    int loop, state_per_proc;
    int num_recv;
    double temp;
    int st11;


    state_per_proc = ct.state_per_proc + 2;

    for (st1 = ct.state_begin; st1 < ct.state_end; st1++)
        for (idx = 0; idx < states[st1].size; idx++)
            states1[st1].psiR[idx] = 0.0;

    for (st1 = ct.state_begin; st1 < ct.state_end; st1++)
    {
        st11 = st1-ct.state_begin;
        for (st2 = ct.state_begin; st2 < ct.state_end; st2++)
            if (state_overlap_or_not[st11 * ct.num_states + st2] == 1)
            {
                temp = work_theta[st11 * ct.num_states + st2];
                theta_phi_new(st1, st2, temp, states[st2].psiR, states1[st1].psiR, 0, states);
            }
    }

    /*  send overlaped orbitals to other PEs */
    my_barrier();




    for (loop = 0; loop < num_sendrecv_loop1; loop++)
    {

        num_recv = recv_from1[loop * state_per_proc + 1];

        for (i = 0; i < num_recv; i++)
        {
            st2 = recv_from1[loop * state_per_proc + i + 2];
            for (st1 = ct.state_begin; st1 < ct.state_end; st1++)
            {
                st11 = st1-ct.state_begin;
                if (state_overlap_or_not[st11 * ct.num_states + st2] == 1)
                {
                    theta_ion = work_theta[st11 * ct.num_states + st2];
                    theta_phi_new(st1, st2, theta_ion, states[st2].psiR, states1[st1].psiR, 0, states);
                }
            }
        }

    }

}

void get_qnm_res(double *work_theta)
{

    int ip1, st1, st2;
    int ion1, ion2, ion1_global, ion2_global;
    int iip1, iip2;
    int size, proc, proc1, proc2, idx;
    int st11;
    int size_projector;
    int num_prj, num_orb, tot_orb, idx1, idx2;


    for (ion1 = 0; ion1 < pct.n_ion_center; ion1++)
    {

        num_prj = pct.prj_per_ion[pct.ionidx[ion1]];
        num_orb = Kbpsi_str.num_orbital_thision[ion1]; 
        tot_orb = Kbpsi_str.orbital_index[ion1].size();

        //  set the length of vector and set their value to 0.0
        Kbpsi_str.kbpsi_res_ion[ion1].resize(num_orb * num_prj);
        std::fill(Kbpsi_str.kbpsi_res_ion[ion1].begin(), 
                Kbpsi_str.kbpsi_res_ion[ion1].end(), 0.0);

        for(idx1 = 0; idx1 < num_orb; idx1++)
        {
            st1 = Kbpsi_str.orbital_index[ion1][idx1];
            st11 = st1 - ct.state_begin;

            for(idx2 = 0; idx2 < tot_orb; idx2++)
            {
                st2 = Kbpsi_str.orbital_index[ion1][idx2];

                for (ip1 = 0; ip1 < num_prj; ip1++)
                    Kbpsi_str.kbpsi_res_ion[ion1][idx1 * num_prj + ip1] += 
                        Kbpsi_str.kbpsi_ion[ion1][idx2 * num_prj + ip1] *
                        work_theta[st11 * ct.num_states + st2];
            }
        }

    }         



}


void get_dnmpsi(STATE *states1)
{
    int ion, ipindex, idx, ip1, ip2;
    double *prjptr;
    int ion2, num_prj, st0, st1;
    double *ddd, *qnm_weight;
    double *qqq;
    double *prj_sum;
    double qsum;

    

    /*
     *  dnm_weght[i] = sum_j dnm[i, j] *<beta_j|phi> 
     * prj_sum(r) = sum_i nm_weight[i] *beta_i(r)  
     */

    qnm_weight = new double[ct.max_nl];
    prj_sum = new double[ct.max_nlpoints];


    prjptr = projectors;

    for (ion2 = 0; ion2 < pct.n_ion_center; ion2++)
    {
        ion = pct.ionidx[ion2];
        ipindex = ion2 * ct.max_nl;
        qqq = pct.qqq[ion];
        ddd = pct.dnmI[ion];

        num_prj = pct.prj_per_ion[ion];

        for(st0 = 0; st0 < Kbpsi_str.num_orbital_thision[ion2]; st0++)
        {
            st1 = Kbpsi_str.orbital_index[ion2][st0];

            for (ip1 = 0; ip1 < num_prj; ip1++)
            {

                qsum = 0.0;

                for (ip2 = 0; ip2 < num_prj; ip2++)
                {

                    qsum -= ddd[ip1 * num_prj + ip2] * Kbpsi_str.kbpsi_ion[ion2][st0 * num_prj + ip2];
                    qsum += 0.5*qqq[ip1 * num_prj + ip2] * Kbpsi_str.kbpsi_res_ion[ion2][st0 * num_prj + ip2];
                }

                qnm_weight[ip1] = 2.0 * qsum;

            }


            for (idx = 0; idx < ct.max_nlpoints; idx++)
            {
                prj_sum[idx] = 0.0;
            }


            for (ip1 = 0; ip1 < num_prj; ip1++)
            {
                for (idx = 0; idx < ct.max_nlpoints; idx++)
                {

                    prj_sum[idx] += qnm_weight[ip1] * prjptr[ip1 * ct.max_nlpoints + idx];
                }
            }

            /*
             *  project the prj_sum to the orbital |phi>  and stored in work 
             */



            qnm_beta_betapsi(&states1[st1], ion, prj_sum);


        }

        prjptr += pct.prj_per_ion[ion] * ct.max_nlpoints;       

    }                           /* end for ion */

    delete [] qnm_weight;
    delete [] prj_sum;


}