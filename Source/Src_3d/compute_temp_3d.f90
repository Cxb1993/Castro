      subroutine compute_temp(lo,hi,state,state_l1,state_l2,state_l3, &
                              state_h1,state_h2,state_h3)

      use network, only : nspec, naux
      use eos_module
      use eos_type_module
      use meth_params_module, only : NVAR, URHO, UEDEN, UEINT, UTEMP, &
                                     UFS, UFX, UMX, UMY, UMZ, allow_negative_energy
      use bl_constants_module

      implicit none
      integer         , intent(in   ) :: lo(3),hi(3)
      integer         , intent(in   ) :: state_l1,state_l2,state_l3
      integer         , intent(in   ) :: state_h1,state_h2,state_h3
      double precision, intent(inout) :: state(state_l1:state_h1,state_l2:state_h2,&
                                               state_l3:state_h3,NVAR)

      integer          :: i,j,k
      double precision :: rhoInv
      integer          :: pt_index(3)

      type (eos_t) :: eos_state

      do k = lo(3),hi(3)
      do j = lo(2),hi(2)
      do i = lo(1),hi(1)
        if (state(i,j,k,URHO) <= ZERO) then
           print *,'   '
           print *,'>>> Error: Castro_3d::compute_temp ',i,j,k
           print *,'>>> ... negative density ',state(i,j,k,URHO)
           print *,'    '
           call bl_error("Error:: Castro_3d.f90 :: compute_temp")
        end if
      enddo
      enddo
      enddo

      if (allow_negative_energy.eq.0) then
         do k = lo(3),hi(3)
         do j = lo(2),hi(2)
         do i = lo(1),hi(1)
            if (state(i,j,k,UEINT) <= ZERO) then
                print *,'   '
                print *,'>>> Warning: Castro_3d::compute_temp ',i,j,k
                print *,'>>> ... (rho e) is negative '
                call bl_error("Error:: Castro_3d.f90 :: compute_temp")
            end if
         enddo
         enddo
         enddo
      end if


      !$OMP PARALLEL DO PRIVATE(i,j,k,eos_state,pt_index,rhoInv)
      do k = lo(3),hi(3)
      do j = lo(2),hi(2)
      do i = lo(1),hi(1)

         rhoInv = ONE / state(i,j,k,URHO)

         eos_state % rho = state(i,j,k,URHO)
         eos_state % e   = state(i,j,k,UEINT) * rhoInv
         eos_state % xn  = state(i,j,k,UFS:UFS+nspec-1) * rhoInv
         eos_state % aux = state(i,j,k,UFX:UFX+naux-1) * rhoInv

         eos_state % T   = state(i,j,k,UTEMP) ! Initial guess for EOS

         pt_index(1) = i
         pt_index(2) = j
         pt_index(3) = k

         call eos(eos_input_re, eos_state, .false., pt_index = pt_index)

         state(i,j,k,UTEMP) = eos_state % T

         ! Reset energy in case we floored

         state(i,j,k,UEINT) = state(i,j,k,URHO) * eos_state % e
         state(i,j,k,UEDEN) = state(i,j,k,UEINT) + HALF &
                            * (state(i,j,k,UMX)**2 + state(i,j,k,UMY)**2 &
                            +  state(i,j,k,UMZ)**2) / state(i,j,k,URHO)

      enddo
      enddo
      enddo
      !$OMP END PARALLEL DO

      end subroutine compute_temp
