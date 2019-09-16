/*
 *
 * Copyright 2019 The RMG Project Developers. See the COPYRIGHT file 
 * at the top-level directory of this distribution or in the current
 * directory.
 * 
 * This file is part of RMG. 
 * RMG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * any later version.
 *
 * RMG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
*/

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>


#include "const.h"
#include "Exxbase.h"
#include "RmgTimer.h"
#include "RmgException.h"
#include "RmgGemm.h"
#include "transition.h"
#include "rmgtypedefs.h"

// This class implements exact exchange for delocalized orbitals.
// The wavefunctions are stored in a single file and are not domain
// decomposed. The file name is given by wavefile_in which is
// mmapped to an array. We only access the array in read-only mode.


template Exxbase<double>::Exxbase(BaseGrid &, Lattice &, const std::string &, int, double *, double *);
template Exxbase<std::complex<double>>::Exxbase(BaseGrid &, Lattice &, const std::string &, int, double *, std::complex<double> *);

template Exxbase<double>::~Exxbase(void);
template Exxbase<std::complex<double>>::~Exxbase(void);

template <class T> Exxbase<T>::Exxbase (
          BaseGrid &G_in,
          Lattice &L_in,
          const std::string &wavefile_in,
          int nstates_in,
          double *occ_in,
          T *psi_in) : G(G_in), L(L_in), wavefile(wavefile_in), nstates(nstates_in), occ(occ_in), psi(psi_in)
{
    RmgTimer RT0("5-Functional: Exx init");

    tpiba = 2.0 * PI / L.celldm[0];
    tpiba2 = tpiba * tpiba;
    alpha = L.get_omega() / ((double)(G.get_NX_GRID(1) * G.get_NY_GRID(1) * G.get_NZ_GRID(1)));
    mode = EXX_DIST_FFT;
    pbasis = G.get_P0_BASIS(1);
    N = (size_t)G.get_NX_GRID(G.default_FG_RATIO) *
              (size_t)G.get_NY_GRID(G.default_FG_RATIO) *
              (size_t)G.get_NZ_GRID(G.default_FG_RATIO);

    int ratio = G.default_FG_RATIO;

    setup_gfac();

    if(mode == EXX_DIST_FFT) return;

    // Write the domain distributed wavefunction array and map it to psi_s
    MPI_Datatype wftype = MPI_DOUBLE;
    if(typeid(T) == typeid(std::complex<double>)) wftype = MPI_DOUBLE_COMPLEX;

    int sizes_c[3], sizes_f[3];
    int subsizes_c[3], subsizes_f[3];
    int starts_c[3], starts_f[3];

    sizes_c[0] = G.get_NX_GRID(1);
    sizes_c[1] = G.get_NY_GRID(1);
    sizes_c[2] = G.get_NZ_GRID(1);

    sizes_f[0] = G.get_NX_GRID(ratio);
    sizes_f[1] = G.get_NY_GRID(ratio);
    sizes_f[2] = G.get_NZ_GRID(ratio);

    subsizes_c[0] = G.get_PX0_GRID(1);
    subsizes_c[1] = G.get_PY0_GRID(1);
    subsizes_c[2] = G.get_PZ0_GRID(1);

    subsizes_f[0] = G.get_PX0_GRID(ratio);
    subsizes_f[1] = G.get_PY0_GRID(ratio);
    subsizes_f[2] = G.get_PZ0_GRID(ratio);

    starts_c[0] = G.get_PX_OFFSET(1);
    starts_c[1] = G.get_PY_OFFSET(1);
    starts_c[2] = G.get_PZ_OFFSET(1);

    starts_f[0] = G.get_PX_OFFSET(ratio);
    starts_f[1] = G.get_PY_OFFSET(ratio);
    starts_f[2] = G.get_PZ_OFFSET(ratio);

    int order = MPI_ORDER_C;
    MPI_Info fileinfo;
    MPI_Datatype grid_c, grid_f;
    MPI_Status status;


    MPI_Type_create_subarray(3, sizes_c, subsizes_c, starts_c, order, wftype, &grid_c);
    MPI_Type_commit(&grid_c);

    MPI_Type_create_subarray(3, sizes_f, subsizes_f, starts_f, order, MPI_DOUBLE, &grid_f);
    MPI_Type_commit(&grid_f);

    MPI_Info_create(&fileinfo);

    int amode = MPI_MODE_RDWR|MPI_MODE_CREATE;
    MPI_File mpi_fhand ;

    MPI_Barrier(G.comm);

    MPI_File_open(G.comm, wavefile.c_str(), amode, fileinfo, &mpi_fhand);
    MPI_Offset disp = 0;
    T *wfptr = psi;

    MPI_File_set_view(mpi_fhand, disp, wftype, grid_c, "native", MPI_INFO_NULL);
    for(int st=0;st < nstates;st++)
    {
        MPI_File_write_all(mpi_fhand, wfptr, pbasis, MPI_DOUBLE, &status);
        wfptr += pbasis;
    }
    MPI_Barrier(G.comm);
    MPI_File_close(&mpi_fhand);
    fflush(NULL);
    MPI_Barrier(G.comm);

    serial_fd = open(wavefile.c_str(), O_RDONLY, (mode_t)0600);
    if(serial_fd < 0)
        throw RmgFatalException() << "Error! Could not open " << wavefile << " . Terminating.\n";

    size_t length = nstates * N;
    psi_s = (T *)mmap(NULL, length, PROT_READ, MAP_PRIVATE, serial_fd, 0);
    MPI_Barrier(G.comm);

    MPI_Type_free(&grid_f);
    MPI_Type_free(&grid_c);

    // Generate the full set of pairs and store in a temporary vector
    // This is for gamma only right now
    int npairs = (N*N + N) / 2;
    std::vector< std::pair <int,int> > temp_pairs; 
    temp_pairs.resize(npairs);
    for(int i=0;i < npairs;i++)
    {
        for(int j=i;j < npairs;j++)
        {
            temp_pairs.push_back(std::make_pair(i, j));
        }
    }

    // Compute start and count for this MPI task
    pair_start = 0;
    pair_count = npairs;
    int rank = G.get_rank();
    int NPES = G.get_NPES(); 
    if(NPES > 1)
    {
        pair_count = npairs / NPES;
        pair_start = pair_count * rank;
        int rem = pair_count % NPES;
        if(rank < rem)
        {
            pair_count++;
            pair_start += rank;
        }
        else
        {
            pair_start += rem;
        }

    }

    // Copy the ones we are responsible for into our local vector of pairs
    pairs.resize(pair_count);
    for(int st=0;st < pair_count;st++)
    {
        pairs[st] = temp_pairs[pair_start + st];
    }

    LG = new BaseGrid(G.get_NX_GRID(1), G.get_NY_GRID(1), G.get_NZ_GRID(1), 1, 1, 1, 0, 1);
    MPI_Comm_split(G.comm, rank+1, rank, &lcomm);
    LG->set_rank(0, lcomm);
    pwave = new Pw(*LG, L, 1, false);
    
    // Now we have to figure out how to distribute the pairs over computational resources.
    // If we only have CPU's then it's easy
}

