#include <cmath>

#include <ParmParse.H>
#include "Gravity.H"
#include "Castro.H"
#include <Gravity_F.H>
#include <Castro_F.H>

#include <MacBndry.H>
#include <MGT_Solver.H>
#include <stencil_types.H>
#include <mg_cpp_f.h>

#define MAX_LEV 15

// Give this a bogus default value to force user to define in inputs file
std::string Gravity::gravity_type = "fillme";
#ifndef NDEBUG
#ifndef PARTICLES
int Gravity::test_solves  = 1;
#else
int Gravity::test_solves  = 0;
#endif
#else
int Gravity::test_solves  = 0;
#endif
int  Gravity::verbose        = 0;
int  Gravity::no_sync        = 0;
int  Gravity::no_composite   = 0;
int  Gravity::drdxfac        = 1;
int  Gravity::lnum           = 0;
int  Gravity::direct_sum_bcs = 0;
Real Gravity::sl_tol         = 1.e100;
Real Gravity::ml_tol         = 1.e100;
Real Gravity::delta_tol      = 1.e100;
Real Gravity::const_grav     =  0.0;
Real Gravity::max_radius_all_in_domain =  0.0;
Real Gravity::mass_offset    =  0.0;
int  Gravity::stencil_type   = CC_CROSS_STENCIL;

// ************************************************************************************** //

// Ggravity is defined as -4 * pi * G, where G is the gravitational constant.

// In CGS, this constant is currently 
//      Gconst   =  6.67428e-8           cm^3/g/s^2 , which results in 
//      Ggravity = -83.8503442814844e-8  cm^3/g/s^2

// ************************************************************************************** //

static Real Ggravity = 0.;

Array< Array<Real> > Gravity::radial_grav_old(MAX_LEV);
Array< Array<Real> > Gravity::radial_grav_new(MAX_LEV);
Array< Array<Real> > Gravity::radial_mass(MAX_LEV);
Array< Array<Real> > Gravity::radial_vol(MAX_LEV);
#ifdef GR_GRAV
Array< Array<Real> > Gravity::radial_pres(MAX_LEV);
#endif
 
Gravity::Gravity(Amr* Parent, int _finest_level, BCRec* _phys_bc, int _Density)
  : 
    parent(Parent),
    LevelData(MAX_LEV),
    phi_prev(MAX_LEV,PArrayManage),
    phi_curr(MAX_LEV,PArrayManage),
    grad_phi_curr(MAX_LEV),
    grad_phi_prev(MAX_LEV),
    phi_flux_reg(MAX_LEV,PArrayManage),
    grids(MAX_LEV),
    level_solver_resnorm(MAX_LEV),
    volume(MAX_LEV),
    area(MAX_LEV),
    phys_bc(_phys_bc)
{
     Density = _Density;
     read_params();
     finest_level_allocated = -1;
     if (gravity_type == "PoissonGrav") make_mg_bc();
}

Gravity::~Gravity() {}

void
Gravity::read_params ()
{
    static bool done = false;

    if (!done)
    {
        ParmParse pp("gravity");

        pp.get("gravity_type", gravity_type);

        if ( (gravity_type != "ConstantGrav") && 
	     (gravity_type != "PoissonGrav") && 
	     (gravity_type != "MonopoleGrav") &&
             (gravity_type != "PrescribedGrav") )
             {
                std::cout << "Sorry -- dont know this gravity type"  << std::endl;
        	BoxLib::Abort("Options are ConstantGrav, PoissonGrav, MonopoleGrav, or PrescribedGrav");
             }
	     
        if (  gravity_type == "ConstantGrav") 
        {
	  if ( Geometry::IsSPHERICAL() )
	      BoxLib::Abort("Cant use constant direction gravity with non-Cartesian coordinates");
           pp.get("const_grav", const_grav);
        }

#if (BL_SPACEDIM == 1)
        if (gravity_type == "PoissonGrav")
        {
	  BoxLib::Abort(" gravity_type = PoissonGrav doesn't work well in 1-d -- please set gravity_type = MonopoleGrav");
        } 
        else if (gravity_type == "MonopoleGrav" && !(Geometry::IsSPHERICAL()))
        {
	  BoxLib::Abort("Only use MonopoleGrav in 1D spherical coordinates");
        }
        else if (gravity_type == "ConstantGrav" && Geometry::IsSPHERICAL())
        {
	  BoxLib::Abort("Can't use constant gravity in 1D spherical coordinates");
        }
	  
#elif (BL_SPACEDIM == 2)
        if (gravity_type == "MonopoleGrav" && Geometry::IsCartesian() )
        {
	  BoxLib::Abort(" gravity_type = MonopoleGrav doesn't make sense in 2D Cartesian coordinates");
        } 
#endif

        pp.query("drdxfac", drdxfac);

        pp.query("v", verbose);
        pp.query("no_sync", no_sync);
        pp.query("no_composite", no_composite);
 
        pp.query("max_multipole_order", lnum);
    
        // Check if the user wants to compute the boundary conditions using the brute force method.
        // Default is false, since this method is slow.

        pp.query("direct_sum_bcs", direct_sum_bcs);

        // Allow run-time input of solver tolerances
	if (Geometry::IsCartesian()) {
	  ml_tol = 1.e-11;
	  sl_tol = 1.e-11;
	  delta_tol = 1.e-11;
	}
	else {
	  ml_tol = 1.e-10;
	  sl_tol = 1.e-10;
	  delta_tol = 1.e-10;
	}
        pp.query("ml_tol",ml_tol);
        pp.query("sl_tol",sl_tol);
        pp.query("delta_tol",delta_tol);

        Real Gconst;
        BL_FORT_PROC_CALL(GET_GRAV_CONST, get_grav_const)(&Gconst);
        Ggravity = -4.0 * M_PI * Gconst;
        if (verbose > 0 && ParallelDescriptor::IOProcessor())
        {
           std::cout << "Getting Gconst from constants: " << Gconst << std::endl;
           std::cout << "Using " << Ggravity << " for 4 pi G in Gravity.cpp " << std::endl;
        }

        done = true;
    }
}

void
Gravity::set_numpts_in_gravity (int numpts)
{
  numpts_at_level = numpts;
}

void
Gravity::install_level (int                   level,
                        AmrLevel*             level_data,
                        MultiFab&             _volume,
                        MultiFab*             _area)
{
    if (verbose > 1 && ParallelDescriptor::IOProcessor())
        std::cout << "Installing Gravity level " << level << '\n';

    LevelData.clear(level);
    LevelData.set(level, level_data);

    volume.clear(level);
    volume.set(level, &_volume);

    area.set(level, _area);

    BoxArray ba(LevelData[level].boxArray());
    grids[level] = ba;

    level_solver_resnorm[level] = 0.0;

    if (gravity_type == "PoissonGrav") {

       phi_prev.clear(level);
       phi_prev.set(level,new MultiFab(grids[level],1,1));
       phi_prev[level].setVal(0.0);

       phi_curr.clear(level);
       phi_curr.set(level,new MultiFab(grids[level],1,1));
       phi_curr[level].setVal(0.0);

       grad_phi_prev[level].clear();
       grad_phi_prev[level].resize(BL_SPACEDIM,PArrayManage);
       for (int n=0; n<BL_SPACEDIM; ++n)
           grad_phi_prev[level].set(n,new MultiFab(BoxArray(grids[level]).surroundingNodes(n),1,1));

       grad_phi_curr[level].clear();
       grad_phi_curr[level].resize(BL_SPACEDIM,PArrayManage);
       for (int n=0; n<BL_SPACEDIM; ++n)
           grad_phi_curr[level].set(n,new MultiFab(BoxArray(grids[level]).surroundingNodes(n),1,1));

       if (level > 0) {
          phi_flux_reg.clear(level);
          IntVect crse_ratio = parent->refRatio(level-1);
          phi_flux_reg.set(level,new FluxRegister(grids[level],crse_ratio,level,1));
       }

#if (BL_SPACEDIM > 1)
    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {

        if (!Geometry::isAllPeriodic())
        {
           int n1d = drdxfac*numpts_at_level;

           radial_grav_old[level].resize(n1d);
           radial_grav_new[level].resize(n1d);
           radial_mass[level].resize(n1d);
           radial_vol[level].resize(n1d);
#ifdef GR_GRAV
           radial_pres[level].resize(n1d);
#endif
        }

#endif
    }

    // Compute the maximum radius at which all the mass at that radius is in the domain,
    //   assuming that the "hi" side of the domain is away from the center.
#if (BL_SPACEDIM > 1)
    if (level == 0)
    {
        Real center[BL_SPACEDIM];
        BL_FORT_PROC_CALL(GET_CENTER,get_center)(center);
        Real x = Geometry::ProbHi(0) - center[0];
        Real y = Geometry::ProbHi(1) - center[1];
        max_radius_all_in_domain = std::min(x,y);
#if (BL_SPACEDIM == 3)
        Real z = Geometry::ProbHi(2) - center[2];
        max_radius_all_in_domain = std::min(max_radius_all_in_domain,z);
#endif
        if (verbose && ParallelDescriptor::IOProcessor())
            std::cout << "Maximum radius for which the mass is contained in the domain: " 
                      << max_radius_all_in_domain << std::endl;
    }
#endif

    finest_level_allocated = level;
}

std::string Gravity::get_gravity_type()
{
  return gravity_type;
}

Real Gravity::get_const_grav()
{
  return const_grav;
}

int Gravity::NoSync()
{
  return no_sync;
}

int Gravity::NoComposite()
{
  return no_composite;
}

int Gravity::test_results_of_solves()
{
  return test_solves;
}

MultiFab* Gravity::get_phi_prev(int level)
{
  return &phi_prev[level];
}

MultiFab* Gravity::get_phi_curr(int level)
{
  return &phi_curr[level];
}

PArray<MultiFab>& 
Gravity::get_grad_phi_prev(int level)
{
  return grad_phi_prev[level];
}

MultiFab*
Gravity::get_grad_phi_prev_comp(int level, int comp)
{
  return &grad_phi_prev[level][comp];
}

PArray<MultiFab>& 
Gravity::get_grad_phi_curr(int level)
{
  return grad_phi_curr[level];
}

void 
Gravity::plus_phi_curr(int level, MultiFab& addend)
{
  phi_curr[level].plus(addend,0,1,0);
}

void 
Gravity::plus_grad_phi_curr(int level, PArray<MultiFab>& addend)
{
  for (int n = 0; n < BL_SPACEDIM; n++)
    grad_phi_curr[level][n].plus(addend[n],0,1,0);
}

void
Gravity::swapTimeLevels (int level)
{
  if (gravity_type == "PoissonGrav") {

     MultiFab* dummy = phi_curr.remove(level);
     phi_prev.clear(level);
     phi_prev.set(level,dummy);

     phi_curr.set(level,new MultiFab(grids[level],1,1));
     phi_curr[level].setVal(1.e50);

     for (int n=0; n < BL_SPACEDIM; n++) {
        MultiFab* dummy = grad_phi_curr[level].remove(n);
        grad_phi_prev[level].clear(n);
        grad_phi_prev[level].set(n,dummy);
   
        grad_phi_curr[level].set(n,new MultiFab(BoxArray(grids[level]).surroundingNodes(n),1,1));
        grad_phi_curr[level][n].setVal(1.e50);
     }
  } 
}

void
Gravity::zeroPhiFluxReg (int level)
{
  phi_flux_reg[level].setVal(0.);
}

void
Gravity::solve_for_old_phi (int               level,
                            MultiFab&         phi,
                            PArray<MultiFab>& grad_phi,
                            int               fill_interior)
{
    BL_PROFILE("Gravity::solve_for_old_phi()");

    Real time = LevelData[level].get_state_data(State_Type).prevTime();

    MultiFab Rhs(grids[level],1,0);

    // Put the gas density into the RHS
    MultiFab& S_old = LevelData[level].get_old_data(State_Type);
    MultiFab::Copy(Rhs,S_old,Density,0,1,0);

#ifdef PARTICLES
    // Add the particle density to the RHS
    if( Castro::theDMPC() )
        AddParticlesToRhs(level,Rhs,1);
#endif

    // This is a correction for fully periodic domains only
    if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
       std::cout << " ... subtracting average density from RHS in solve ... " << mass_offset << std::endl;
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi) 
       Rhs[mfi].plus(-mass_offset);

    solve_for_phi(level,Rhs,phi,grad_phi,time,fill_interior);
}


void
Gravity::solve_for_new_phi (int               level,
                            MultiFab&         phi,
                            PArray<MultiFab>& grad_phi,
                            int               fill_interior)
{
    BL_PROFILE("Gravity::solve_for_new_phi()");

    MultiFab& S_new = LevelData[level].get_new_data(State_Type);
    MultiFab Rhs(grids[level],1,0);
    MultiFab::Copy(Rhs,S_new,Density,0,1,0);

#ifdef PARTICLES
    if( Castro::theDMPC() )
        AddParticlesToRhs(level,Rhs,1);
#endif

    // This is a correction for fully periodic domains only
    if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
       std::cout << " ... subtracting average density from RHS in solve ... " << mass_offset << std::endl;
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi) 
       Rhs[mfi].plus(-mass_offset);
    
    Real time = LevelData[level].get_state_data(State_Type).curTime();

    solve_for_phi(level,Rhs,phi,grad_phi,time,fill_interior);
}

void
Gravity::solve_for_phi (int               level,
                        MultiFab&         Rhs,
                        MultiFab&         phi,
                        PArray<MultiFab>& grad_phi,
                        Real              time,
                        int               fill_interior)

