     subroutine ca_estdt(u,u_l1,u_l2,u_l3,u_h1,u_h2,u_h3,lo,hi,dx,dt)

     use network, only : nspec, naux
     use eos_module
     use eos_type_module
     use meth_params_module, only : NVAR, URHO, UMX, UMY, UMZ, UEINT, UESGS, UTEMP, UFS, UFX, &
                                    allow_negative_energy
     use bl_constants_module

     implicit none

     integer          :: u_l1,u_l2,u_l3,u_h1,u_h2,u_h3
     integer          :: lo(3), hi(3)
     double precision :: u(u_l1:u_h1,u_l2:u_h2,u_l3:u_h3,NVAR)
     double precision :: dx(3), dt

     double precision :: rhoInv,ux,uy,uz,c,dt1,dt2,dt3
     double precision :: sqrtK,grid_scl,dt4
     integer          :: i,j,k
     integer          :: pt_index(3)

     type (eos_t) :: eos_state

     grid_scl = (dx(1)*dx(2)*dx(3))**THIRD

     ! Translate to primitive variables, compute sound speed (call eos)
     !$OMP PARALLEL DO PRIVATE(i,j,k,rhoInv,ux,uy,uz,sqrtK,eos_state,pt_index,dt1,dt2,dt3) REDUCTION(min:dt)
     do k = lo(3),hi(3)
         do j = lo(2),hi(2)
            do i = lo(1),hi(1)

               rhoInv = ONE / u(i,j,k,URHO)

               ux = u(i,j,k,UMX) * rhoInv
               uy = u(i,j,k,UMY) * rhoInv
               uz = u(i,j,k,UMZ) * rhoInv

               ! Use internal energy for calculating dt 
               eos_state % e  = u(i,j,k,UEINT)*rhoInv

               if (UESGS .gt. -1) &
                  sqrtK = dsqrt( rhoInv*u(i,j,k,UESGS) )

               ! Protect against negative e
               if (eos_state % e .gt. ZERO .or. allow_negative_energy .eq. 1) then
                  eos_state % rho = u(i,j,k,URHO)
                  eos_state % T   = u(i,j,k,UTEMP)
                  eos_state % xn  = u(i,j,k,UFS:UFS+nspec-1) * rhoInv
                  eos_state % aux = u(i,j,k,UFX:UFX+naux-1) * rhoInv

                  pt_index(1) = i
                  pt_index(2) = j
                  pt_index(3) = k
 
                  call eos(eos_input_re, eos_state, .false., pt_index = pt_index)

                  c = eos_state % cs
               else
                  c = ZERO
               end if

               dt1 = dx(1)/(c + abs(ux))
               dt2 = dx(2)/(c + abs(uy))
               dt3 = dx(3)/(c + abs(uz))
               dt = min(dt,dt1,dt2,dt3)

               ! Now let's check the diffusion terms for the SGS equations
               if (UESGS .gt. -1) then

                  ! First for the term in the momentum equation
                  ! This is actually dx^2 / ( 6 nu_sgs )
                  ! Actually redundant as it takes the same form as below with different coeff
                  ! dt4 = grid_scl / ( 0.42d0 * sqrtK )

                  ! Now for the term in the K equation itself
                  ! nu_sgs is 0.65
                  ! That gives us 0.65*6 = 3.9
                  ! Using 4.2 to be conservative (Mach1-256 broke during testing with 3.9)
                  !               dt4 = grid_scl / ( 3.9d0 * sqrtK )
                  dt4 = grid_scl / ( 4.2d0 * sqrtK )
                  dt = min(dt,dt4)

               end if

            enddo
         enddo
     enddo
     !$OMP END PARALLEL DO

     end subroutine ca_estdt