template <> void Exxbase<double>::fftpair(double *psi_i, double *psi_j, std::complex<double> *p)
{
    std::complex<double> ZERO_t(0.0, 0.0);
    for(int idx=0;idx < pbasis;idx++) p[idx] = std::complex<double>(psi_i[idx] * psi_j[idx], 0.0);
    coarse_pwaves->FftForward(p, p);
    for(int ig=0;ig < pbasis;ig++) p[ig] *= gfac[ig];
    coarse_pwaves->FftInverse(p, p);
}

template <> void Exxbase<std::complex<double>>::fftpair(std::complex<double> *psi_i, std::complex<double> *psi_j, std::complex<double> *p)
{
}

// This implements different ways of handling the divergence at G=0
template <> void Exxbase<double>::setup_gfac(void)
{
    gfac = new double[pbasis];

    const std::string &dftname = Functional::get_dft_name_rmg();
    double scrlen = Functional::get_screening_parameter_rmg();

    if((dftname == "gaupbe") || (scr_type == GAU_SCREENING))
    {
        double gau_scrlen = Functional::get_gau_parameter_rmg();
        double a0 = pow(PI / gau_scrlen, 1.5);
        for(int ig=0;ig < pbasis;ig++)
        {
            if(coarse_pwaves->gmask[ig])
                gfac[ig] = a0 * exp(-tpiba2*coarse_pwaves->gmags[ig] / 4.0 / gau_scrlen);
            else
                gfac[ig] = 0.0;
        }
    }
    else if(scr_type == ERF_SCREENING)
    {
        double a0 = 4.0 * PI;
        for(int ig=0;ig < pbasis;ig++)
        {
            if((coarse_pwaves->gmags[ig] > 1.0e-6) && coarse_pwaves->gmask[ig])
                gfac[ig] = a0 * exp(-tpiba2*coarse_pwaves->gmags[ig] / 4.0 / (scrlen*scrlen)) / (tpiba2*coarse_pwaves->gmags[ig]);
            else
                gfac[ig] = 0.0;
        }
    }
    else if(scr_type == ERFC_SCREENING)
    {
        double a0 = 4.0 * PI;
        for(int ig=0;ig < pbasis;ig++)
        {
            if((coarse_pwaves->gmags[ig] > 1.0e-6) && coarse_pwaves->gmask[ig])
                gfac[ig] = a0 * (1.0 - exp(-tpiba2*coarse_pwaves->gmags[ig] / 4.0 / (scrlen*scrlen))) / (tpiba2*coarse_pwaves->gmags[ig]);
            else
                gfac[ig] = 0.0;
        }
    }
    else  // Default is probably wrong
    {
        for(int ig=0;ig < pbasis;ig++)
        {
            if((coarse_pwaves->gmags[ig] > 1.0e-6) && coarse_pwaves->gmask[ig])
                gfac[ig] = 1.0/(coarse_pwaves->gmags[ig] *tpiba2);
            else
                gfac[ig] = 0.0;
        }
    }

}