{
    BL_PROFILE("Gravity::solve_for_phi()");

    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << " ... solve for phi at level " << level << std::endl;

    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);

    // This is a sanity check on whether we are trying to fill multipole boundary conditiosn
    //  for grids at this level > 0 -- this case is not currently supported. 
    //  Here we shrink the domain at this level by 1 in any direction which is not symmetry or periodic, 
    //  then ask if the grids at this level are contained in the shrunken domain.  If not, then grids
    //  at this level touch the domain boundary and we must abort.
    if (level > 0  && !Geometry::isAllPeriodic()) 
    {
      Box shrunk_domain(geom.Domain());
      for (int dir = 0; dir < BL_SPACEDIM; dir++)
      {
          if (!Geometry::isPeriodic(dir)) 
          {
              if (phys_bc->lo(dir) != Symmetry) 
                  shrunk_domain.growLo(dir,-1);
              if (phys_bc->hi(dir) != Symmetry) 
                  shrunk_domain.growHi(dir,-1);
          }
      }
      BoxArray shrunk_domain_ba(shrunk_domain);
      if (!shrunk_domain_ba.contains(phi.boxArray()))
         BoxLib::Error("Oops -- don't know how to set boundary conditions for grids at this level that touch the domain boundary!");
    }
      
    if (level == 0  && !Geometry::isAllPeriodic()) {
      if (verbose && ParallelDescriptor::IOProcessor()) 
         std::cout << " ... Making bc's for phi at level 0 and time "  << time << std::endl;

      // Fill the ghost cells using a multipole approximation. By default, lnum = 0
      // and a monopole approximation is used. Do this only if we are in 3D; otherwise,
      // default to the make_radial_phi approach, that integrates spherical shells of mass.
      // We can also do a brute force sum that explicitly calculates the potential
      // at each ghost zone by summing over all the cells in the domain.
#if (BL_SPACEDIM == 3)
      if ( direct_sum_bcs )
        fill_direct_sum_BCs(level,Rhs,phi);
      else
        fill_multipole_BCs(level,Rhs,phi);
#else
      make_radial_phi(level,Rhs,phi,fill_interior);
#endif

    }

    Rhs.mult(Ggravity);

    MacBndry bndry(grids[level],1,geom);

    IntVect crse_ratio = level > 0 ? parent->refRatio(level-1)
                                   : IntVect::TheZeroVector();

    //
    // Set Dirichlet boundary condition for phi in phi grow cells, use to 
    // initialize bndry.
    //
    const int src_comp  = 0;
    const int dest_comp = 0;
    const int num_comp  = 1;
    if (level == 0)
    {
        bndry.setBndryValues(phi,src_comp,dest_comp,num_comp,*phys_bc);
    }
    else
    {
        MultiFab CPhi;
        GetCrsePhi(level,CPhi,time);
        BoxArray crse_boxes = BoxArray(grids[level]).coarsen(crse_ratio);
        const int in_rad     = 0;
        const int out_rad    = 1;
        const int extent_rad = 2;
        BndryRegister crse_br(crse_boxes,in_rad,out_rad,extent_rad,num_comp);
        crse_br.copyFrom(CPhi,CPhi.nGrow(),src_comp,dest_comp,num_comp);
        bndry.setBndryValues(crse_br,src_comp,phi,src_comp,
                             dest_comp,num_comp,crse_ratio,*phys_bc);
    }

    std::vector<BoxArray> bav(1);
    bav[0] = phi.boxArray();
    std::vector<DistributionMapping> dmv(1);
    dmv[0] = Rhs.DistributionMap();
    std::vector<Geometry> fgeom(1);
    fgeom[0] = geom;

    MGT_Solver mgt_solver(fgeom, mg_bc, bav, dmv, false, stencil_type);

    Array< Array<Real> > xa(1);
    Array< Array<Real> > xb(1);

    xa[0].resize(BL_SPACEDIM);
    xb[0].resize(BL_SPACEDIM);

    if (level == 0) {
      for ( int i = 0; i < BL_SPACEDIM; ++i ) {
        xa[0][i] = 0.;
        xb[0][i] = 0.;
      }
    } else {
      const Real* dx_crse   = parent->Geom(level-1).CellSize();
      for ( int i = 0; i < BL_SPACEDIM; ++i ) {
        xa[0][i] = 0.5 * dx_crse[i];
        xb[0][i] = 0.5 * dx_crse[i];
      }
    }

#if (BL_SPACEDIM == 3)
    if ( (level == 0) && Geometry::isAllPeriodic() )
    {
       Real sum = computeAvg(level,&Rhs);

       const Real* dx = parent->Geom(0).CellSize();
       Real domain_vol = grids[0].d_numPts() * dx[0] * dx[1] * dx[2];

       sum = sum / domain_vol;
       Rhs.plus(-sum,0,1,0);

       if (verbose && ParallelDescriptor::IOProcessor()) 
          std::cout << " ... subtracting " << sum << " to ensure solvability " << std::endl;
    }
#endif

    MultiFab* phi_p[1];
    MultiFab* Rhs_p[1];

    phi_p[0] = &phi;
    Rhs_p[0] = &Rhs;

    // Need to do this even if Cartesian because the array is needed in set_gravity_coefficients
    Array< PArray<MultiFab> > coeffs(1);
    coeffs[0].resize(BL_SPACEDIM,PArrayManage);
    for (int i = 0; i < BL_SPACEDIM ; i++) {
        coeffs[0].set(i, new MultiFab);
        geom.GetFaceArea(coeffs[0][i],grids[level],i,0);
        coeffs[0][i].setVal(1.0);
    }

#if (BL_SPACEDIM < 3)
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
      applyMetricTerms(level,(*Rhs_p[0]),coeffs[0]);
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,0);
    } 
    else 
#endif
    {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,1);
    }

    Real     tol = sl_tol;
    Real abs_tol = 0.0;
    mgt_solver.solve(phi_p, Rhs_p, tol, abs_tol, bndry, 1, level_solver_resnorm[level]);
    
    int mglev = 0;
    const Real* dx   = geom.CellSize();
    mgt_solver.get_fluxes(mglev, grad_phi, dx);

#if (BL_SPACEDIM < 3)
//  Need to un-weight the fluxes
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
      unweight_edges(level, grad_phi);
#endif

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::solve_for_phi() time = " << end << std::endl;
    }
}


void
Gravity::solve_for_delta_phi (int                        crse_level,
                              int                        fine_level,
                              MultiFab&                  CrseRhs,
                              PArray<MultiFab>&          delta_phi,
                              PArray<PArray<MultiFab> >& grad_delta_phi)
{
    BL_PROFILE("Gravity::solve_delta_phi()");

    int nlevs = fine_level - crse_level + 1;
    BL_ASSERT(grad_delta_phi.size() == nlevs);
    BL_ASSERT(delta_phi.size() == nlevs);

    if (verbose && ParallelDescriptor::IOProcessor()) {
      std::cout << "... solving for delta_phi at crse_level = " << crse_level << std::endl;
      std::cout << "...                    up to fine_level = " << fine_level << std::endl;
    }

    const Geometry& geom = parent->Geom(crse_level);
    MacBndry bndry(grids[crse_level],1,geom);

    IntVect crse_ratio = crse_level > 0 ? parent->refRatio(crse_level-1)
                                        : IntVect::TheZeroVector();

    // Set homogeneous Dirichlet values for the solve.
    bndry.setHomogValues(*phys_bc,crse_ratio);

    std::vector<BoxArray> bav(nlevs);
    std::vector<DistributionMapping> dmv(nlevs);

    for (int lev = crse_level; lev <= fine_level; lev++) {
       bav[lev-crse_level] = grids[lev];
       MultiFab& S_new = LevelData[lev].get_new_data(State_Type);
       dmv[lev-crse_level] = S_new.DistributionMap();
    }
    std::vector<Geometry> fgeom(nlevs);
    for (int lev = crse_level; lev <= fine_level; lev++) 
      fgeom[lev-crse_level] = parent->Geom(lev);

    MGT_Solver mgt_solver(fgeom, mg_bc, bav, dmv, false, stencil_type);

    Array< Array<Real> > xa(nlevs);
    Array< Array<Real> > xb(nlevs);

    for (int lev = crse_level; lev <= fine_level; lev++) {
        xa[lev-crse_level].resize(BL_SPACEDIM);
        xb[lev-crse_level].resize(BL_SPACEDIM);
      if (lev == 0) { 
        for ( int i = 0; i < BL_SPACEDIM; ++i ) {
          xa[lev-crse_level][i] = 0.;
          xb[lev-crse_level][i] = 0.; 
        }    
      } else {
        const Real* dx_crse   = parent->Geom(lev-1).CellSize();
        for ( int i = 0; i < BL_SPACEDIM; ++i ) {
          xa[lev-crse_level][i] = 0.5 * dx_crse[i];
          xb[lev-crse_level][i] = 0.5 * dx_crse[i];
        }
      }
    }

    MultiFab** phi_p = new MultiFab*[nlevs];
    MultiFab** Rhs_p = new MultiFab*[nlevs];

    Array< PArray<MultiFab> > coeffs(nlevs);

    for (int lev = crse_level; lev <= fine_level; lev++)
    {
        phi_p[lev-crse_level] = delta_phi.remove(lev-crse_level); // Turn ctrl of ptr over for a moment
        phi_p[lev-crse_level]->setVal(0.);

        if (lev == crse_level) {
            Rhs_p[0] = &CrseRhs;
        } else {
            Rhs_p[lev-crse_level] = new MultiFab(grids[lev],1,0);
            Rhs_p[lev-crse_level]->setVal(0.0);
        }

       // Need to do this even if Cartesian because the array is needed in set_gravity_coefficients
       coeffs[lev-crse_level].resize(BL_SPACEDIM,PArrayManage);
       Geometry g = LevelData[lev].Geom();
       for (int i = 0; i < BL_SPACEDIM ; i++) {
           coeffs[lev-crse_level].set(i, new MultiFab);
           g.GetFaceArea(coeffs[lev-crse_level][i],grids[lev],i,0);
           coeffs[lev-crse_level][i].setVal(1.0);
       }

#if (BL_SPACEDIM < 3)
       if (Geometry::IsRZ() || Geometry::IsSPHERICAL())
          applyMetricTerms(lev,(*Rhs_p[lev-crse_level]),coeffs[lev-crse_level]);
#endif
    }

    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,0);
    } else {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,1);
    }

    Real     tol = delta_tol;
    Real abs_tol = level_solver_resnorm[crse_level];
    for (int lev = crse_level+1; lev < fine_level; lev++)
        abs_tol = std::max(abs_tol,level_solver_resnorm[lev]);
  
    Real final_resnorm = 0.0;
    mgt_solver.solve(phi_p, Rhs_p, tol, abs_tol, bndry, 1, final_resnorm);

    for (int lev = crse_level; lev <= fine_level; lev++)
    {
        PArray<MultiFab>& gdphi = grad_delta_phi[lev-crse_level];
        const Real* dx   = parent->Geom(lev).CellSize();
        mgt_solver.get_fluxes(lev-crse_level, gdphi, dx);

#if (BL_SPACEDIM < 3)
        if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
          unweight_edges(lev, gdphi);
#endif

    }

    for (int lev = 0; lev < nlevs; lev++)
    {
        delta_phi.set(lev,phi_p[lev]); // Return ctrl of ptr
        if (lev != 0)
            delete Rhs_p[lev]; // Do not delete the [0] Rhs, it is passed in
    }
    delete [] phi_p;
    delete [] Rhs_p;
}

