#ifndef ENRICO_SHIFT_DRIVER_H
#define ENRICO_SHIFT_DRIVER_H

#include <memory> // for shared_ptr
#include <vector>

#include "Geometria/rtk/RTK_Geometry.hh"         // for Geometry
#include "Omnibus/driver/Multiphysics_Driver.hh" // for MultiPhysics_Driver
#include "Shift/mc_physics/SCE_Physics.hh"
#include "Teuchos_ParameterList.hpp"
#include "Teuchos_RCP.hpp"

#include "enrico/geom.h"
#include "enrico/neutronics_driver.h"

namespace enrico {

class ShiftDriverNew : public NeutronicsDriver {
public:
  // Constructor
  ShiftDriverNew(MPI_Comm comm);

  //////////////////////////////////////////////////////////////////////////////
  // NeutronicsDriver interface

  // get the heat source normalized to the given total power
  xt::xtensor<double, 1> heat_source(double power) const final;

  //! Find cells corresponding to a vector of positions
  //! \param positions (x,y,z) coordinates to search for
  //! \return Handles to cells
  std::vector<CellHandle> find(const std::vector<Position>& positions) override;

  //! Set the density of the material in a cell
  //! \param cell Handle to a cell
  //! \param rho Density in [g/cm^3]
  void set_density(CellHandle cell, double rho) const override;

  //! Set the temperature of a cell
  //! \param cell Handle to a cell
  //! \param T Temperature in [K]
  void set_temperature(CellHandle cell, double T) const override;

  //! Get the density of a cell
  //! \param cell Handle to a cell
  //! \return Cell density in [g/cm^3]
  double get_density(CellHandle cell) const override;

  //! Get the temperature of a cell
  //! \param cell Handle to a cell
  //! \return Temperature in [K]
  double get_temperature(CellHandle cell) const override;

  //! Get the volume of a cell
  //! \param cell Handle to a cell
  //! \return Volume in [cm^3]
  double get_volume(CellHandle cell) const override;

  //! Detemrine whether a cell contains fissionable nuclides
  //! \param cell Handle to a cell
  //! \return Whether the cell contains fissionable nuclides
  bool is_fissionable(CellHandle cell) const override;

  std::size_t n_cells() const override { return num_shift_cells_; }

  //! Create energy production tallies
  void create_tallies() override;

  //////////////////////////////////////////////////////////////////////////////
  // Driver interface

  //! Initialization required in each Picard iteration
  void init_step() final;

  //! Runs Shift for one Picard iteration
  void solve_step() final;

  //! Writes Shift output for given timestep and iteration
  //! \param timestep timestep index
  //! \param iteration iteration index
  void write_step(int timestep, int iteration) final;

  //! Finalization required in each Picard iteration
  void finalize_step() final;

private:
  std::shared_ptr<geometria::RTK_Core> geometry_;        //!< Core model
  std::shared_ptr<omnibus::Multiphysics_Driver> driver_; //!< Multiphysics driver

  std::vector<int> matids_; //!< Matids corresponding to T/H mesh elements
  int num_shift_cells_;     //!< Number of Shift cells
};

} // end namespace enrico

#endif // ENRICO_SHIFT_DRIVER_H