template <> void Exxbase<std::complex<double>>::setup_gfac(void)
{
}

// These compute the action of the exact exchange operator on all wavefunctions
// and writes the result into vfile.
template <> void Exxbase<double>::Vexx(double *vexx)
{
    RmgTimer RT0("5-Functional: Exx potential");
    double scale = - 1.0 / (double)coarse_pwaves->global_basis;

    double ZERO_t(0.0), ONE_T(1.0);
    char *trans_a = "t";
    char *trans_n = "n";

    // Clear vexx
    for(int idx=0;idx < nstates*pbasis;idx++) vexx[idx] = 0.0;

    int nstates_occ = 0;
    for(int st=0;st < nstates;st++) if(occ[st] > 1.0e-6) nstates_occ++;

    if(mode == EXX_DIST_FFT)
    {
        std::complex<double> *p = (std::complex<double> *)fftw_malloc(sizeof(std::complex<double>) * pbasis);
        // Loop over fft pairs and compute Kij(r) 
        for(int i=0;i < nstates_occ;i++)
        {
            double *psi_i = (double *)&psi[i*pbasis];
            for(int j=0;j < nstates_occ;j++)
            {   
                double *psi_j = (double *)&psi[j*pbasis];
                RmgTimer RT1("5-Functional: Exx potential fft");
                fftpair(psi_i, psi_j, p);
//if(G.get_rank()==0)printf("TTTT  %d  %d  %e\n",i,j,std::real(p[1]));
                for(int idx = 0;idx < pbasis;idx++)vexx[i*pbasis +idx] += scale * std::real(p[idx]) * psi[j*pbasis + idx];
            }
        }

        fftw_free(p);
    }
    else
    {
        rmg_error_handler (__FILE__,__LINE__,"Exx potential mode not programmed yet. Terminating.");
    }

}


template <> void Exxbase<std::complex<double>>::Vexx(std::complex<double> *vexx)
{
    RmgTimer RT0("5-Functional: Exx potential");
    rmg_error_handler (__FILE__,__LINE__,"Exx potential not programmed for non-gamma yet. Terminating.");
}

