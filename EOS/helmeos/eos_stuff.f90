module eos_module

  use bl_types
  use bl_error_module
  use bl_constants_module
  use network, only: nspec, aion, zion
  use eos_type_module
  use eos_data_module

  implicit none

  logical,         save, private :: do_coulomb
  integer,         save, private :: max_newton = 100
  logical,         save, private :: input_is_constant

  double precision, save, private :: ttol = 1.0d-8
  double precision, save, private :: dtol = 1.0d-8

  public eos_init, eos

contains

  ! EOS initialization routine -- this is used by both MAESTRO and CASTRO
  ! For this general EOS, this calls helmeos_init() which reads in the 
  ! table with the electron component's properties.
  subroutine eos_init(small_temp, small_dens)

    use parallel
    use extern_probin_module, only: use_eos_coulomb, eos_input_is_constant

    implicit none
 
    double precision, intent(in), optional :: small_temp
    double precision, intent(in), optional :: small_dens
 
    do_coulomb = use_eos_coulomb
    input_is_constant = eos_input_is_constant 

    smallt = 1.d4

    if (present(small_temp)) then
      if (small_temp > ZERO) then
       smallt = small_temp
      end if
    endif

    smalld = 1.d-5
 
    if (present(small_dens)) then
       if (small_dens > ZERO) then
         smalld = small_dens
       endif
    endif

    if (parallel_IOProcessor()) print *, 'Initializing helmeos... Coulomb corrections = ', do_coulomb

    ! Call the helmeos initialization routine and read in the table 
    ! containing the electron contribution.

    call helmeos_init()

    initialized = .true.
 
  end subroutine eos_init



  !---------------------------------------------------------------------------
  ! The main interface
  !---------------------------------------------------------------------------
  subroutine eos(input, state, do_eos_diag_in, pt_index)

    ! A generic wrapper for the Helmholtz electron/positron degenerate EOS.  

    implicit none

    ! Input arguments

    integer,           intent(in   ) :: input
    type (eos_t),      intent(inout) :: state 
    logical, optional, intent(in   ) :: do_eos_diag_in
    integer, optional, intent(in   ) :: pt_index(:)

    ! Local variables and arrays
    
    double precision :: ymass(nspec), ysum, yzsum
    double precision :: e_want, p_want, s_want, h_want

    double precision, parameter :: init_test = -1.0d199

    logical eosfail, do_eos_diag

    integer :: n, dim_ptindex

    ! z_err is used to convert the pt_index information into a string

    character (len=64) :: z_err  

    ! Error messages in case of EOS failure

    character (len=64) :: g_err, n_err, i_err
    character (len=64) :: neg_e_err, neg_p_err, neg_s_err, neg_h_err
    character (len=64) :: eos_input_str


    if (.not. initialized) call bl_error('EOS: not initialized')

    do_eos_diag = .false.

    if (present(do_eos_diag_in)) do_eos_diag = do_eos_diag_in

    write(eos_input_str, '(A13, I1)') ' EOS input = ', input

    g_err = 'EOS: error in the EOS.' // eos_input_str
    n_err = 'EOS: invalid input.' // eos_input_str
    i_err = 'EOS: Newton-Raphson iterations failed to converge.' // eos_input_str

    neg_e_err = 'EOS: energy < 0 in the EOS.' // eos_input_str
    neg_s_err = 'EOS: entropy < 0 in the EOS.' // eos_input_str
    neg_p_err = 'EOS: pressure < 0 in the EOS.' // eos_input_str
    neg_h_err = 'EOS: enthalpy < 0 in the EOS.' // eos_input_str

    ! this format statement is for writing into z_err -- make sure that
    ! the len of z_err can accomodate this format specifier
1001 format(1x,"zone index info: i = ", i5)
1002 format(1x,"zone index info: i = ", i5, '  j = ', i5)
1003 format(1x,"zone index info: i = ", i5, '  j = ', i5, '  k = ', i5)

    if (present(pt_index)) then
 
       dim_ptindex = size(pt_index,dim=1)

       if (dim_ptindex .eq. 1) then 
          write (z_err,1001) pt_index(1)
       else if (dim_ptindex .eq. 2) then 
          write (z_err,1002) pt_index(1), pt_index(2)
       else if (dim_ptindex .eq. 3) then 
          write (z_err,1003) pt_index(1), pt_index(2), pt_index(3)
       end if

    else

      z_err = ''

    endif

    ! Check to make sure the composition was set properly.

    do n = 1, nspec
      if (state % xn(n) .lt. init_test) call bl_error("EOS: species abundances not set.")
    enddo

    ! Get abar, zbar, etc.

    call composition(state, .false.)

    eosfail = .false.

    select case (input)