void
Gravity::gravity_sync (int crse_level, int fine_level, 
                       const MultiFab& drho_and_drhoU, const MultiFab& dphi,
                       PArray<MultiFab>& grad_delta_phi_cc)
{
    BL_PROFILE("Gravity::gravity_sync()");

    BL_ASSERT(parent->finestLevel()>crse_level);
    if (verbose && ParallelDescriptor::IOProcessor()) {
          std::cout << " ... gravity_sync at crse_level " << crse_level << '\n';
          std::cout << " ...     up to finest_level     " << fine_level << '\n';
    }

    // Build Rhs for solve for delta_phi
    MultiFab CrseRhs(grids[crse_level],1,0);
    MultiFab::Copy(CrseRhs,drho_and_drhoU,0,0,1,0);
    CrseRhs.mult(Ggravity);
    CrseRhs.plus(dphi,0,1,0);

    const Geometry& crse_geom = parent->Geom(crse_level);
    const Box&    crse_domain = crse_geom.Domain();
    if (crse_geom.isAllPeriodic() && (grids[crse_level].numPts() == crse_domain.numPts()) ) {
       Real local_correction = 0.0;
       for (MFIter mfi(CrseRhs); mfi.isValid(); ++mfi)
           local_correction += CrseRhs[mfi].sum(mfi.validbox(),0,1);
       ParallelDescriptor::ReduceRealSum(local_correction);
       
       local_correction = local_correction / grids[crse_level].numPts();

       if (verbose && ParallelDescriptor::IOProcessor())
          std::cout << "WARNING: Adjusting RHS in gravity_sync solve by " << local_correction << std::endl;
       for (MFIter mfi(CrseRhs); mfi.isValid(); ++mfi) 
          CrseRhs[mfi].plus(-local_correction);
    }

    // delta_phi needs a ghost cell for the solve
    PArray<MultiFab>  delta_phi(fine_level-crse_level+1, PArrayManage);
    for (int lev = crse_level; lev <= fine_level; lev++) {
       delta_phi.set(lev-crse_level,new MultiFab(grids[lev],1,1));
       delta_phi[lev-crse_level].setVal(0.);
    }

    PArray<PArray<MultiFab> > ec_gdPhi(fine_level-crse_level+1, PArrayManage);
    for (int lev = crse_level; lev <= fine_level; lev++) {
       ec_gdPhi.set(lev-crse_level,new PArray<MultiFab>(BL_SPACEDIM,PArrayManage));
       for (int n=0; n<BL_SPACEDIM; ++n)  
          ec_gdPhi[lev-crse_level].set(n,new MultiFab(BoxArray(grids[lev]).surroundingNodes(n),1,0));
    }

    // Do multi-level solve for delta_phi
    solve_for_delta_phi(crse_level,fine_level,CrseRhs,delta_phi,ec_gdPhi);

    // In the all-periodic case we enforce that delta_phi averages to zero.
    if (crse_geom.isAllPeriodic() && (grids[crse_level].numPts() == crse_domain.numPts()) ) {
       Real local_correction = 0.0;
       for (MFIter mfi(delta_phi[0]); mfi.isValid(); ++mfi)
           local_correction += delta_phi[0][mfi].sum(mfi.validbox(),0,1);
       ParallelDescriptor::ReduceRealSum(local_correction);

       local_correction = local_correction / grids[crse_level].numPts();

       for (int lev = crse_level; lev <= fine_level; lev++) 
          for (MFIter mfi(delta_phi[lev-crse_level]); mfi.isValid(); ++mfi)
             delta_phi[lev-crse_level][mfi].plus(-local_correction);
    }

    // Add delta_phi to phi_curr, and grad(delta_phi) to grad(delta_phi_curr) on each level
    for (int lev = crse_level; lev <= fine_level; lev++) {
       phi_curr[lev].plus(delta_phi[lev-crse_level],0,1,0);
       for (int n = 0; n < BL_SPACEDIM; n++) 
          grad_phi_curr[lev][n].plus(ec_gdPhi[lev-crse_level][n],0,1,0);
    }

    int is_new = 1;

    // Average phi_curr from fine to coarse level
    for (int lev = fine_level-1; lev >= crse_level; lev--)
    {
       const IntVect ratio = parent->refRatio(lev);
       avgDown(phi_curr[lev],phi_curr[lev+1],ratio);
    } 

    // Average the edge-based grad_phi from finer to coarser level
    for (int lev = fine_level-1; lev >= crse_level; lev--)
       average_fine_ec_onto_crse_ec(lev,is_new);

    // Add the contribution of grad(delta_phi) to the flux register below if necessary.
    if (crse_level > 0)
    {
        for (MFIter mfi(delta_phi[0]); mfi.isValid(); ++mfi) 
            for (int n=0; n<BL_SPACEDIM; ++n)
               phi_flux_reg[crse_level].FineAdd(ec_gdPhi[0][n][mfi],n,mfi.index(),0,0,1,1);
    }

    int lo_bc[BL_SPACEDIM];
    int hi_bc[BL_SPACEDIM];
    for (int dir = 0; dir < BL_SPACEDIM; dir++) {
      lo_bc[dir] = (phys_bc->lo(dir) == Symmetry); 
      hi_bc[dir] = (phys_bc->hi(dir) == Symmetry); 
    }
    int symmetry_type = Symmetry;

    int coord_type = Geometry::Coord();

    for (int lev = crse_level; lev <= fine_level; lev++) {

       const Real* problo = parent->Geom(lev).ProbLo();

       MultiFab& S = LevelData[lev].get_new_data(State_Type);
    
       grad_delta_phi_cc[lev-crse_level].setVal(0.0);

       const Real* dx = parent->Geom(lev).CellSize();

       for (MFIter mfi(S); mfi.isValid(); ++mfi)
       {
          int index = mfi.index();
          const Box& bx = grids[lev][index];

          // Average edge-centered gradients of crse dPhi to cell centers
          BL_FORT_PROC_CALL(CA_AVG_EC_TO_CC,ca_avg_ec_to_cc)
              (bx.loVect(), bx.hiVect(),
               lo_bc, hi_bc, &symmetry_type,
               BL_TO_FORTRAN(grad_delta_phi_cc[lev-crse_level][mfi]),
               D_DECL(BL_TO_FORTRAN(ec_gdPhi[lev-crse_level][0][mfi]),
                      BL_TO_FORTRAN(ec_gdPhi[lev-crse_level][1][mfi]),
                      BL_TO_FORTRAN(ec_gdPhi[lev-crse_level][2][mfi])),
                      dx,problo,&coord_type);
       }
    }
}

void
Gravity::GetCrsePhi(int level,
                    MultiFab& phi_crse,
                    Real      time      )
{
    BL_ASSERT(level!=0);

    const Real t_old = LevelData[level-1].get_state_data(State_Type).prevTime();
    const Real t_new = LevelData[level-1].get_state_data(State_Type).curTime();
    Real alpha = (time - t_old)/(t_new - t_old);

    phi_crse.clear();
    phi_crse.define(grids[level-1], 1, 1, Fab_allocate); // BUT NOTE we don't trust phi's ghost cells.
    FArrayBox PhiCrseTemp;
    for (MFIter mfi(phi_crse); mfi.isValid(); ++mfi)
    {
       PhiCrseTemp.resize(phi_crse[mfi].box(),1);

       PhiCrseTemp.copy(phi_prev[level-1][mfi]);
       Real omalpha = 1.0 - alpha;
       PhiCrseTemp.mult(omalpha);

       phi_crse[mfi].copy(phi_curr[level-1][mfi]);
       phi_crse[mfi].mult(alpha);
       phi_crse[mfi].plus(PhiCrseTemp);
    }

    phi_crse.FillBoundary();

    const Geometry& geom = parent->Geom(level-1);
    geom.FillPeriodicBoundary(phi_crse,true);
}

void
Gravity::GetCrseGradPhi(int level,
                        PArray<MultiFab>& grad_phi_crse,
                        Real              time          ) 
{
    BL_ASSERT(level!=0);

    const Real t_old = LevelData[level-1].get_state_data(State_Type).prevTime();
    const Real t_new = LevelData[level-1].get_state_data(State_Type).curTime();
    Real alpha = (time - t_old)/(t_new - t_old);

    BL_ASSERT(grad_phi_crse.size() == BL_SPACEDIM);
    for (int i=0; i<BL_SPACEDIM; ++i)
    {
        BL_ASSERT(!grad_phi_crse.defined(i));
        const BoxArray eba = BoxArray(grids[level-1]).surroundingNodes(i);
        grad_phi_crse.set(i,new MultiFab(eba, 1, 0));
        FArrayBox GradPhiCrseTemp;
        for (MFIter mfi(grad_phi_crse[i]); mfi.isValid(); ++mfi)
        {
            GradPhiCrseTemp.resize(mfi.validbox(),1);
            
            GradPhiCrseTemp.copy(grad_phi_prev[level-1][i][mfi]);
            Real omalpha = 1.0 - alpha;
            GradPhiCrseTemp.mult(omalpha);
            
            grad_phi_crse[i][mfi].copy(grad_phi_curr[level-1][i][mfi]);
            grad_phi_crse[i][mfi].mult(alpha);
            grad_phi_crse[i][mfi].plus(GradPhiCrseTemp);
        }
        grad_phi_crse[i].FillBoundary();

        const Geometry& geom = parent->Geom(level-1);
        geom.FillPeriodicBoundary(grad_phi_crse[i],false);
    }
}

void
Gravity::multilevel_solve_for_new_phi (int level, int finest_level, int use_previous_phi_as_guess)
{
    BL_PROFILE("Gravity::multilevel_solve_for_new_phi()");

    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << "... multilevel solve for new phi at base level " << level << " to finest level " << finest_level << std::endl;

    for (int lev = level; lev <= finest_level; lev++) {
       BL_ASSERT(grad_phi_curr[lev].size()==BL_SPACEDIM);
       for (int n=0; n<BL_SPACEDIM; ++n)
       {
           grad_phi_curr[lev].clear(n);
           const BoxArray eba = BoxArray(grids[lev]).surroundingNodes(n);
           grad_phi_curr[lev].set(n,new MultiFab(eba,1,1));
       }
    }

    int is_new = 1;
    actual_multilevel_solve(level,finest_level,phi_curr,grad_phi_curr, is_new, use_previous_phi_as_guess);
}

void
Gravity::multilevel_solve_for_old_phi (int level, int finest_level, int use_previous_phi_as_guess)
{
    if (finest_level > 0) 
       BL_ASSERT(parent->subCycle()==0);

    if (verbose && ParallelDescriptor::IOProcessor())
      std::cout << "... multilevel solve for old phi at base level " << level << " to finest level " << finest_level << std::endl;

    for (int lev = level; lev <= finest_level; lev++) {
       BL_ASSERT(grad_phi_prev[lev].size()==BL_SPACEDIM);
       for (int n=0; n<BL_SPACEDIM; ++n)
       {
           grad_phi_prev[lev].clear(n);
           const BoxArray eba = BoxArray(grids[lev]).surroundingNodes(n);
           grad_phi_prev[lev].set(n,new MultiFab(eba,1,1));
       }
    }

    int is_new = 0;
    actual_multilevel_solve(level,finest_level,phi_prev,grad_phi_prev, is_new, use_previous_phi_as_guess);
}

void
Gravity::multilevel_solve_for_phi (int level, int finest_level, int use_previous_phi_as_guess)
{
    multilevel_solve_for_new_phi (level, finest_level);
}

