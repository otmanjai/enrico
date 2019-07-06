#include "enrico/surrogate_heat_driver.h"

#include "enrico/vtk_viz.h"
#include "openmc/xml_interface.h"
#include "surrogates/heat_xfer_backend.h"
#include "xtensor/xadapt.hpp"
#include "xtensor/xbuilder.hpp"
#include "xtensor/xview.hpp"

#include <iostream>

namespace enrico {

SurrogateHeatDriver::SurrogateHeatDriver(MPI_Comm comm,
                                         double pressure_bc,
                                         pugi::xml_node node)
  : HeatFluidsDriver(comm, pressure_bc)
{
  // Determine thermal-hydraulic parameters for solid phase
  clad_inner_radius_ = node.child("clad_inner_radius").text().as_double();
  clad_outer_radius_ = node.child("clad_outer_radius").text().as_double();
  pellet_radius_ = node.child("pellet_radius").text().as_double();
  n_fuel_rings_ = node.child("fuel_rings").text().as_int();
  n_clad_rings_ = node.child("clad_rings").text().as_int();
  n_pins_x_ = node.child("n_pins_x").text().as_int();
  n_pins_y_ = node.child("n_pins_y").text().as_int();
  n_pins_ = n_pins_x_ * n_pins_y_;
  pin_pitch_ = node.child("pin_pitch").text().as_double();

  // Determine thermal-hydraulic parameters for fluid phase
  mass_flowrate_ = node.child("mass_flowrate").text().as_double();
  n_channels_ = (n_pins_x_ + 1) * (n_pins_y_ + 1);

  // check validity of user input
  Expects(clad_inner_radius_ > 0);
  Expects(clad_outer_radius_ > clad_inner_radius_);
  Expects(pellet_radius_ < clad_inner_radius_);
  Expects(n_fuel_rings_ > 0);
  Expects(n_clad_rings_ > 0);
  Expects(n_pins_x_ > 0);
  Expects(n_pins_y_ > 0);
  Expects(pin_pitch_ > 2.0 * clad_outer_radius_);
  Expects(mass_flowrate_ > 0.0);

  // Set pin locations, where the center of the assembly is assumed to occur at
  // x = 0, y = 0. It is also assumed that the rod-boundary separation in the
  // x and y directions is the same and equal to half the pitch.
  double assembly_width_x = n_pins_x_ * pin_pitch_;
  double assembly_width_y = n_pins_y_ * pin_pitch_;

  pin_centers_.resize({n_pins_});
  for (int row = 0; row < n_pins_y_; ++row) {
    for (int col = 0; col < n_pins_x_; ++col) {
      int pin_index = row * n_pins_x_ + col;
      pin_centers_(pin_index, 0) = -assembly_width_x / 2.0 + pin_pitch_ / 2.0 + col * pin_pitch_;
      pin_centers_(pin_index, 1) = assembly_width_y / 2.0 - (pin_pitch_ / 2.0 + row * pin_pitch_);
    }
  }

  // Set channel flow areas using a coolant-centered approach
  channel_areas_.resize({n_channels_});
  double interior_flow_area = pin_pitch_ * pin_pitch_ - M_PI * clad_outer_radius_ * clad_outer_radius_;
  double edge_flow_area = interior_flow_area / 2.0;
  double corner_flow_area = interior_flow_area / 4.0;

  for (int row = 0; row < n_pins_y_ + 1; ++row) {
    for (int col = 0; col < n_pins_x_ + 1; ++col) {
      int channel = channel_index(row, col);

      if ((row == 0 || row == n_pins_y_) && (col == 0 || col == n_pins_x_))
        channel_areas_(channel) = corner_flow_area;
      else if (row == 0 || row == n_pins_y_ || col == 0 || col == n_pins_x_)
        channel_areas_(channel) = edge_flow_area;
      else
        channel_areas_(channel) = interior_flow_area;
    }
  }

  double total_flow_area = 0.0;
  for (const auto area : channel_areas_)
    total_flow_area += area;

  channel_flowrates_.resize({n_channels_});
  for (int i = 0; i < channel_flowrates_.size(); ++i)
    channel_flowrates_(i) = channel_areas_(i) / total_flow_area * mass_flowrate_;

  // Get z values
  // TODO: Switch to get_node_xarray on OpenMC update
  auto z_values = openmc::get_node_array<double>(node, "z");
  z_ = xt::adapt(z_values);
  n_axial_ = z_.size() - 1;

  // Heat equation solver tolerance
  tol_ = node.child("tolerance").text().as_double();

  // Check for visualization input
  if (node.child("viz")) {
    pugi::xml_node viz_node = node.child("viz");
    if (viz_node.attribute("filename")) {
      viz_basename_ = viz_node.attribute("filename").value();
    }

    // if a viz node is found, write final iteration by default
    viz_iterations_ = "final";
    if (viz_node.child("iterations")) {
      viz_iterations_ = viz_node.child("iterations").text().as_string();
    }
    // set other viz values
    if (viz_node.child("resolution")) {
      vtk_radial_res_ = viz_node.child("resolution").text().as_int();
    }
    if (viz_node.child("data")) {
      viz_data_ = viz_node.child("data").text().as_string();
    }
    if (viz_node.child("regions")) {
      viz_regions_ = viz_node.child("regions").text().as_string();
    }
  }

  // Initialize heat transfer solver
  generate_arrays();
};

void SurrogateHeatDriver::generate_arrays()
{
  // Make a radial grid for the clad with equal spacing.
  r_grid_clad_ =
    xt::linspace<double>(clad_inner_radius_, clad_outer_radius_, n_clad_rings_ + 1);

  // Make a radial grid for the fuel with equal spacing.
  r_grid_fuel_ = xt::linspace<double>(0, pellet_radius_, n_fuel_rings_ + 1);

  // Create empty arrays for source term and temperature
  source_ = xt::empty<double>({n_pins_, n_axial_, n_rings()});
  temperature_ = xt::empty<double>({n_pins_, n_axial_, n_rings()});
  density_ = xt::zeros<double>({n_pins_, n_axial_, n_rings()});
  fluid_mask_ = xt::zeros<int>({n_pins_, n_axial_, n_rings()});
}

void SurrogateHeatDriver::solve_step()
{
  solve_heat();
}

void SurrogateHeatDriver::solve_heat()
{
  std::cout << "Solving heat equation...\n";
  // NuScale inlet temperature
  double T_co = 523.15;

  // Set initial temperature
  for (auto& T : temperature_) {
    T = T_co;
  }

  // Convert source to [W/m^3] as expected by Magnolia
  xt::xtensor<double, 3> q = 1e6 * source_;

  // Convert radial grids to [m] as expected by Magnolia
  xt::xtensor<double, 1> r_fuel = 0.01 * r_grid_fuel_;
  xt::xtensor<double, 1> r_clad = 0.01 * r_grid_clad_;

  for (int i = 0; i < n_pins_; ++i) {
    for (int j = 0; j < n_axial_; ++j) {
      solve_steady_nonlin(&q(i, j, 0),
                          T_co,
                          r_fuel.data(),
                          r_clad.data(),
                          n_fuel_rings_,
                          n_clad_rings_,
                          tol_,
                          &temperature_(i, j, 0));
    }
  }
}

xt::xtensor<double, 1> SurrogateHeatDriver::temperature() const
{
  return xt::flatten(temperature_);
}

double SurrogateHeatDriver::temperature(int pin, int axial, int ring) const
{
  return temperature_(pin, axial, ring);
}

xt::xtensor<double, 1> SurrogateHeatDriver::density() const
{
  return xt::flatten(density_);
}

xt::xtensor<int, 1> SurrogateHeatDriver::fluid_mask() const
{
  return xt::flatten(fluid_mask_);
}

void SurrogateHeatDriver::write_step(int timestep, int iteration)
{
  // if called, but viz isn't requested for the situation,
  // exit early - no output
  if ((iteration < 0 && "final" != viz_iterations_) ||
      (iteration >= 0 && "all" != viz_iterations_)) {
    return;
  }

  // otherwise construct an appropriate filename and write the data
  std::stringstream filename;
  filename << viz_basename_;
  if (iteration >= 0 && timestep >= 0) {
    filename << "_t" << timestep << "_i" << iteration;
  }
  filename << ".vtk";

  SurrogateVtkWriter vtk_writer(*this, vtk_radial_res_, viz_regions_, viz_data_);

  std::cout << "Writing VTK file: " << filename.str() << "\n";
  vtk_writer.write(filename.str());
  return;
}

} // namespace enrico