!---------------------------------------------------------------------------
! dens, temp, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_rt)

       ! Check to ensure that rho and T were initialized.

       if (state % rho .lt. init_test .or. state % T .lt. init_test) then
         call bl_error("EOS called with rho and T as inputs, but these were not initialized.")
       endif

       ! Call the EOS.

       call helmeos(do_coulomb, eosfail, state)

       if (eosfail) call bl_error(g_err, z_err)


!---------------------------------------------------------------------------
! dens, enthalpy, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_rh)

       ! Check to ensure that rho and h were initialized.

       if (state % rho .lt. init_test .or. state % h .lt. init_test) then
         call bl_error("EOS called with rho and h as inputs, but these were not initialized.")
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given enthalpy.

       h_want = state % h

       if (do_eos_diag) print *, 'WANT H ', state % h

       call newton_iter(state, do_eos_diag, g_err, i_err, z_err, 'h', 'T', h_want)



!---------------------------------------------------------------------------
! temp, pres, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_tp)

       if (state % T .lt. init_test .or. state % p .lt. init_test) then
         call bl_error("EOS called with temp and pressure as inputs, but these were not initialized.")
       endif

       ! We want to converge to the given pressure
       p_want = state % p

       if (p_want < ZERO) call bl_error(neg_p_err, z_err)
         
       call newton_iter(state, do_eos_diag, g_err, i_err, z_err, 'p', 'r', p_want)

       if (input_is_constant) state % p = p_want



!---------------------------------------------------------------------------
! dens, pres, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_rp)

       if (state % rho .lt. init_test .or. state % p .lt. init_test) then
         call bl_error("EOS called with rho and pressure as inputs, but these were not initialized.")
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given pressure
       p_want = state % p

       if (do_eos_diag) print *, 'P WANT ', p_want

       if (p_want < ZERO) call bl_error(neg_p_err, z_err)
       
       call newton_iter(state, do_eos_diag, g_err, i_err, z_err, 'p', 'T', p_want)

       if (input_is_constant) state % p = p_want



!---------------------------------------------------------------------------
! dens, energy, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_re)

       if (state % rho .lt. init_test .or. state % e .lt. init_test) then
         call bl_error('EOS called with rho and e as inputs, but these were not initialized.')
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given energy
       e_want = state % e

       if (e_want < ZERO) call bl_error(neg_e_err, z_err)

       if (do_eos_diag) print *, 'WANT e ', e_want

       call newton_iter(state, do_eos_diag, g_err, i_err, z_err, 'e', 'T', e_want)

       if (input_is_constant) state % e = e_want
              


!---------------------------------------------------------------------------
! pres, entropy, and xmass are inputs
!---------------------------------------------------------------------------

    case (eos_input_ps)

       if (state % p .lt. init_test .or. state % s .lt. init_test) then
         call bl_error("EOS called with pressure and entropy as inputs, but these were not initialized.")
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given entropy and pressure
       s_want = state % s
       p_want = state % p

       if (s_want < ZERO) call bl_error(neg_s_err, z_err)

       if (p_want < ZERO) call bl_error(neg_p_err, z_err)

       if (do_eos_diag) then
          print *, 'WANT s ', s_want
          print *, 'WANT p ', p_want
       endif

       call newton_iter2(state, do_eos_diag, g_err, i_err, z_err, 'p', p_want, 's', s_want)

       if (input_is_constant) then
          state % s = s_want
          state % p = p_want
       endif



!---------------------------------------------------------------------------
! pres, enthalpy, and xmass are inputs
!---------------------------------------------------------------------------
    case (eos_input_ph)

       if (state % p .lt. init_test .or. state % h .lt. init_test) then
         call bl_error("EOS called with pressure and enthalpy as inputs, but these were not initialized.")
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given enthalpy and pressure
       s_want = state % s
       h_want = state % h

       if (p_want < ZERO) call bl_error(neg_p_err, z_err)

       if (h_want < ZERO) call bl_error(neg_h_err, z_err)

       if (do_eos_diag) then
          print *, 'WANT p ', p_want
          print *, 'WANT h ', h_want
       endif

       call newton_iter2(state, do_eos_diag, g_err, i_err, z_err, 'p', p_want, 'h', h_want)

       if (input_is_constant) then
          state % p = p_want
          state % h = h_want
       endif