void
Gravity::actual_multilevel_solve (int level, int finest_level, 
                                  PArray<MultiFab>& phi, 
                                  Array<PArray<MultiFab> >& grad_phi, int is_new, 
                                  int use_previous_phi_as_guess)
{
    BL_PROFILE("Gravity::actual_multilevel_solve()");

    int nlevs = finest_level-level+1;

    std::vector<BoxArray> bav(nlevs);
    std::vector<DistributionMapping> dmv(nlevs);

    // Ok to use S_new here because S_new and S_old have the same DistributionMap
    for (int lev = 0; lev < nlevs; lev++) {
       bav[lev] = grids[level+lev];
       MultiFab& S_new = LevelData[level+lev].get_new_data(State_Type);
       dmv[lev] = S_new.DistributionMap();
    }
    std::vector<Geometry> fgeom(nlevs);
    for (int i = 0; i < nlevs; i++) 
      fgeom[i] = parent->Geom(level+i);

    MGT_Solver mgt_solver(fgeom, mg_bc, bav, dmv, false, stencil_type);
    mgt_solver.set_maxorder(3);
    
    Array< Array<Real> > xa(nlevs);
    Array< Array<Real> > xb(nlevs);

    for (int lev = 0; lev < nlevs; lev++) 
    {
        xa[lev].resize(BL_SPACEDIM);
        xb[lev].resize(BL_SPACEDIM);
        if (level+lev == 0) {
           for ( int i = 0; i < BL_SPACEDIM; ++i ) {
             xa[lev][i] = 0.;
             xb[lev][i] = 0.;
           }
        } else {
           const Real* dx_crse   = parent->Geom(level+lev-1).CellSize();
           for ( int i = 0; i < BL_SPACEDIM; ++i ) {
             xa[lev][i] = 0.5 * dx_crse[i];
             xb[lev][i] = 0.5 * dx_crse[i];
           } 
        }
    }

    MultiFab** phi_p = new MultiFab*[nlevs];
    MultiFab** Rhs_p = new MultiFab*[nlevs];

    Array< PArray<MultiFab> > coeffs(nlevs);

#ifdef PARTICLES
    PArray<MultiFab> Rhs_particles(nlevs,PArrayManage);
    if ( Castro::theDMPC() )
    {
        for (int lev = 0; lev < nlevs; lev++)
        {
           Rhs_particles.set(lev, new MultiFab(grids[level+lev], 1, 0));
           Rhs_particles[lev].setVal(0.);
        }
        AddParticlesToRhs(level,finest_level,Rhs_particles);
    }
#endif
     
//  **********************************************************************************************

    for (int lev = 0; lev < nlevs; lev++) 
    {
       phi_p[lev] = &phi[level+lev];         // Working in result data structure directly
       if (!use_previous_phi_as_guess)
          phi_p[lev]->setVal(0.);

       Rhs_p[lev] = new MultiFab(grids[level+lev],1,0);

       if (is_new == 1) {
          MultiFab::Copy(*(Rhs_p[lev]),LevelData[level+lev].get_new_data(State_Type),Density,0,1,0);
       } else if (is_new == 0) {
          MultiFab::Copy(*(Rhs_p[lev]),LevelData[level+lev].get_old_data(State_Type),Density,0,1,0);
       }

#ifdef PARTICLES
       if( Castro::theDMPC() ){
          MultiFab::Add(*(Rhs_p[lev]),Rhs_particles[lev],0,0,1,0);
       }
#endif

       // Need to do this even if Cartesian because the array is needed in set_gravity_coefficients
       coeffs[lev].resize(BL_SPACEDIM,PArrayManage);
       Geometry g = LevelData[level+lev].Geom();
       for (int i = 0; i < BL_SPACEDIM ; i++) {
           coeffs[lev].set(i, new MultiFab);
           g.GetFaceArea(coeffs[lev][i],grids[level+lev],i,0);
           coeffs[lev][i].setVal(1.0);
       }

       if ( (level == 0) && (lev == 0) && !Geometry::isAllPeriodic() ) 
       {
	   if (verbose && ParallelDescriptor::IOProcessor()) 
	       std::cout << " ... Making bc's for phi at level 0 " << std::endl;

	   int fill_interior = 1;
	   make_radial_phi(0,*(Rhs_p[0]),*(phi_p[0]),fill_interior);
#if (BL_SPACEDIM == 3)
	   if ( direct_sum_bcs )
               fill_direct_sum_BCs(0,*(Rhs_p[0]),*(phi_p[0]));
	   else
               // Note that the ghost cells of phi are zero'd out before being filled
               //      so the previous values from make_radial_phi will be forgotten
               fill_multipole_BCs(0,*(Rhs_p[0]),*(phi_p[0]));
#endif
       }
    }
     
//  **********************************************************************************************

#if (BL_SPACEDIM == 3)
    if ( Geometry::isAllPeriodic() )
    {
       Real sum = 0;
       for (int lev = 0; lev < nlevs; lev++) 
          sum += computeAvg(lev,Rhs_p[lev]);

       const Real* dx = parent->Geom(0).CellSize();
       Real domain_vol = grids[0].d_numPts() * dx[0] * dx[1] * dx[2];

       sum = sum / domain_vol;
//     if (verbose && ParallelDescriptor::IOProcessor()) 
//        std::cout << " ... current avg vs mass_offset " << sum << " " << mass_offset
//                  << " ... diff is " << (sum-mass_offset) <<  std::endl;

       Real eps = 1.e-10 * std::abs(mass_offset);
       if (std::abs(sum - mass_offset) > eps)
       {
          if (ParallelDescriptor::IOProcessor()) 
          {
              std::cout << " ... current avg vs mass_offset " << sum << " " << mass_offset
                        << " ... diff is " << (sum-mass_offset) <<  std::endl;
              std::cout << " ... Gravity::actual_multilevel_solve -- total mass has changed!" << std::endl;;
          }
//        BoxLib::Error("Gravity::actual_multilevel_solve -- total mass has changed!");
       }

       if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
          std::cout << " ... subtracting average density " << mass_offset << 
                       " from RHS at each level " << std::endl;

       for (int lev = 0; lev < nlevs; lev++) 
          for (MFIter mfi(*(Rhs_p[lev])); mfi.isValid(); ++mfi) 
             (*Rhs_p[lev])[mfi].plus(-mass_offset);
    }
#endif
     
//  **********************************************************************************************

    for (int lev = 0; lev < nlevs; lev++) 
    {
       // Multiply by G
       Rhs_p[lev]->mult(Ggravity,0,1);

#if (BL_SPACEDIM < 3)
       // Adjust by metric terms
       if (Geometry::IsRZ() || Geometry::IsSPHERICAL())
          applyMetricTerms(level+lev,(*Rhs_p[lev]),coeffs[lev]);
#endif
    }
     
//  **********************************************************************************************

#if (BL_SPACEDIM == 3)
    if (Geometry::isAllPeriodic() )
    {
       Real sum = 0;
       for (int lev = 0; lev < nlevs; lev++) 
          sum += computeAvg(lev,Rhs_p[lev]);

       const Real* dx = parent->Geom(0).CellSize();
       Real domain_vol = grids[0].d_numPts() * dx[0] * dx[1] * dx[2];
       sum = sum / domain_vol;

       if (verbose && ParallelDescriptor::IOProcessor()) 
          std::cout << " ... subtracting " << sum << " to ensure solvability " << std::endl;
   
       for (int lev = 0; lev < nlevs; lev++)  
          (*Rhs_p[lev]).plus(-sum,0,1,0);
    }
#endif

    IntVect crse_ratio = level > 0 ? parent->refRatio(level-1)
                                   : IntVect::TheZeroVector();

    //
    // Store the Dirichlet boundary condition for phi in bndry.
    //
    const Geometry& geom = parent->Geom(level);
    MacBndry bndry(grids[level],1,geom);
    const int src_comp  = 0;
    const int dest_comp = 0;
    const int num_comp  = 1;
    //
    // Build the homogeneous boundary conditions.  One could setVal
    // the bndry fabsets directly, but we instead do things as if
    // we had a fill-patched mf with grows--in that case the bndry
    // object knows how to grab grow data from the mf on physical 
    // boundarys.  Here we creat an mf, setVal, and pass that to 
    // the bndry object.
    //
    if (level == 0)
    {
//      bndry.setHomogValues(*phys_bc,crse_ratio);
        bndry.setBndryValues(*(phi_p[0]),src_comp,dest_comp,num_comp,*phys_bc);
    }
    else
    {
        MultiFab CPhi;
        Real cur_time = LevelData[level].get_state_data(State_Type).curTime();
        GetCrsePhi(level,CPhi,cur_time);
        BoxArray crse_boxes = BoxArray(grids[level]).coarsen(crse_ratio);
        const int in_rad     = 0;
        const int out_rad    = 1;
        const int extent_rad = 2;
        BndryRegister crse_br(crse_boxes,in_rad,out_rad,extent_rad,num_comp);
        crse_br.copyFrom(CPhi,CPhi.nGrow(),src_comp,dest_comp,num_comp);

        bndry.setBndryValues(crse_br,src_comp,phi_curr[level],src_comp,
                             dest_comp,num_comp,crse_ratio,*phys_bc);
    }

    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,0);
    } else {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,1);
    }

    Real     tol = ml_tol;
    Real abs_tol = 0.0;

    Real final_resnorm = 0.0;
    mgt_solver.solve(phi_p, Rhs_p, tol, abs_tol, bndry, 1, final_resnorm);

    for (int lev = 0; lev < nlevs; lev++) {
      const Real* dx   = parent->Geom(level+lev).CellSize();
      mgt_solver.get_fluxes(lev, grad_phi[level+lev], dx);

#if (BL_SPACEDIM < 3)
//    Need to un-weight the fluxes
      if (Geometry::IsSPHERICAL() || Geometry::IsRZ())
        unweight_edges(level+lev, grad_phi[level+lev]);
#endif
    }

    // Average phi from fine to coarse level
    for (int lev = finest_level; lev > level; lev--)
    {
       const IntVect ratio = parent->refRatio(lev-1);
       if (is_new == 1)
       {
           avgDown(phi_curr[lev-1],phi_curr[lev],ratio);
       }
       else if (is_new == 0)
       {
           avgDown(phi_prev[lev-1],phi_prev[lev],ratio);
       }

    }

    // Average grad_phi from fine to coarse level
    for (int lev = finest_level; lev > level; lev--) 
       average_fine_ec_onto_crse_ec(lev-1,is_new);

    for (int lev = 0; lev < nlevs; lev++) 
       delete Rhs_p[lev];

    delete [] phi_p;
    delete [] Rhs_p;
}

void
Gravity::get_old_grav_vector(int level, MultiFab& grav_vector, Real time)
{
    BL_PROFILE("Gravity::get_old_grav_vector()");

    int ng = grav_vector.nGrow();

    if (gravity_type == "ConstantGrav") {

       // Set to constant value in the BL_SPACEDIM direction
       grav_vector.setVal(0.0       ,0            ,BL_SPACEDIM-1,ng);
       grav_vector.setVal(const_grav,BL_SPACEDIM-1,            1,ng);

    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {
 
#if (BL_SPACEDIM == 1)
       make_one_d_grav(level,time,grav_vector);
#else

       if (gravity_type == "MonopoleGrav") 
       {
          const Real prev_time = LevelData[level].get_state_data(State_Type).prevTime();
          make_radial_gravity(level,prev_time,radial_grav_old[level]);
          interpolate_monopole_grav(level,radial_grav_old[level],grav_vector);

       }
       else if (gravity_type == "PrescribedGrav") 
       {
          make_prescribed_grav(level,time,grav_vector);
       }  

#endif 
    } else if (gravity_type == "PoissonGrav") {

       // Set to zero to fill ghost cells.
       grav_vector.setVal(0.);

       // Fill grow cells in grad_phi, will need to compute grad_phi_cc in 1 grow cell
       const Geometry& geom = parent->Geom(level);
       if (level==0)
       {
             for (int i = 0; i < BL_SPACEDIM ; i++)
             {
                 grad_phi_prev[level][i].setBndry(0.0);
                 grad_phi_prev[level][i].FillBoundary();
                 geom.FillPeriodicBoundary(grad_phi_prev[level][i]);
             }
 
       } else {
 
             PArray<MultiFab> crse_grad_phi(BL_SPACEDIM,PArrayManage);
             GetCrseGradPhi(level,crse_grad_phi,time);
             fill_ec_grow(level,grad_phi_prev[level],crse_grad_phi);
       }
 
       int lo_bc[BL_SPACEDIM];
       int hi_bc[BL_SPACEDIM];
       for (int dir = 0; dir < BL_SPACEDIM; dir++) {
         lo_bc[dir] = phys_bc->lo(dir);
         hi_bc[dir] = phys_bc->hi(dir);
       }
       int symmetry_type = Symmetry;

       int coord_type = Geometry::Coord();
       const Real*     dx = parent->Geom(level).CellSize();
       const Real* problo = parent->Geom(level).ProbLo();

       // Average edge-centered gradients to cell centers, including grow cells
       //   Grow cells are filled either by physical bc's in AVG_EC_TO_CC or
       //   by FillBoundary call to grav_vector afterwards.
       for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi) {
 
           int i = mfi.index();
           const Box& bx = grids[level][i];
 
           BL_FORT_PROC_CALL(CA_AVG_EC_TO_CC,ca_avg_ec_to_cc)
               (bx.loVect(), bx.hiVect(),
                lo_bc, hi_bc, &symmetry_type,
                BL_TO_FORTRAN(grav_vector[i]),
                D_DECL(BL_TO_FORTRAN(grad_phi_prev[level][0][i]),
                       BL_TO_FORTRAN(grad_phi_prev[level][1][i]),
                       BL_TO_FORTRAN(grad_phi_prev[level][2][i])),
                       dx,problo,&coord_type);
       }
       grav_vector.FillBoundary();
       geom.FillPeriodicBoundary(grav_vector,0,BL_SPACEDIM);
 
    } else {
       BoxLib::Abort("Unknown gravity_type in get_old_grav_vector");
    }
 
    MultiFab& G_old = LevelData[level].get_old_data(Gravity_Type);
 
    // Fill G_old from grav_vector
    MultiFab::Copy(G_old,grav_vector,0,0,BL_SPACEDIM,0);

#if (BL_SPACEDIM > 1)
    if (gravity_type != "ConstantGrav") {
 
       // This is a hack-y way to fill the ghost cell values of grav_vector
       //   before returning it
       AmrLevel* amrlev = &parent->getLevel(level) ;

       for (FillPatchIterator fpi(*amrlev,G_old,ng,time,Gravity_Type,0,BL_SPACEDIM); 
         fpi.isValid(); ++fpi) 
         {
            int i = fpi.index();
            grav_vector[i].copy(fpi());
         }
    }
#endif

#ifdef POINTMASS
    Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(level));
    Real point_mass = cs->get_point_mass();
    add_pointmass_to_gravity(level,grav_vector,point_mass);
#endif
}

void
Gravity::get_new_grav_vector(int level, MultiFab& grav_vector, Real time)
{
    BL_PROFILE("Gravity::get_new_grav_vector()");

    int ng = grav_vector.nGrow();

    if (gravity_type == "ConstantGrav") {

       // Set to constant value in the BL_SPACEDIM direction
       grav_vector.setVal(0.0       ,            0,BL_SPACEDIM-1,ng);
       grav_vector.setVal(const_grav,BL_SPACEDIM-1,            1,ng);

    } else if (gravity_type == "MonopoleGrav" || gravity_type == "PrescribedGrav") {

#if (BL_SPACEDIM == 1)
       make_one_d_grav(level,time,grav_vector);
#else

       // We always fill radial_grav_new (at every level)
       if (gravity_type == "MonopoleGrav")
       {
          const Real cur_time = LevelData[level].get_state_data(State_Type).curTime();
          make_radial_gravity(level,cur_time,radial_grav_new[level]);
          interpolate_monopole_grav(level,radial_grav_new[level],grav_vector);
       }
       else if (gravity_type == "PrescribedGrav") 
       {
          make_prescribed_grav(level,time,grav_vector);
       }
#endif

    } else if (gravity_type == "PoissonGrav") {

       // Set to zero to fill ghost cells
       grav_vector.setVal(0.);

      // Fill grow cells in grad_phi, will need to compute grad_phi_cc in 1 grow cell
      const Geometry& geom = parent->Geom(level);
      if (level==0)
      {
            for (int i = 0; i < BL_SPACEDIM ; i++)
            {
                grad_phi_curr[level][i].setBndry(0.0);
                grad_phi_curr[level][i].FillBoundary();
                geom.FillPeriodicBoundary(grad_phi_curr[level][i]);
            }

      } else {

            PArray<MultiFab> crse_grad_phi(BL_SPACEDIM,PArrayManage);
            GetCrseGradPhi(level,crse_grad_phi,time);
            fill_ec_grow(level,grad_phi_curr[level],crse_grad_phi);
      }

       int lo_bc[BL_SPACEDIM];
       int hi_bc[BL_SPACEDIM];
       for (int dir = 0; dir < BL_SPACEDIM; dir++) {
         lo_bc[dir] = phys_bc->lo(dir);
         hi_bc[dir] = phys_bc->hi(dir);
       }
       int symmetry_type = Symmetry;

       int coord_type = Geometry::Coord();
       const Real*     dx = parent->Geom(level).CellSize();
       const Real* problo = parent->Geom(level).ProbLo();

      // Average edge-centered gradients to cell centers, including grow cells
      //   Grow cells are filled either by physical bc's in AVG_EC_TO_CC or
      //   by FillBoundary call to grav_vector afterwards.
       for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi)
       {
          int i = mfi.index();
          const Box& bx = grids[level][i];

          BL_FORT_PROC_CALL(CA_AVG_EC_TO_CC,ca_avg_ec_to_cc)
              (bx.loVect(), bx.hiVect(),
               lo_bc, hi_bc, &symmetry_type,
               BL_TO_FORTRAN(grav_vector[i]),
               D_DECL(BL_TO_FORTRAN(grad_phi_curr[level][0][i]),
                      BL_TO_FORTRAN(grad_phi_curr[level][1][i]),
                      BL_TO_FORTRAN(grad_phi_curr[level][2][i])),
                      dx,problo,&coord_type);
       }
       grav_vector.FillBoundary();
       geom.FillPeriodicBoundary(grav_vector,0,BL_SPACEDIM);

    } else {
       BoxLib::Abort("Unknown gravity_type in get_new_grav_vector");
    }

    MultiFab& G_new = LevelData[level].get_new_data(Gravity_Type);

    // Fill G_new from grav_vector
    MultiFab::Copy(G_new,grav_vector,0,0,BL_SPACEDIM,0);

#if (BL_SPACEDIM > 1)
    if (gravity_type != "ConstantGrav") {

       // This is a hack-y way to fill the ghost cell values of grav_vector
       //   before returning it
       AmrLevel* amrlev = &parent->getLevel(level) ;

       for (FillPatchIterator fpi(*amrlev,G_new,ng,time,Gravity_Type,0,BL_SPACEDIM);
         fpi.isValid(); ++fpi)
         {
            int i = fpi.index();
            grav_vector[i].copy(fpi());
         }
    }