// This computes exact exchange integrals
// and writes the result into vfile.
template <> void Exxbase<double>::Vexx_integrals(std::string &vfile)
{

    double beta = 0.0;
    double scale = 1.0 / (double)coarse_pwaves->global_basis;

    char *trans_a = "t";
    char *trans_n = "n";
    int nstates_occ = 0;
    for(int st=0;st < nstates;st++) if(occ[st] > 1.0e-6) nstates_occ++;


    if(mode == EXX_DIST_FFT)
    {
        RmgTimer RT0("5-Functional: Exx integrals");

        std::vector< std::pair <int,int> > wf_pairs;
        for(int i=0;i < nstates_occ;i++)
        {
            for(int j=i;j < nstates_occ;j++)
            {
                wf_pairs.push_back(std::make_pair(i, j));
            }
        }

        block_size = 16;
        if(block_size > nstates_occ) block_size = nstates_occ;
        int num_blocks = wf_pairs.size()/block_size;
        int num_rem = wf_pairs.size() % block_size;

        // The wave function array is always at least double the number of states so use
        // the upper part for storage of our kl pairs

        kl_pair = (double *)&psi[nstates * pbasis];

        // We block the ij pairs for GEMM efficiency
        ij_pair = new double[block_size * pbasis];

        Exxints = new double[block_size * block_size];
        Summedints = new double[block_size * block_size];

        std::complex<double> *p = (std::complex<double> *)fftw_malloc(sizeof(std::complex<double>) * pbasis);
        double *psi_base = (double *)psi;

        for(int ib =0; ib < num_blocks; ib++)
        {

            for(int ic = 0; ic < block_size; ic++)
            {
                int ij = ib * block_size + ic;
                int i = wf_pairs[ij].first;
                int j = wf_pairs[ij].second;
                double *psi_i = (double *)&psi[i*pbasis];
                double *psi_j = (double *)&psi[j*pbasis];
                fftpair(psi_i, psi_j, p);

                // store (i,j) fft pair in ij_pair
                // forward and then backward fft should not have the scale, Wenchang
                for(int idx=0;idx < pbasis;idx++) ij_pair[ic * pbasis + idx] = scale * std::real(p[idx]);
            }


            // calculate kl pairs
            for(int id =ib; id < num_blocks; id++)
            {
                for(int ie = 0; ie < block_size; ie++)
                {
                    int kl = id * block_size + ie;
                    int k = wf_pairs[kl].first;
                    int l = wf_pairs[kl].second;
                    double *psi_k = (double *)&psi[k*pbasis];
                    double *psi_l = (double *)&psi[l*pbasis];
                    for(int idx=0;idx < pbasis;idx++) kl_pair[ie*pbasis + idx] = psi_k[idx]*psi_l[idx];
                }

                // Now compute integrals for (i, j, k, l)

                // Now matrix multiply to produce a block of (1,jblocks, 1, nstates_occ) results
                RmgGemm(trans_a, trans_n, block_size, block_size, pbasis, alpha, ij_pair, pbasis, kl_pair, pbasis, beta, Exxints, block_size);
                int pairsize = block_size * block_size;
                MPI_Reduce(Exxints, Summedints, pairsize, MPI_DOUBLE, MPI_SUM, 0, G.comm);

                for(int ic = 0; ic < block_size; ic++)
                {
                    int ij = ib * block_size + ic;
                    int i = wf_pairs[ij].first;
                    int j = wf_pairs[ij].second;

                    for(int ie = 0; ie < block_size; ie++)
                    {
                        int kl = id * block_size + ie;
                        int k = wf_pairs[kl].first;
                        int l = wf_pairs[kl].second;

                        if(G.get_rank()==0)printf("KKK  = (%d,%d,%d,%d)  %14.8e\n", i, j, k, l, Summedints[ie * block_size + ic]);
                        //if(G.get_rank()==0)printf("%d  %14.8e\n", i*16*16*16+ j*16*16+ k*16+ l, Summedints[ie * block_size + ic]);
                    }
                }

            }

            // calculate remaining kl pairs
            for(int kl = block_size * num_blocks; kl < int(wf_pairs.size()); kl++)
            {
                int ie = kl - block_size * num_blocks;
                int k = wf_pairs[kl].first;
                int l = wf_pairs[kl].second;
                double *psi_k = (double *)&psi[k*pbasis];
                double *psi_l = (double *)&psi[l*pbasis];
                for(int idx=0;idx < pbasis;idx++) kl_pair[ie*pbasis + idx] = psi_k[idx]*psi_l[idx];
            }


            // Now matrix multiply to produce a block of (1,jblocks, 1, nstates_occ) results
            RmgGemm(trans_a, trans_n, block_size, num_rem, pbasis, alpha, ij_pair, pbasis, kl_pair, pbasis, beta, Exxints, block_size);
            int pairsize = block_size * num_rem;
            MPI_Reduce(Exxints, Summedints, pairsize, MPI_DOUBLE, MPI_SUM, 0, G.comm);

            for(int ic = 0; ic < block_size; ic++)
            {
                int ij = ib * block_size + ic;
                int i = wf_pairs[ij].first;
                int j = wf_pairs[ij].second;

                for(int kl = block_size * num_blocks; kl <(int)wf_pairs.size(); kl++)
                {
                    int ie = kl - block_size * num_blocks;
                    int k = wf_pairs[kl].first;
                    int l = wf_pairs[kl].second;

                    if(G.get_rank()==0)printf("KKK  = (%d,%d,%d,%d)  %14.8e\n", i, j, k, l, Summedints[ie * block_size + ic]);
            //            if(G.get_rank()==0)printf("%d  %14.8e\n", i*16*16*16+ j*16*16+ k*16+ l, Summedints[ie * block_size + ic]);
                }
            }

        }

        for(int ij = block_size * num_blocks; ij < (int)wf_pairs.size(); ij++)
        {
            int ic = ij - block_size * num_blocks;
            int i = wf_pairs[ij].first;
            int j = wf_pairs[ij].second;
            double *psi_i = (double *)&psi[i*pbasis];
            double *psi_j = (double *)&psi[j*pbasis];
            fftpair(psi_i, psi_j, p);

            // store (i,j) fft pair in ij_pair
            // forward and then backward fft should not have the scale, Wenchang
            for(int idx=0;idx < pbasis;idx++) ij_pair[ic * pbasis + idx] = scale * std::real(p[idx]);
        }

        // calculate remaining kl pairs
        for(int kl = block_size * num_blocks; kl <(int)wf_pairs.size(); kl++)
        {
            int ie = kl - block_size * num_blocks;
            int k = wf_pairs[kl].first;
            int l = wf_pairs[kl].second;
            double *psi_k = (double *)&psi[k*pbasis];
            double *psi_l = (double *)&psi[l*pbasis];
            for(int idx=0;idx < pbasis;idx++) kl_pair[ie*pbasis + idx] = psi_k[idx]*psi_l[idx];
        }


        // Now matrix multiply to produce a block of (1,jblocks, 1, nstates_occ) results
        RmgGemm(trans_a, trans_n, num_rem, num_rem, pbasis, alpha, ij_pair, pbasis, kl_pair, pbasis, beta, Exxints, block_size);
        int pairsize = block_size * num_rem;
        MPI_Reduce(Exxints, Summedints, pairsize, MPI_DOUBLE, MPI_SUM, 0, G.comm);

        for(int ij = block_size * num_blocks; ij < (int)wf_pairs.size(); ij++)
        {
            int ic = ij - block_size * num_blocks;
            int i = wf_pairs[ij].first;
            int j = wf_pairs[ij].second;

            for(int kl = block_size * num_blocks; kl < (int)wf_pairs.size(); kl++)
            {
                int ie = kl - block_size * num_blocks;
                int k = wf_pairs[kl].first;
                int l = wf_pairs[kl].second;

                if(G.get_rank()==0)printf("KKK  = (%d,%d,%d,%d)  %14.8e\n", i, j, k, l, Summedints[ie * block_size + ic]);
              //          if(G.get_rank()==0)printf("%d  %14.8e\n", i*16*16*16+ j*16*16+ k*16+ l, Summedints[ie * block_size + ic]);
            }
        }





        fftw_free(p);
        delete [] ij_pair;
    }
    else
    {
        printf("Exx mode not programmed yet\n");
    }


}

template <> void Exxbase<std::complex<double>>::Vexx_integrals(std::string &vfile)
{
    printf("Exx mode not programmed yet\n");
}

template <class T> Exxbase<T>::~Exxbase(void)
{

    if(mode == EXX_DIST_FFT) return;

    close(serial_fd);
    size_t length = nstates * N;
    munmap(psi_s, length);
    unlink(wavefile.c_str());

    delete LG;
    MPI_Comm_free(&lcomm); 
    delete pwave;
}