!---------------------------------------------------------------------------
! temp, enthalpy, and xmass are inputs
!---------------------------------------------------------------------------
    case (eos_input_th)

       if (state % t .lt. init_test .or. state % h .lt. init_test) then
         call bl_error("EOS called with temperature and enthalpy as inputs, but these were not initialized.")
       endif

       if (do_eos_diag) print *, 'T/D INIT ', state % T, state % rho

       ! We want to converge to the given enthalpy
       h_want = state % h

       if (h_want < ZERO) call bl_error(neg_h_err, z_err)

       if (do_eos_diag) print *, 'WANT h ', h_want

       call newton_iter(state, do_eos_diag, g_err, i_err, z_err, 'h', 'r', h_want)

       if (input_is_constant) state % h = h_want



!---------------------------------------------------------------------------
! The EOS input doesn't match any of the available options.
!---------------------------------------------------------------------------

    case default 

       call bl_error(n_err, z_err)

    end select



    ! Take care of final housekeeping.

    ! Count the positron contribution in the electron quantities.
    state % xne  = state % xne  + state % xnp
    state % pele = state % pele + state % ppos

    ! Use the non-relativistic version of the sound speed, cs = sqrt(gam_1 * P / rho).
    ! This replaces the relativistic version that comes out of helmeos.

    state % cs = sqrt(state % gam1 * state % p / state % rho)

    ! Get dpdX, dedX, dhdX.

    call composition_derivatives(state, .false.)



    return

  end subroutine eos



  subroutine newton_iter(state, do_eos_diag, g_err, i_err, z_err, var, dvar, f_want)

     implicit none

     type (eos_t),       intent(inout) :: state
     character,          intent(in   ) :: var, dvar
     character (len=64), intent(in   ) :: g_err, i_err, z_err
     double precision,   intent(in   ) :: f_want
     logical,            intent(in   ) :: do_eos_diag

     integer          :: iter
     double precision :: smallx, error, xnew, xtol
     double precision :: f, x, dfdx

     logical :: converged, eosfail

     if (.not. (dvar .eq. 'T' .or. dvar .eq. 'r') ) then
       call bl_error('EOS: Newton iterations can only be done either over T or r.', z_err)
     endif

     converged = .false.

     ! First pass
     call helmeos(do_coulomb, eosfail, state)

     if (eosfail) call bl_error(g_err, z_err)

     xnew = ZERO

     do iter = 1, max_newton

        ! First, figure out what variable we're working with

        if (dvar .eq. 'T') then

          x = state % T

          smallx = smallt
          xtol = ttol

          select case (var)

            case ('p')
              f    = state % p
              dfdx = state % dpdT
            case ('e')
              f    = state % e
              dfdx = state % dedT
            case ('s')
              f    = state % s
              dfdx = state % dsdT
            case ('h')
              f    = state % h
              dfdx = state % dhdT
            case default
              call bl_error('EOS: Newton iterations called with an unrecognized variable.', z_err)

          end select

        else ! dvar == 'r'

          x = state % rho

          smallx = smalld
          xtol = dtol

          select case (var)

            case ('p')
              f    = state % p
              dfdx = state % dpdr
            case ('e')
              f    = state % e
              dfdx = state % dedr
            case ('s')
              f    = state % s
              dfdx = state % dsdr
            case ('h')
              f    = state % h
              dfdx = state % dhdr
            case default
              call bl_error('EOS: Newton iterations called with an unrecognized variable.', z_err)
 
          end select

        endif

        ! Now do the calculation for the next guess for T/rho

        if (do_eos_diag) then
          print *, 'VAR  = ', var , iter, ' f    = ', f
          print *, 'DVAR = ', dvar, iter, ' dfdx = ', dfdx
        endif

        xnew = x - (f - f_want) / dfdx

        if (do_eos_diag) then
          print *, dvar // 'NEW FIRST ', x, ' - ', f - f_want, ' / ', dfdx
        endif

        ! Don't let the temperature/density change by more than a factor of two
        xnew = max(HALF * x, min(xnew, TWO * x))

        ! Don't let us freeze/evacuate
        xnew = max(smallx, xnew)

        if (do_eos_diag) then
          print *, var // 'NEW AFTER ', iter, xnew
        endif

        ! Compute the error

        error = abs( (xnew - x) / x )

        if (error .lt. xtol) then
          converged = .true.
          exit
        endif
        
        ! Store the new temperature/density if we're still iterating

        if (dvar .eq. 'T') then
          state % T    = xnew
        else
          state % rho  = xnew
        endif

        call helmeos(do_coulomb, eosfail, state)

        if (eosfail) call bl_error(g_err, z_err)
        
     enddo

     ! Call error if too many iterations are needed

     if (.not. converged) call bl_error(i_err, z_err)

  end subroutine newton_iter



  subroutine newton_iter2(state, do_eos_diag, g_err, i_err, z_err, var1, f_want, var2, g_want)

     implicit none

     type (eos_t),       intent(inout) :: state
     character,          intent(in   ) :: var1, var2
     character (len=64), intent(in   ) :: g_err, i_err, z_err
     double precision,   intent(in   ) :: f_want, g_want
     logical,            intent(in   ) :: do_eos_diag

     integer          :: iter
     double precision :: error1, error2, fi, gi, rnew, tnew, delr
     double precision :: f, dfdt, dfdr
     double precision :: g, dgdt, dgdr
     double precision :: temp, dens

     logical :: converged, eosfail

     ! Set the appropriate pointers for the variables

     converged = .false.     

     ! First pass
     call helmeos(do_coulomb, eosfail, state)

     if (eosfail) call bl_error(g_err, z_err)

     rnew = ZERO
     tnew = ZERO

     do iter = 1, max_newton

        ! First, figure out which variables we're using
 
        temp = state % T
        dens = state % rho

        select case (var1)

           case ('p')
             f    = state % p
             dfdt = state % dpdT
             dfdr = state % dpdr
           case ('e')
             f    = state % e
             dfdt = state % dedT
             dfdr = state % dedr
           case ('s')
             f    = state % s
             dfdt = state % dsdT
             dfdr = state % dsdr
           case ('h')
             f    = state % h
             dfdT = state % dhdT
             dfdr = state % dhdr
           case default
             call bl_error('EOS: Newton iterations called with an unrecognized variable.', z_err)

         end select

         select case (var2)

           case ('p')
             g    = state % p
             dgdt = state % dpdT
             dgdr = state % dpdr
           case ('e')
             g    = state % e
             dgdt = state % dedT
             dgdr = state % dedr
           case ('s')
             g    = state % s
             dgdt = state % dsdT
             dgdr = state % dsdr
           case ('h')
             g    = state % h
             dgdt = state % dhdT
             dgdr = state % dhdr
           case default
             call bl_error('EOS: Newton iterations called with an unrecognized variable.', z_err)

         end select

         if (do_eos_diag) then
           print *, 'VAR1 ', var1, iter, f
           print *, 'VAR2 ', var2, iter, g
         end if

        ! Two functions, f and g, to iterate over
        fi = f_want - f
        gi = g_want - g

        !
        ! 0 = f + dfdr * delr + dfdt * delt
        ! 0 = g + dgdr * delr + dgdt * delt
        !

        delr = (fi*dgdt - gi*dfdt) / (dgdr*dfdt - dgdt*dfdr)

        rnew = dens + delr

        tnew = temp - (fi + dfdr*delr) / dfdt

        if (do_eos_diag) then
           print *, 'RNEW FIRST ', dens, ' + ', &
                fi*dgdt - gi*dfdt, ' / ', dgdr*dfdt - dgdt*dfdr
           print *, 'TNEW FIRST ', temp, ' - ', &
                fi + dfdr*delr, ' / ', dfdt
        endif

        ! Don't let the temperature or density change by more
        ! than a factor of two
        tnew = max(HALF * temp, min(tnew, TWO * temp))
        rnew = max(HALF * dens, min(rnew, TWO * dens))

        ! Don't let us freeze or evacuate
        tnew = max(smallt, tnew)
        rnew = max(smalld, rnew)

        if (do_eos_diag) then
           print *, 'RNEW AFTER ', iter, rnew
           print *, 'TNEW AFTER ', iter, tnew
        endif

        ! Compute the errors
        error1 = abs( (rnew - dens) / dens )
        error2 = abs( (tnew - temp) / temp )

        if (error1 .LT. dtol .and. error2 .LT. ttol) then
          converged = .true.
          exit
        endif
     
        ! Store the new temperature and density
        state % rho = rnew
        state % T   = tnew
        
        call helmeos(do_coulomb, eosfail, state)

        if (eosfail) call bl_error(g_err, z_err)
        
     enddo

     ! Call error if too many iterations are needed

     if (.not. converged) call bl_error(i_err, z_err)

  end subroutine newton_iter2

end module eos_module