#endif

#ifdef POINTMASS
    Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(level));
    Real point_mass = cs->get_point_mass();
    add_pointmass_to_gravity(level,grav_vector,point_mass);
#endif
}

void
Gravity::test_level_grad_phi_prev(int level)
{
    BL_PROFILE("Gravity::test_level_grad_phi_prev()");

    // Fill the RHS for the solve
    MultiFab& S_old = LevelData[level].get_old_data(State_Type);
    MultiFab Rhs(grids[level],1,0);
    MultiFab::Copy(Rhs,S_old,Density,0,1,0);

    // This is a correction for fully periodic domains only
    if ( Geometry::isAllPeriodic() )
    {
       if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
          std::cout << " ... subtracting average density from RHS at level ... " 
                    << level << " " << mass_offset << std::endl;
       for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
          Rhs[mfi].plus(-mass_offset);
    }

    Rhs.mult(Ggravity);

    if (verbose) {
       Real rhsnorm = Rhs.norm0();
       if (ParallelDescriptor::IOProcessor()) {
          std::cout << "... test_level_grad_phi_prev at level " << level << std::endl;
          std::cout << "       norm of RHS             " << rhsnorm << std::endl;
       }
    }

    const Real* dx     = parent->Geom(level).CellSize();
    const Real* problo = parent->Geom(level).ProbLo();
    int coord_type     = Geometry::Coord();

    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        const Box bx = mfi.validbox();
        // Test whether using the edge-based gradients
        //   to compute Div(Grad(Phi)) satisfies Lap(phi) = RHS
        // Fill the RHS array with the residual
        BL_FORT_PROC_CALL(CA_TEST_RESIDUAL,ca_test_residual)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(grad_phi_prev[level][0][mfi]),
                    BL_TO_FORTRAN(grad_phi_prev[level][1][mfi]),
                    BL_TO_FORTRAN(grad_phi_prev[level][2][mfi])),
                    dx,problo,&coord_type);
    }
    if (verbose) {
       Real resnorm = Rhs.norm0();
//     Real gppxnorm = grad_phi_prev[level][0].norm0();
#if (BL_SPACEDIM > 1)
//     Real gppynorm = grad_phi_prev[level][1].norm0();
#endif
#if (BL_SPACEDIM > 2)
//     Real gppznorm = grad_phi_prev[level][2].norm0();
#endif
      if (ParallelDescriptor::IOProcessor())
        std::cout << "       norm of residual        " << resnorm << std::endl;
//      std::cout << "       norm of grad_phi_prev_x " << gppxnorm << std::endl;
#if (BL_SPACEDIM > 1)
//      std::cout << "       norm of grad_phi_prev_y " << gppynorm << std::endl;
#endif
#if (BL_SPACEDIM > 2)
//      std::cout << "       norm of grad_phi_prev_z " << gppznorm << std::endl;
#endif
    }
}

void
Gravity::test_level_grad_phi_curr(int level)
{
    BL_PROFILE("Gravity::test_level_grad_phi_curr()");

    // Fill the RHS for the solve
    MultiFab& S_new = LevelData[level].get_new_data(State_Type);
    MultiFab Rhs(grids[level],1,0);
    MultiFab::Copy(Rhs,S_new,Density,0,1,0);

    // This is a correction for fully periodic domains only
    if ( Geometry::isAllPeriodic() )
    {
       if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
          std::cout << " ... subtracting average density from RHS in solve ... " << mass_offset << std::endl;
       for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
          Rhs[mfi].plus(-mass_offset);
    }

    Rhs.mult(Ggravity);

    if (verbose) {
       Real rhsnorm = Rhs.norm0();
       if (ParallelDescriptor::IOProcessor()) {
          std::cout << "... test_level_grad_phi_curr at level " << level << std::endl;
          std::cout << "       norm of RHS             " << rhsnorm << std::endl;
        }
    }

    const Real*     dx = parent->Geom(level).CellSize();
    const Real* problo = parent->Geom(level).ProbLo();
    int coord_type     = Geometry::Coord();

    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        const Box bx = mfi.validbox();
        // Test whether using the edge-based gradients
        //   to compute Div(Grad(Phi)) satisfies Lap(phi) = RHS
        // Fill the RHS array with the residual
        BL_FORT_PROC_CALL(CA_TEST_RESIDUAL,ca_test_residual)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(grad_phi_curr[level][0][mfi]),
                    BL_TO_FORTRAN(grad_phi_curr[level][1][mfi]),
                    BL_TO_FORTRAN(grad_phi_curr[level][2][mfi])),
                    dx,problo,&coord_type);
    }
    if (verbose) {
       Real resnorm = Rhs.norm0();
//     Real gppxnorm = grad_phi_curr[level][0].norm0();
#if (BL_SPACEDIM > 1)
//     Real gppynorm = grad_phi_curr[level][1].norm0();
#endif
#if (BL_SPACEDIM > 2)
//     Real gppznorm = grad_phi_curr[level][2].norm0();
#endif
       if (ParallelDescriptor::IOProcessor())
          std::cout << "       norm of residual        " << resnorm << std::endl;
//        std::cout << "       norm of grad_phi_curr_x " << gppxnorm << std::endl;
#if (BL_SPACEDIM > 1)
//        std::cout << "       norm of grad_phi_curr_y " << gppynorm << std::endl;
#endif
#if (BL_SPACEDIM > 2)
//        std::cout << "       norm of grad_phi_curr_z " << gppznorm << std::endl;
#endif
    }
}

void 
Gravity::create_comp_minus_level_grad_phi(int level, MultiFab& comp_minus_level_phi,
                                          PArray<MultiFab>& comp_minus_level_grad_phi) 
{
    BL_PROFILE("Gravity::create_comp_minus_level_grad_phi()");

    MultiFab SL_phi;
    PArray<MultiFab> SL_grad_phi(BL_SPACEDIM,PArrayManage);

    SL_phi.define(grids[level],1,1,Fab_allocate);
    SL_phi.setVal(0.);

    comp_minus_level_phi.setVal(0.);
    for (int n=0; n<BL_SPACEDIM; ++n)
      comp_minus_level_grad_phi[n].setVal(0.);

    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        SL_grad_phi.clear(n);
        SL_grad_phi.set(n,new MultiFab(BoxArray(grids[level]).surroundingNodes(n),1,0));
        SL_grad_phi[n].setVal(0.);
    }

    // Do level solve at beginning of time step in order to compute the
    //   difference between the multilevel and the single level solutions.

    int fill_interior = 1;
#ifdef PARTICLES
    BoxLib::Error("Particles + Gravity + AMR: here be dragons... ( Gravity.cpp Gravity::create_comp_minus_level_grad_phi() )");
#endif
    solve_for_old_phi(level,SL_phi,SL_grad_phi,fill_interior);

    if (verbose && ParallelDescriptor::IOProcessor())  
       std::cout << "... compute difference between level and composite solves at level " << level << '\n';

    comp_minus_level_phi.copy(phi_prev[level],0,0,1);
    comp_minus_level_phi.minus(SL_phi,0,1,0);

    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        comp_minus_level_grad_phi[n].copy(grad_phi_prev[level][n],0,0,1);
        comp_minus_level_grad_phi[n].minus(SL_grad_phi[n],0,1,0);
    }

    // Just do this to release the memory
    for (int n=0; n<BL_SPACEDIM; ++n) SL_grad_phi.clear(n);
}

void
Gravity::add_to_fluxes(int level, int iteration, int ncycle)
{
    BL_PROFILE("Gravity::add_to_fluxes()");

    int finest_level = parent->finestLevel();
    FluxRegister* phi_fine = (level<finest_level ? &phi_flux_reg[level+1] : 0);
    FluxRegister* phi_current = (level>0 ? &phi_flux_reg[level] : 0);

    if (phi_fine) {

        for (int n=0; n<BL_SPACEDIM; ++n) {

            BoxArray ba = grids[level];
            ba.surroundingNodes(n);
            MultiFab fluxes(ba, 1, 0);

            for (MFIter mfi(phi_curr[level]); mfi.isValid(); ++mfi)
            {
                FArrayBox& gphi_flux = fluxes[mfi];
                gphi_flux.copy(grad_phi_curr[level][n][mfi]);
                gphi_flux.mult(area[level][n][mfi]);
            }

            phi_fine->CrseInit(fluxes,n,0,0,1,-1);
        }
    }

    if (phi_current && (iteration == ncycle)) 
      for (MFIter mfi(phi_curr[level]); mfi.isValid(); ++mfi) 
      {
         for (int n=0; n<BL_SPACEDIM; ++n)
            phi_current->FineAdd(grad_phi_curr[level][n][mfi],area[level][n][mfi],n,mfi.index(),0,0,1,1.);
      }

}

void
Gravity::average_fine_ec_onto_crse_ec(int level, int is_new)
{
    BL_PROFILE("Gravity::average_fine_ec_onto_crse_ec()");

    // NOTE: this is called with level == the coarser of the two levels involved
    if (level == parent->finestLevel()) return;

    //
    // Coarsen() the fine stuff on processors owning the fine data.
    //
    BoxArray crse_gphi_fine_BA(grids[level+1].size());

    IntVect fine_ratio = parent->refRatio(level);

    for (int i = 0; i < crse_gphi_fine_BA.size(); ++i)
        crse_gphi_fine_BA.set(i,BoxLib::coarsen(grids[level+1][i],fine_ratio));

    PArray<MultiFab> crse_gphi_fine(BL_SPACEDIM,PArrayManage);
    for (int n=0; n<BL_SPACEDIM; ++n)
    {
        const BoxArray eba = BoxArray(crse_gphi_fine_BA).surroundingNodes(n);
        crse_gphi_fine.set(n,new MultiFab(eba,1,0));
    }

    if (is_new == 1)
    {
       for (MFIter mfi(grad_phi_curr[level+1][0]); mfi.isValid(); ++mfi)
       {
           const int        i        = mfi.index();
           const Box&       ovlp     = crse_gphi_fine_BA[i];
   
           BL_FORT_PROC_CALL(CA_AVERAGE_EC,ca_average_ec)
               (D_DECL(BL_TO_FORTRAN(grad_phi_curr[level+1][0][mfi]),
                       BL_TO_FORTRAN(grad_phi_curr[level+1][1][mfi]),
                       BL_TO_FORTRAN(grad_phi_curr[level+1][2][mfi])),
                D_DECL(BL_TO_FORTRAN(crse_gphi_fine[0][mfi]),
                       BL_TO_FORTRAN(crse_gphi_fine[1][mfi]),
                       BL_TO_FORTRAN(crse_gphi_fine[2][mfi])),
                ovlp.loVect(),ovlp.hiVect(),fine_ratio.getVect());
       }
   
       for (int n=0; n<BL_SPACEDIM; ++n)
         grad_phi_curr[level][n].copy(crse_gphi_fine[n]);
    }
    else if (is_new == 0)
    {
       for (MFIter mfi(grad_phi_prev[level+1][0]); mfi.isValid(); ++mfi)
       {
           const int        i        = mfi.index();
           const Box&       ovlp     = crse_gphi_fine_BA[i];
   
           BL_FORT_PROC_CALL(CA_AVERAGE_EC,ca_average_ec)
               (D_DECL(BL_TO_FORTRAN(grad_phi_prev[level+1][0][mfi]),
                       BL_TO_FORTRAN(grad_phi_prev[level+1][1][mfi]),
                       BL_TO_FORTRAN(grad_phi_prev[level+1][2][mfi])),
                D_DECL(BL_TO_FORTRAN(crse_gphi_fine[0][mfi]),
                       BL_TO_FORTRAN(crse_gphi_fine[1][mfi]),
                       BL_TO_FORTRAN(crse_gphi_fine[2][mfi])),
                ovlp.loVect(),ovlp.hiVect(),fine_ratio.getVect());
       }
   
       for (int n=0; n<BL_SPACEDIM; ++n)
         grad_phi_prev[level][n].copy(crse_gphi_fine[n]);
    }
}

void
Gravity::avgDown (MultiFab& crse, const MultiFab& fine, const IntVect& ratio)
{
    BL_PROFILE("Gravity::avgDown()");

    //
    // Coarsen() the fine stuff on processors owning the fine data.
    //
    BoxArray crse_fine_BA(fine.boxArray().size());

    for (int i = 0; i < fine.boxArray().size(); ++i)
    {
        crse_fine_BA.set(i,BoxLib::coarsen(fine.boxArray()[i],ratio));
    }

    MultiFab crse_fine(crse_fine_BA,1,0);

    for (MFIter mfi(fine); mfi.isValid(); ++mfi)
    {
        const int        i        = mfi.index();
        const Box&       ovlp     = crse_fine_BA[i];
        FArrayBox&       crse_fab = crse_fine[i];
        const FArrayBox& fine_fab = fine[i];

	BL_FORT_PROC_CALL(CA_AVGDOWN_PHI,ca_avgdown_phi)
            (BL_TO_FORTRAN(crse_fab), 
             BL_TO_FORTRAN(fine_fab),
             ovlp.loVect(),ovlp.hiVect(),
             ratio.getVect());
    }

    crse.copy(crse_fine);
}

void
Gravity::test_composite_phi (int level)
{
    BL_PROFILE("Gravity::test_composite_phi()");

    if (verbose && ParallelDescriptor::IOProcessor()) {
        std::cout << "   " << '\n';
        std::cout << "... test_composite_phi at base level " << level << '\n';
    }

    int finest_level = parent->finestLevel();
    int nlevs = finest_level- level + 1;

    std::vector<BoxArray> bav(nlevs);
    std::vector<DistributionMapping> dmv(nlevs);

    for (int lev = 0; lev < nlevs; lev++) {
       bav[lev] = grids[level+lev];
       MultiFab& S_new = LevelData[level+lev].get_new_data(State_Type);
       dmv[lev] = S_new.DistributionMap();
    }
    std::vector<Geometry> fgeom(nlevs);
    for (int i = 0; i < nlevs; i++) 
      fgeom[i] = parent->Geom(level+i);

    MGT_Solver mgt_solver(fgeom, mg_bc, bav, dmv, false, stencil_type);
    
    Array< Array<Real> > xa(nlevs);
    Array< Array<Real> > xb(nlevs);

    for (int lev = 0; lev < nlevs; lev++) 
    {
        xa[lev].resize(BL_SPACEDIM);
        xb[lev].resize(BL_SPACEDIM);
         if ( level+lev == 0 ) {
           for ( int i = 0; i < BL_SPACEDIM; ++i ) {
             xa[lev][i] = 0.;
             xb[lev][i] = 0.;
           }
         } else {
           const Real* dx_crse   = parent->Geom(level+lev-1).CellSize();
           for ( int i = 0; i < BL_SPACEDIM; ++i ) {
             xa[lev][i] = 0.5 * dx_crse[i];
             xb[lev][i] = 0.5 * dx_crse[i];
           } 
         } 
    }

    MultiFab** phi_p = new MultiFab*[nlevs];
    MultiFab** Rhs_p = new MultiFab*[nlevs];
    MultiFab** Res_p = new MultiFab*[nlevs];

    Array< PArray<MultiFab> > coeffs(nlevs);

    for (int lev = 0; lev < nlevs; lev++)
    {
       BoxArray boxes(grids[level+lev]);

       phi_p[lev] = new MultiFab(boxes,1,1);
       MultiFab::Copy(*(phi_p[lev]),phi_curr[level+lev],0,0,1,1);

       Rhs_p[lev] = new MultiFab(boxes,1,0);
       Rhs_p[lev]->setVal(0.0);

       MultiFab::Copy(*(Rhs_p[lev]),LevelData[level+lev].get_new_data(State_Type),Density,0,1,0);

       // This is a correction for fully periodic domains only
       if ( Geometry::isAllPeriodic() )
       {
          if (verbose && ParallelDescriptor::IOProcessor() && mass_offset != 0.0)
             std::cout << " ... subtracting average density from RHS in solve at level ... " 
                       << level+lev << " " << mass_offset << std::endl;
          for (MFIter mfi((*Rhs_p[lev])); mfi.isValid(); ++mfi)
             (*Rhs_p[lev])[mfi].plus(-mass_offset);
       }

       Rhs_p[lev]->mult(Ggravity,0,1);

       // Need to do this even if Cartesian because the array is needed in set_gravity_coefficients
       coeffs[lev].resize(BL_SPACEDIM,PArrayManage);
       Geometry g = LevelData[level+lev].Geom();
       for (int i = 0; i < BL_SPACEDIM ; i++) {
           coeffs[lev].set(i, new MultiFab);
           g.GetFaceArea(coeffs[lev][i],boxes,i,0);
           coeffs[lev][i].setVal(1.0);
       }

#if (BL_SPACEDIM < 3)
       if (Geometry::IsRZ() || Geometry::IsSPHERICAL())
          applyMetricTerms(level+lev,(*Rhs_p[lev]),coeffs[lev]);
#endif

       Res_p[lev] = new MultiFab(boxes,1,0);
       Res_p[lev]->setVal(0.);
    }

    // Move filling of bndry to here so we can use Rhs and phi from above
    IntVect crse_ratio = level > 0 ? parent->refRatio(level-1)
                                   : IntVect::TheZeroVector();

    //
    // Store the Dirichlet boundary condition for phi in bndry.
    //
    const Geometry& geom = parent->Geom(level);
    MacBndry bndry(grids[level],1,geom);
    const int src_comp  = 0;
    const int dest_comp = 0;
    const int num_comp  = 1;

    // Build the homogeneous boundary conditions.  One could setVal
    // the bndry fabsets directly, but we instead do things as if
    // we had a fill-patched mf with grows--in that case the bndry
    // object knows how to grab grow data from the mf on physical
    // boundarys.  Here we creat an mf, setVal, and pass that to
    // the bndry object.
    //
    if (level == 0)
    {
        bndry.setHomogValues(*phys_bc,crse_ratio);
    }
    else
    {
        MultiFab CPhi;
        Real cur_time = LevelData[level].get_state_data(State_Type).curTime();
        GetCrsePhi(level,CPhi,cur_time);
        BoxArray crse_boxes = BoxArray(grids[level]).coarsen(crse_ratio);
        const int in_rad     = 0;
        const int out_rad    = 1;
        const int extent_rad = 2;
        BndryRegister crse_br(crse_boxes,in_rad,out_rad,extent_rad,num_comp);
        crse_br.copyFrom(CPhi,CPhi.nGrow(),src_comp,dest_comp,num_comp);
        bndry.setBndryValues(crse_br,src_comp,phi_curr[level],src_comp,
                             dest_comp,num_comp,crse_ratio,*phys_bc);
    }

    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
    {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,0);
    } else {
      mgt_solver.set_gravity_coefficients(coeffs,xa,xb,1);
    }
 
    mgt_solver.compute_residual(phi_p, Rhs_p, Res_p, bndry);

#if (BL_SPACEDIM < 3)
    // Do this to unweight the residual before printing the norm
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
       for (int lev = 0; lev < nlevs; lev++)
          unweight_cc(level+lev,(*Res_p[lev]));
#endif

    if (verbose) 
    {
        // Average residual from fine to coarse level before printing the norm
        for (int lev = nlevs-2; lev >= 0; lev--)
        {
           const IntVect ratio = parent->refRatio(lev);
           avgDown(*Res_p[lev],*Res_p[lev+1],ratio);
        } 

        for (int lev = 0; lev < nlevs; lev++) {
           Real resnorm = Res_p[lev]->norm0();
           if (ParallelDescriptor::IOProcessor()) 
             std::cout << "      ... norm of composite residual at level " << level+lev << 
                          "  " << resnorm << '\n';
        }
        if (ParallelDescriptor::IOProcessor()) std::cout << " " << '\n';
    }

    for (int lev = 0; lev < nlevs; lev++) {
       delete phi_p[lev];
       delete Rhs_p[lev];
       delete Res_p[lev];
    }
    delete [] phi_p;
    delete [] Rhs_p;
    delete [] Res_p;
}

void
Gravity::reflux_phi (int level, MultiFab& dphi)
{
    const Geometry& geom = parent->Geom(level);
    dphi.setVal(0.);
    phi_flux_reg[level+1].Reflux(dphi,volume[level],1.0,0,0,1,geom);
}

void 
Gravity::fill_ec_grow (int level,
                       PArray<MultiFab>&       ecF,
                       const PArray<MultiFab>& ecC) const
{
    BL_PROFILE("Gravity::fill_ec_grow()");

    //
    // Fill grow cells of the edge-centered mfs.  Assume
    // ecF built on edges of grids at this amr level, and ecC 
    // is build on edges of the grids at amr level-1
    //
    BL_ASSERT(ecF.size() == BL_SPACEDIM);

    const int nGrow = ecF[0].nGrow();

    if (nGrow == 0 || level == 0) return;

#if BL_SPACEDIM >= 2
    BL_ASSERT(nGrow == ecF[1].nGrow());
#endif
#if BL_SPACEDIM == 3
    BL_ASSERT(nGrow == ecF[2].nGrow());
#endif

    const BoxArray& fgrids = grids[level];
    const Geometry& fgeom  = parent->Geom(level);

    BoxList bl = BoxLib::GetBndryCells(fgrids,1);

    BoxArray f_bnd_ba(bl);

    bl.clear();

    BoxArray c_bnd_ba = BoxArray(f_bnd_ba.size());

    IntVect crse_ratio = parent->refRatio(level-1);

    for (int i = 0; i < f_bnd_ba.size(); ++i)
    {
        c_bnd_ba.set(i,Box(f_bnd_ba[i]).coarsen(crse_ratio));
        f_bnd_ba.set(i,Box(c_bnd_ba[i]).refine(crse_ratio));
    }
    
    for (int n = 0; n < BL_SPACEDIM; ++n)
    {
        //
        // crse_src & fine_src must have same parallel distribution.
        // We'll use the KnapSack distribution for the fine_src_ba.
        // Since fine_src_ba should contain more points, this'll lead
        // to a better distribution.
        //
        BoxArray crse_src_ba(c_bnd_ba);
        BoxArray fine_src_ba(f_bnd_ba);
        
        crse_src_ba.surroundingNodes(n);
        fine_src_ba.surroundingNodes(n);
        
        std::vector<long> wgts(fine_src_ba.size());
        
        for (unsigned int i = 0; i < wgts.size(); i++)
        {
            wgts[i] = fine_src_ba[i].numPts();
        }
        DistributionMapping dm;
        //
        // This call doesn't invoke the MinimizeCommCosts() stuff.
        // There's very little to gain with these types of coverings
        // of trying to use SFC or anything else.
        // This also guarantees that these DMs won't be put into the
        // cache, as it's not representative of that used for more
        // usual MultiFabs.
        //
        dm.KnapSackProcessorMap(wgts,ParallelDescriptor::NProcs());
        
        MultiFab crse_src; crse_src.define(crse_src_ba, 1, 0, dm, Fab_allocate);
        MultiFab fine_src; fine_src.define(fine_src_ba, 1, 0, dm, Fab_allocate);
        
        crse_src.setVal(1.e200);
        fine_src.setVal(1.e200);
        //
        // We want to fill crse_src from ecC[n].
        // Gotta do it in steps since parallel copy only does valid region.
        //
        {
            BoxArray edge_grids = ecC[n].boxArray();
            edge_grids.grow(ecC[n].nGrow());

            MultiFab ecCG(edge_grids,1,0);

            for (MFIter mfi(ecC[n]); mfi.isValid(); ++mfi)
                ecCG[mfi].copy(ecC[n][mfi]);

            crse_src.copy(ecCG);
        }

        for (MFIter mfi(crse_src); mfi.isValid(); ++mfi)
        {
            const int  nComp = 1;
            const Box  box   = crse_src[mfi].box();
            const int* rat   = crse_ratio.getVect();
            BL_FORT_PROC_CALL(CA_PC_EDGE_INTERP,ca_pc_edge_interp)
                (box.loVect(), box.hiVect(), &nComp, rat, &n,
                 BL_TO_FORTRAN(crse_src[mfi]),
                 BL_TO_FORTRAN(fine_src[mfi]));
        }

        crse_src.clear();
        //
        // Replace pc-interpd fine data with preferred u_mac data at
        // this level u_mac valid only on surrounding faces of valid
        // region - this op will not fill grow region.
        //
        fine_src.copy(ecF[n]); // parallel copy
        
        for (MFIter mfi(fine_src); mfi.isValid(); ++mfi)
        {
            //
            // Interpolate unfilled grow cells using best data from
            // surrounding faces of valid region, and pc-interpd data
            // on fine edges overlaying coarse edges.
            //
            const int  nComp = 1;
            const Box& fbox  = fine_src[mfi.index()].box();
            const int* rat   = crse_ratio.getVect();
            BL_FORT_PROC_CALL(CA_EDGE_INTERP,ca_edge_interp)
                (fbox.loVect(), fbox.hiVect(), &nComp, rat, &n,
                 BL_TO_FORTRAN(fine_src[mfi]));
        }
        //
        // Build a mf with no grow cells on ecF grown boxes, do parallel copy into.
        //
        BoxArray fgridsG = ecF[n].boxArray();
        fgridsG.grow(ecF[n].nGrow());

        MultiFab ecFG(fgridsG, 1, 0);

        ecFG.copy(fine_src); // Parallel copy
        ecFG.copy(ecF[n]);   // Parallel copy

        for (MFIter mfi(ecF[n]); mfi.isValid(); ++mfi)
            ecF[n][mfi].copy(ecFG[mfi]);
    }

    for (int n = 0; n < BL_SPACEDIM; ++n)
    {
        ecF[n].FillBoundary();
        fgeom.FillPeriodicBoundary(ecF[n],true);
    }
}

#if (BL_SPACEDIM == 1)
void
Gravity::make_one_d_grav(int level,Real time, MultiFab& grav_vector)
{
    BL_PROFILE("Gravity::make_one_d_grav()");

   int ng = grav_vector.nGrow();

   AmrLevel* amrlev =                     &parent->getLevel(level) ;
   const Real* dx   = parent->Geom(level).CellSize();
   Box domain(parent->Geom(level).Domain());

   Box bx(grids[level].minimalBox());
   bx.setSmall(0,-ng);
   bx.setBig(0,bx.hiVect()[0]+ng);
   BoxArray ba(bx);

   FArrayBox grav_fab(bx,1);

   // We only use mf for its BoxArray in the FillPatchIterator --
   //    it doesn't need to have enough components
   MultiFab mf(ba,1,0,Fab_allocate);

   const Real* problo = parent->Geom(level).ProbLo();

#ifdef GR_GRAV
   // Fill the state, interpolated from coarser levels where needed,
   //   and compute gravity by integrating outward from the center.
   //   Include post-Newtonian corrections.

   // We only use S_new to get the number of components for the GR routine
   MultiFab& S_new = LevelData[level].get_new_data(State_Type);
   int nvar = S_new.nComp(); 

   for (FillPatchIterator fpi(*amrlev,mf,0,time,State_Type,Density,nvar); 
        fpi.isValid(); ++fpi) 
   {
      BL_FORT_PROC_CALL(CA_COMPUTE_1D_GR_GRAV,ca_compute_1d_gr_grav)
          (BL_TO_FORTRAN(fpi()),grav_fab.dataPtr(),dx,problo);
   }
#else
   // Fill density, interpolated from coarser levels where needed,
   //   and compute gravity by integrating outward from the center
   for (FillPatchIterator fpi(*amrlev,mf,0,time,State_Type,Density,1); 
        fpi.isValid(); ++fpi) 
   {
      BL_FORT_PROC_CALL(CA_COMPUTE_1D_GRAV,ca_compute_1d_grav)
          (BL_TO_FORTRAN(fpi()),grav_fab.dataPtr(),dx,problo);
   }
#endif

   // Only whichProc is used to fill grav_fab.
   int whichProc(mf.DistributionMap()[0]);

   ParallelDescriptor::Bcast(grav_fab.dataPtr(),grav_fab.box().numPts(),whichProc);

   for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi) 
   {
        grav_vector[mfi].copy(grav_fab);
   }
      
}
#endif

#if (BL_SPACEDIM > 1)
void
Gravity::make_prescribed_grav(int level, Real time, MultiFab& grav_vector)
{
    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();



    for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi)
    {
       Box bx(mfi.validbox());
       BL_FORT_PROC_CALL(CA_PRESCRIBE_GRAV,ca_prescribe_grav)
           (bx.loVect(), bx.hiVect(), dx,
            BL_TO_FORTRAN(grav_vector[mfi]),
            geom.ProbLo());
    }
    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_prescribed_grav() time = " << end << std::endl;
    }
}

void
Gravity::interpolate_monopole_grav(int level, Array<Real>& radial_grav, MultiFab& grav_vector)
{
    int n1d = radial_grav.size();

    const Geometry& geom = parent->Geom(level);
    const Real* dx = geom.CellSize();
    Real dr        = dx[0] / double(drdxfac);

    for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi)
    {
       Box bx(mfi.validbox());
       BL_FORT_PROC_CALL(CA_PUT_RADIAL_GRAV,ca_put_radial_grav)
           (bx.loVect(), bx.hiVect(),dx,&dr,
            BL_TO_FORTRAN(grav_vector[mfi]),
            radial_grav.dataPtr(),geom.ProbLo(),
            &n1d,&level);
    }
}
#endif

void
Gravity::make_radial_phi(int level, MultiFab& Rhs, MultiFab& phi, int fill_interior)
{
    BL_PROFILE("Gravity::make_radial_phi()");

    BL_ASSERT(level==0);

#if (BL_SPACEDIM > 1)
    const Real strt = ParallelDescriptor::second();

    int n1d = drdxfac*numpts_at_level;

    Array<Real> radial_mass(n1d,0);
    Array<Real> radial_vol(n1d,0);
    Array<Real> radial_phi(n1d,0);
    Array<Real> radial_grav(n1d+1,0);

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();
    Real dr = dx[0] / double(drdxfac);

    for (int i = 0; i < n1d; i++) radial_mass[i] = 0.;
    for (int i = 0; i < n1d; i++) radial_vol[i] = 0.;

    // Define total mass in each shell
    // Note that RHS = density (we have not yet multiplied by G)
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_COMPUTE_RADIAL_MASS,ca_compute_radial_mass)
            (bx.loVect(), bx.hiVect(),dx,&dr,
             BL_TO_FORTRAN(Rhs[mfi]), 
             radial_mass.dataPtr(), radial_vol.dataPtr(),
             geom.ProbLo(),&n1d,&drdxfac,&level);
    }
   
    ParallelDescriptor::ReduceRealSum(radial_mass.dataPtr(),n1d);

    // Integrate radially outward to define the gravity
    BL_FORT_PROC_CALL(CA_INTEGRATE_PHI,ca_integrate_phi)
        (radial_mass.dataPtr(),radial_grav.dataPtr(),radial_phi.dataPtr(),&dr,&n1d);

    Box domain(parent->Geom(level).Domain());
    for (MFIter mfi(phi); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_PUT_RADIAL_PHI,ca_put_radial_phi)
            (bx.loVect(), bx.hiVect(),
             domain.loVect(), domain.hiVect(),
             dx,&dr, BL_TO_FORTRAN(phi[mfi]),
             radial_phi.dataPtr(),geom.ProbLo(),
             &n1d,&fill_interior);
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_radial_phi() time = " << end << std::endl;
    }

#else
    BoxLib::Abort("Can't use make_radial_phi with dim == 1");
#endif
}


#if (BL_SPACEDIM == 3)
void
Gravity::fill_multipole_BCs(int level, MultiFab& Rhs, MultiFab& phi)
{
    BL_ASSERT(level==0);

    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();

    // Storage arrays for the multipole moments.
    // We will initialize them to zero, and then
    // sum up the results over grids.

    Box boxq0( IntVect(), IntVect( lnum, 0,    0 ) );
    Box boxqC( IntVect(), IntVect( lnum, lnum, 0 ) );
    Box boxqS( IntVect(), IntVect( lnum, lnum, 0 ) );

    FArrayBox q0(boxq0);
    FArrayBox qC(boxqC);
    FArrayBox qS(boxqS);

    q0.setVal(0.0);
    qC.setVal(0.0);
    qS.setVal(0.0);

    // Loop through the grids and compute the individual contributions
    // to the various moments. The multipole moment constructor
    // is coded to only add to the moment arrays, so it is safe
    // to directly hand the arrays to them.

    int lo_bc[3];
    int hi_bc[3];

    for (int dir = 0; dir < 3; dir++)
    {
      lo_bc[dir] = phys_bc->lo(dir);
      hi_bc[dir] = phys_bc->hi(dir);
    }

    int symmetry_type = Symmetry;

    Box domain(parent->Geom(level).Domain());
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_COMPUTE_MULTIPOLE_MOMENTS,ca_compute_multipole_moments)
	    (bx.loVect(), bx.hiVect(), domain.loVect(), domain.hiVect(), 
             &symmetry_type,lo_bc,hi_bc,
             dx,BL_TO_FORTRAN(Rhs[mfi]),geom.ProbLo(),geom.ProbHi(),
             &lnum,q0.dataPtr(),qC.dataPtr(),qS.dataPtr());
    }

    // Now, do a global reduce over all processes.

    ParallelDescriptor::ReduceRealSum(q0.dataPtr(),boxq0.numPts());
    ParallelDescriptor::ReduceRealSum(qC.dataPtr(),boxqC.numPts());
    ParallelDescriptor::ReduceRealSum(qS.dataPtr(),boxqS.numPts());

    // Finally, construct the boundary conditions using the
    // complete multipole moments, for all points on the
    // boundary that are held on this process.

    for (MFIter mfi(phi); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_PUT_MULTIPOLE_BC,ca_put_multipole_bc)
            (bx.loVect(), bx.hiVect(),
             domain.loVect(), domain.hiVect(),
             dx, BL_TO_FORTRAN(phi[mfi]),
             geom.ProbLo(), geom.ProbHi(),
             &lnum,q0.dataPtr(),qC.dataPtr(),qS.dataPtr());
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::fill_multipole_BCs() time = " << end << std::endl;
    }

}

void
Gravity::fill_direct_sum_BCs(int level, MultiFab& Rhs, MultiFab& phi)
{
    BL_ASSERT(level==0);

    const Real strt = ParallelDescriptor::second();

    const Geometry& geom = parent->Geom(level);
    const Real* dx   = geom.CellSize();

    // Storage arrays for the BCs.

    const int* domlo = geom.Domain().loVect();
    const int* domhi = geom.Domain().hiVect();

    const int loVectXY[3] = {domlo[0]-1, domlo[1]-1, 0         };
    const int hiVectXY[3] = {domhi[0]+1, domhi[1]+1, 0         };

    const int loVectXZ[3] = {domlo[0]-1, 0         , domlo[2]-1};
    const int hiVectXZ[3] = {domhi[0]+1, 0         , domhi[2]+1};

    const int loVectYZ[3] = {0         , domlo[1]-1, domlo[2]-1};
    const int hiVectYZ[3] = {0         , domhi[1]+1, domhi[1]+1};

    IntVect smallEndXY( loVectXY );
    IntVect bigEndXY  ( hiVectXY );
    IntVect smallEndXZ( loVectXZ );
    IntVect bigEndXZ  ( hiVectXZ );
    IntVect smallEndYZ( loVectYZ );
    IntVect bigEndYZ  ( hiVectYZ );

    Box boxXY(smallEndXY, bigEndXY);
    Box boxXZ(smallEndXZ, bigEndXZ);
    Box boxYZ(smallEndYZ, bigEndYZ);

    const int nPtsXY = boxXY.numPts();
    const int nPtsXZ = boxXZ.numPts();
    const int nPtsYZ = boxYZ.numPts(); 

    FArrayBox bcXYLo(boxXY);
    FArrayBox bcXYHi(boxXY);
    FArrayBox bcXZLo(boxXZ);
    FArrayBox bcXZHi(boxXZ);
    FArrayBox bcYZLo(boxYZ);
    FArrayBox bcYZHi(boxYZ);

    bcXYLo.setVal(0.0);
    bcXYHi.setVal(0.0);
    bcXZLo.setVal(0.0);
    bcXZHi.setVal(0.0);
    bcYZLo.setVal(0.0);
    bcYZHi.setVal(0.0);
    
    // Loop through the grids and compute the individual contributions
    // to the BCs. The BC constructor is coded to only add to the 
    // BCs, so it is safe to directly hand the arrays to them.

    int lo_bc[3];
    int hi_bc[3];

    for (int dir = 0; dir < 3; dir++)
    {
      lo_bc[dir] = phys_bc->lo(dir);
      hi_bc[dir] = phys_bc->hi(dir);
    }

    int symmetry_type = Symmetry;
    
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_COMPUTE_DIRECT_SUM_BC,ca_compute_direct_sum_bc)
	    (bx.loVect(), bx.hiVect(), domlo, domhi, 
             &symmetry_type,lo_bc,hi_bc,
             dx,BL_TO_FORTRAN(Rhs[mfi]),
             geom.ProbLo(),geom.ProbHi(),
             bcXYLo.dataPtr(), bcXYHi.dataPtr(),
             bcXZLo.dataPtr(), bcXZHi.dataPtr(),
             bcYZLo.dataPtr(), bcYZHi.dataPtr());
    }

    ParallelDescriptor::ReduceRealSum(bcXYLo.dataPtr(), nPtsXY);
    ParallelDescriptor::ReduceRealSum(bcXYHi.dataPtr(), nPtsXY);
    ParallelDescriptor::ReduceRealSum(bcXZLo.dataPtr(), nPtsXZ);
    ParallelDescriptor::ReduceRealSum(bcXZHi.dataPtr(), nPtsXZ);
    ParallelDescriptor::ReduceRealSum(bcYZLo.dataPtr(), nPtsYZ);
    ParallelDescriptor::ReduceRealSum(bcYZHi.dataPtr(), nPtsYZ);
    
    for (MFIter mfi(phi); mfi.isValid(); ++mfi)
    {
        Box bx(mfi.validbox());
        BL_FORT_PROC_CALL(CA_PUT_DIRECT_SUM_BC,ca_put_direct_sum_bc)
            (bx.loVect(), bx.hiVect(), domlo, domhi,
             BL_TO_FORTRAN(phi[mfi]),
             bcXYLo.dataPtr(), bcXYHi.dataPtr(),
             bcXZLo.dataPtr(), bcXZHi.dataPtr(),
             bcYZLo.dataPtr(), bcYZHi.dataPtr());
    }

    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::fill_direct_sum_BCs() time = " << end << std::endl;
    }
    
}
#endif

#if (BL_SPACEDIM < 3)
void
Gravity::applyMetricTerms(int level, MultiFab& Rhs, PArray<MultiFab>& coeffs)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
    for (MFIter mfi(Rhs); mfi.isValid(); ++mfi)
    {
        const Box bx = mfi.validbox();
        // Modify Rhs and coeffs with the appropriate metric terms.
        BL_FORT_PROC_CALL(CA_APPLY_METRIC,ca_apply_metric)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(Rhs[mfi]),
             D_DECL(BL_TO_FORTRAN(coeffs[0][mfi]),
                    BL_TO_FORTRAN(coeffs[1][mfi]),
                    BL_TO_FORTRAN(coeffs[2][mfi])),
                    dx,&coord_type);
    }
}

void
Gravity::unweight_cc(int level, MultiFab& cc)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
    for (MFIter mfi(cc); mfi.isValid(); ++mfi)
    {
        int index = mfi.index();
        const Box bx = grids[level][index];
        BL_FORT_PROC_CALL(CA_UNWEIGHT_CC,ca_unweight_cc)
            (bx.loVect(), bx.hiVect(),
             BL_TO_FORTRAN(cc[mfi]),dx,&coord_type);
    }
}

void
Gravity::unweight_edges(int level, PArray<MultiFab>& edges)
{
    const Real* dx = parent->Geom(level).CellSize();
    int coord_type = Geometry::Coord();
    for (MFIter mfi(edges[0]); mfi.isValid(); ++mfi)
    {
        int index = mfi.index();
        const Box bx = grids[level][index];
        BL_FORT_PROC_CALL(CA_UNWEIGHT_EDGES,ca_unweight_edges)
            (bx.loVect(), bx.hiVect(),
             D_DECL(BL_TO_FORTRAN(edges[0][mfi]),
                    BL_TO_FORTRAN(edges[1][mfi]),
                    BL_TO_FORTRAN(edges[2][mfi])),
             dx,&coord_type);
    }
}
#endif

void
Gravity::make_mg_bc ()
{
    const Geometry& geom = parent->Geom(0);
    for ( int dir = 0; dir < BL_SPACEDIM; ++dir )
    {
        if ( geom.isPeriodic(dir) )
        {
            mg_bc[2*dir + 0] = 0;
            mg_bc[2*dir + 1] = 0;
        }
        else
        {
            if (phys_bc->lo(dir) == Symmetry) {
              mg_bc[2*dir + 0] = MGT_BC_NEU;
            } else if (phys_bc->lo(dir) == Outflow) {
              mg_bc[2*dir + 0] = MGT_BC_DIR;
            } else {
              BoxLib::Abort("Unknown lo bc in make_mg_bc");
            }
            if (phys_bc->hi(dir) == Symmetry) {
              mg_bc[2*dir + 1] = MGT_BC_NEU;
            } else if (phys_bc->hi(dir) == Outflow) {
              mg_bc[2*dir + 1] = MGT_BC_DIR;
            } else {
              BoxLib::Abort("Unknown hi bc in make_mg_bc");
            }
        }
    }

    // Set Neumann bc at r=0.
    if (Geometry::IsSPHERICAL() || Geometry::IsRZ() )
        mg_bc[0] = MGT_BC_NEU;
}

void
Gravity::set_mass_offset (Real time)
{
    Real old_mass_offset = 0;

    if (parent->finestLevel() > 0) old_mass_offset = mass_offset;

    mass_offset = 0;

    const Geometry& geom = parent->Geom(0);

    if (geom.isAllPeriodic()) 
    {
       // Note: we must loop over levels because the volWgtSum routine zeros out
       //       crse regions under fine regions
       for (int lev = 0; lev <= parent->finestLevel(); lev++) {
          Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(lev));
          mass_offset += cs->volWgtSum("density", time);

#ifdef PARTICLES
          if ( Castro::theDMPC() )
             mass_offset   += Castro::theDMPC()->sumParticleMass(lev);
#endif
       }
 
       mass_offset = mass_offset / geom.ProbSize();
       if (verbose && ParallelDescriptor::IOProcessor()) 
          std::cout << "Defining average density to be " << mass_offset << std::endl;
    }

    Real diff = std::abs(mass_offset - old_mass_offset);
    Real eps = 1.e-10 * std::abs(old_mass_offset);
    if (diff > eps && old_mass_offset > 0)
    {
       if (ParallelDescriptor::IOProcessor())
       {
          std::cout << " ... new vs old mass_offset " << mass_offset << " " << old_mass_offset
                    << " ... diff is " << diff <<  std::endl;
          std::cout << " ... Gravity::set_mass_offset -- total mass has changed!" << std::endl;;
       }
//     BoxLib::Error("Gravity::set_mass_offset -- total mass has changed!");
    }
}

#ifdef POINTMASS
void
Gravity::add_pointmass_to_gravity (int level, MultiFab& grav_vector, Real point_mass)
{
   const Real* dx     = parent->Geom(level).CellSize();
   const Real* problo = parent->Geom(level).ProbLo();
   for (MFIter mfi(grav_vector); mfi.isValid(); ++mfi)
   {
        BL_FORT_PROC_CALL(PM_ADD_TO_GRAV,pm_add_to_grav)
            (&point_mass,BL_TO_FORTRAN(grav_vector[mfi]),
             problo,dx);
   }
}
#endif

#if (BL_SPACEDIM == 3)
Real
Gravity::computeAvg (int level, MultiFab* mf)
{
    BL_PROFILE("Gravity::computeAvg()");

    Real        sum     = 0.0;

    const Geometry& geom = parent->Geom(level);
    const Real* dx       = geom.CellSize();

    BL_ASSERT(mf != 0);

    BoxArray baf;

    if (level < parent->finestLevel())
    {
        IntVect fine_ratio = parent->refRatio(level);
        baf = parent->boxArray(level+1);
        baf.coarsen(fine_ratio);
    }

    for (MFIter mfi(*mf); mfi.isValid(); ++mfi)
    {
        FArrayBox& fab = (*mf)[mfi];

        if (level < parent->finestLevel())
        {
            std::vector< std::pair<int,Box> > isects = baf.intersections(grids[level][mfi.index()]);

            for (int ii = 0; ii < isects.size(); ii++)
            {
                fab.setVal(0,isects[ii].second,0,fab.nComp());
            }
        }
        Real s;
        const Box& box  = mfi.validbox();
        const int* lo   = box.loVect();
        const int* hi   = box.hiVect();

        //
        // Note that this routine will do a volume weighted sum of
        // whatever quantity is passed in, not strictly the "mass".
        //
	BL_FORT_PROC_CALL(CA_SUMMASS,ca_summass)
            (BL_TO_FORTRAN(fab),lo,hi,dx,&s);
        sum += s;
    }

    ParallelDescriptor::ReduceRealSum(sum);

    return sum;
}
#endif

#if (BL_SPACEDIM > 1)
void
Gravity::make_radial_gravity(int level, Real time, Array<Real>& radial_grav)
{
    BL_PROFILE("Gravity::make_radial_gravity()");

    const Real strt = ParallelDescriptor::second();

    // This is just here in case we need to debug ...
    int do_diag = 0;

    BoxArray baf;
    Real sum_over_levels = 0.;

    for (int lev = 0; lev <= level; lev++)
    {
        const Real t_old = LevelData[lev].get_state_data(State_Type).prevTime();
        const Real t_new = LevelData[lev].get_state_data(State_Type).curTime();
        const Real eps   = (t_new - t_old) * 1.e-6;

        if (lev < level)
        {
            baf = parent->boxArray(lev+1);
            baf.coarsen(parent->refRatio(lev));
        }

        // Create MultiFab with one component and no grow cells
        MultiFab S(grids[lev],1,0);

        if ( std::abs(time-t_old) < eps)
        {
            S.copy(LevelData[lev].get_old_data(State_Type),Density,0,1);
        } 
        else if ( std::abs(time-t_new) < eps)
        {
            S.copy(LevelData[lev].get_new_data(State_Type),Density,0,1);
            if (lev < level)
            {
                Castro* cs = dynamic_cast<Castro*>(&parent->getLevel(lev+1));
                cs->getFluxReg().Reflux(S,volume[lev],1.0,0,0,1,parent->Geom(lev));
            }
        } 
        else if (time > t_old && time < t_new)
        {
            Real alpha   = (time - t_old)/(t_new - t_old);
            Real omalpha = 1.0 - alpha;

            S.copy(LevelData[lev].get_old_data(State_Type),Density,0,1);
            S.mult(omalpha);

            MultiFab S_new(grids[lev],1,0);
            S_new.copy(LevelData[lev].get_new_data(State_Type),Density,0,1);
            S_new.mult(alpha);

            S.plus(S_new,Density,1,0);
        }  
        else
        {  
     	    std::cout << " Level / Time in make_radial_gravity is: " << lev << " " << time  << std::endl;
      	    std::cout << " but old / new time      are: " << t_old << " " << t_new << std::endl;
      	    BoxLib::Abort("Problem in Gravity::make_radial_gravity");
        }  

        int n1d = radial_mass[lev].size();

#ifdef GR_GRAV
        for (int i = 0; i < n1d; i++) radial_pres[lev][i] = 0.;
#endif
        for (int i = 0; i < n1d; i++) radial_vol[lev][i] = 0.;
        for (int i = 0; i < n1d; i++) radial_mass[lev][i] = 0.;

        const Geometry& geom = parent->Geom(lev);
        const Real* dx   = geom.CellSize();
        Real dr = dx[0] / double(drdxfac);

        for (MFIter mfi(S); mfi.isValid(); ++mfi)
        {
           Box bx(mfi.validbox());
           FArrayBox& fab = S[mfi];
           if (lev < level)
           {
               std::vector< std::pair<int,Box> > isects = baf.intersections(grids[lev][mfi.index()]);
               for (int ii = 0; ii < isects.size(); ii++)
                   fab.setVal(0,isects[ii].second,0,fab.nComp());
           }

           BL_FORT_PROC_CALL(CA_COMPUTE_RADIAL_MASS,ca_compute_radial_mass)
               (bx.loVect(), bx.hiVect(), dx, &dr,
                BL_TO_FORTRAN(fab), 
                radial_mass[lev].dataPtr(), 
                radial_vol[lev].dataPtr(), 
                geom.ProbLo(),&n1d,&drdxfac,&lev);

#ifdef GR_GRAV
           BL_FORT_PROC_CALL(CA_COMPUTE_AVGPRES,ca_compute_avgpres)
               (bx.loVect(), bx.hiVect(),dx,&dr,
                BL_TO_FORTRAN(fab),
                radial_pres[lev].dataPtr(),
                geom.ProbLo(),&n1d,&drdxfac,&lev);
#endif
        }

        ParallelDescriptor::ReduceRealSum(radial_mass[lev].dataPtr() ,n1d);
        ParallelDescriptor::ReduceRealSum(radial_vol[lev].dataPtr()  ,n1d);
#ifdef GR_GRAV
        ParallelDescriptor::ReduceRealSum(radial_pres[lev].dataPtr()  ,n1d);
#endif

        if (do_diag > 0)
        {
            Real sum = 0.;
            for (int i = 0; i < n1d; i++) sum += radial_mass[lev][i];
            sum_over_levels += sum;
        }
    }

    if (do_diag > 0 && ParallelDescriptor::IOProcessor())
        std::cout << "Gravity::make_radial_gravity: Sum of mass over all levels " << sum_over_levels << std::endl;

    int n1d = radial_mass[level].size();
    Array<Real> radial_mass_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
    {
        radial_mass_summed[i] = radial_mass[level][i];
    }

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        int ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
            {
                for (int n = 0; n < ratio; n++)
                {
                   radial_mass_summed[ratio*i+n] += 1./double(ratio) * radial_mass[lev][i];
                }
            }
        }
    }

    if (do_diag > 0 && ParallelDescriptor::IOProcessor())
    {
        Real sum_added = 0.;
        for (int i = 0; i < n1d; i++) sum_added += radial_mass_summed[i];
        std::cout << "Gravity::make_radial_gravity: Sum of combined mass " << sum_added << std::endl;
    }

    const Geometry& geom = parent->Geom(level);
    const Real* dx = geom.CellSize();
    Real dr        = dx[0] / double(drdxfac);

    // ***************************************************************** //
    // Compute the average density to use at the radius above
    //   max_radius_all_in_domain so we effectively count mass outside
    //   the domain.
    // ***************************************************************** //

    Array<Real> radial_vol_summed(n1d,0);
    Array<Real> radial_den_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
         radial_vol_summed[i] =  radial_vol[level][i];

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        int ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
            {
                for (int n = 0; n < ratio; n++)
                {
                   radial_vol_summed[ratio*i+n]  += 1./double(ratio) * radial_vol[lev][i];
                }
            }
        }
    }

    for (int i = 0; i < n1d; i++)  
    {
        radial_den_summed[i] = radial_mass_summed[i];
        if (radial_vol_summed[i] > 0.) radial_den_summed[i]  /= radial_vol_summed[i];
    }

#ifdef GR_GRAV
    Array<Real> radial_pres_summed(n1d,0);

    // First add the contribution from this level
    for (int i = 0; i < n1d; i++)  
        radial_pres_summed[i] = radial_pres[level][i];

    // Now add the contribution from coarser levels
    if (level > 0) 
    {
        ratio = parent->refRatio(level-1)[0];
        for (int lev = level-1; lev >= 0; lev--)
        {
            if (lev < level-1) ratio *= parent->refRatio(lev)[0];
            for (int i = 0; i < n1d/ratio; i++)  
                for (int n = 0; n < ratio; n++)
                   radial_pres_summed[ratio*i+n] += 1./double(ratio) * radial_pres[lev][i];
        }
    }

    for (int i = 0; i < n1d; i++)  
        if (radial_vol_summed[i] > 0.) radial_pres_summed[i] /= radial_vol_summed[i];

    // Integrate radially outward to define the gravity -- here we add the post-Newtonian correction
    BL_FORT_PROC_CALL(CA_INTEGRATE_GR_GRAV,ca_integrate_gr_grav)
        (radial_den_summed.dataPtr(),radial_mass_summed.dataPtr(),
         radial_pres_summed.dataPtr(),radial_grav.dataPtr(),&dr,&n1d);

#else
    // Integrate radially outward to define the gravity
    BL_FORT_PROC_CALL(CA_INTEGRATE_GRAV,ca_integrate_grav)
        (radial_mass_summed.dataPtr(),radial_den_summed.dataPtr(),
         radial_grav.dataPtr(),&max_radius_all_in_domain,&dr,&n1d); 
#endif
   
    if (verbose)
    {
        const int IOProc = ParallelDescriptor::IOProcessorNumber();
        Real      end    = ParallelDescriptor::second() - strt;

        ParallelDescriptor::ReduceRealMax(end,IOProc);

        if (ParallelDescriptor::IOProcessor())
            std::cout << "Gravity::make_radial_gravity() time = " << end << std::endl;
    }
}

#ifdef PARTICLES
void
Gravity::AddParticlesToRhs (int               level,
                            MultiFab&         Rhs,
                            int               ngrow)
{
    if( Castro::theDMPC() )
    {
        MultiFab particle_mf(grids[level], 1, ngrow);
        particle_mf.setVal(0.);
        Castro::theDMPC()->AssignDensitySingleLevel(particle_mf, level);
        MultiFab::Add(Rhs, particle_mf, 0, 0, 1, 0);
    }
}

void
Gravity::AddParticlesToRhs(int base_level, int finest_level, PArray<MultiFab>& Rhs_particles)
{
    const int num_levels = finest_level - base_level + 1;

    if( Castro::theDMPC() )
    {
        PArray<MultiFab> PartMF;
        Castro::theDMPC()->AssignDensity(PartMF, base_level, 1, finest_level);
        for (int lev = 0; lev < num_levels; lev++)
        {
            if (PartMF[lev].contains_nan())
            {
                std::cout << "Testing particle density at level " << base_level+lev << std::endl;
                BoxLib::Abort("...PartMF has NaNs in Gravity::actual_multilevel_solve()");
            }
        }

        for (int lev = finest_level - 1 - base_level; lev >= 0; lev--)
        {
            const IntVect ratio = parent->refRatio(lev+base_level);
            avgDown(PartMF[lev], PartMF[lev+1], ratio);
        }

        for (int lev = 0; lev < num_levels; lev++)
        {
            MultiFab::Add(Rhs_particles[lev], PartMF[lev], 0, 0, 1, 0);
        }
    }
}
#endif
#endif
