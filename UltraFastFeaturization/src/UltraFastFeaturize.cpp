#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <H5Cpp.h>
#include <string>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
//#include <Eigen/Dense>

#include "UltraFastFeaturize.h"

#include "uf3_bspline_basis3.h"
#include "uf3_bspline_basis2.h"

#include "UltraFastNeighs.h"

namespace py = pybind11;

UltraFastFeaturize::UltraFastFeaturize(int _degree,
                             int _nelements,
                             py::tuple _interactions_map,
                             py::array_t<double, py::array::c_style> _n2b_knots_map,
                             py::array_t<int, py::array::c_style> _n2b_num_knots,
                             py::array_t<double, py::array::c_style> _n3b_knots_map,
                             py::array_t<int, py::array::c_style> _n3b_num_knots,
                             py::array_t<int, py::array::c_style> _n3b_symm_array,
                             py::array_t<int, py::array::c_style> _n3b_feature_sizes)
        : degree(_degree), nelements(_nelements), 
          interactions_map(_interactions_map),
          n2b_knots_map(_n2b_knots_map),
          n2b_num_knots(_n2b_num_knots),
          n3b_knots_map(_n3b_knots_map),
          n3b_num_knots(_n3b_num_knots),
          n3b_symm_array(_n3b_symm_array),
          n3b_feature_sizes(_n3b_feature_sizes),
          // As bspline_config_ff conatins const members, we have to use the
          // copy constructor of 'bspline_config_ff' and initialize the 
          // BsplineConfig here
          BsplineConfig(degree, nelements, interactions_map, n2b_knots_map, n2b_num_knots,
                        n3b_knots_map, n3b_num_knots, n3b_symm_array, n3b_feature_sizes)
{
  num_of_interxns = {static_cast<int>(this->BsplineConfig.n2b_interactions), 
                     static_cast<int>(this->BsplineConfig.n3b_interactions)};
  
  n2b_types = this->BsplineConfig.n2b_types;
  if (this->BsplineConfig.degree ==3)
    n3b_types = this->BsplineConfig.n3b_types;

  elements = std::vector<int> (nelements,0);
  for (int i=0; i<nelements; i++)
    elements[i] = interactions_map[i].cast<int>();

  rmin_max_2b_sq = this->BsplineConfig.rmin_max_2b_sq;
  rcut_max_sq = this->BsplineConfig.rcut_max_sq;
  if (this->BsplineConfig.degree ==3)
    rmin_max_3b = this->BsplineConfig.rmin_max_3b;

  ////Create 2b bspline basis constant
  auto n2b_knots_map_un = n2b_knots_map.unchecked<2>();
  auto n2b_num_knots_un = n2b_num_knots.unchecked<1>();
  //Find max_num_2b_knots
  int max_num_2b_knots = 0;
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++)
    max_num_2b_knots = std::max(max_num_2b_knots, n2b_num_knots_un(interxn));

  //Calculate constants in 2b bsline basis set
  //Create constants array
  constants_2b.resize(num_of_interxns[0]);
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++){
    constants_2b[interxn].resize(max_num_2b_knots-4);
    for (int knot_no = 0; knot_no < max_num_2b_knots-4; knot_no++)
      constants_2b[interxn][knot_no].resize(16,0);
  }
  //Calculate constants in derivative of 2b bsline basis set --> for forces
  //Create dnconstants array
  constants_2b_deri1.resize(num_of_interxns[0]);
  //constants_2b_deri2.resize(num_of_interxns[0]);
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++){
    constants_2b_deri1[interxn].resize(max_num_2b_knots-4);
    //constants_2b_deri2[interxn].resize(max_num_2b_knots-4);
    for (int knot_no = 0; knot_no < max_num_2b_knots-4; knot_no++){
      constants_2b_deri1[interxn][knot_no].resize(9,0);
      //constants_2b_deri2[interxn][knot_no].resize(9,0);
    }
  }


  n2b_num_knots_array = std::vector<int> (num_of_interxns[0], 0);
  n2b_knots_array = std::vector<std::vector<double>> (num_of_interxns[0], std::vector<double>(max_num_2b_knots));

  reprn_length = 1+nelements;
  //2b
  for (int interxn=0; interxn < num_of_interxns[0]; interxn++) {
    //clamped knots --> -4
    for (int knot_no = 0; knot_no < n2b_num_knots_un(interxn) - 4; knot_no++) {
      double *temp_knots = new double[5] {n2b_knots_map_un(interxn,knot_no),
                                          n2b_knots_map_un(interxn,knot_no+1),
                                          n2b_knots_map_un(interxn,knot_no+2),
                                          n2b_knots_map_un(interxn,knot_no+3),
                                          n2b_knots_map_un(interxn,knot_no+4)};

      std::vector<double> c = get_constants(temp_knots);

      for (int i=0; i < 16; i++)
        constants_2b[interxn][knot_no][i] = (std::isinf(c[i]) || 
                                             std::isnan(c[i])) ? 0 : c[i];
      delete[] temp_knots;
    }
    //for derivatives
    for (int knot_no = 1; knot_no < n2b_num_knots_un(interxn) - 4; knot_no++) {
      // 3 intervals, 4 knots
      double *temp_knots1 = new double[4] {n2b_knots_map_un(interxn,knot_no),
                                          n2b_knots_map_un(interxn,knot_no+1),
                                          n2b_knots_map_un(interxn,knot_no+2),
                                          n2b_knots_map_un(interxn,knot_no+3)};
      

      std::vector<double> c1 = get_dnconstants(temp_knots1,
                                               3/(temp_knots1[3]-temp_knots1[0]));


      for (int i=0; i < 9; i++){
        constants_2b_deri1[interxn][knot_no][i] = (std::isinf(c1[i]) || 
                                             std::isnan(c1[i])) ? 0 : c1[i];
      }
      delete[] temp_knots1;// temp_knots2;
      
    }

    n2b_num_knots_array[interxn] = n2b_num_knots_un(interxn);
    for (int knot_no = 0; knot_no < max_num_2b_knots; knot_no++)
      n2b_knots_array[interxn][knot_no] = n2b_knots_map_un(interxn,knot_no);

    reprn_length = reprn_length + n2b_num_knots_un(interxn)-4;
  }
  tot_2b_features_size = reprn_length - 1 - nelements;
  //3b
  ////Create 3b bspline basis constant
  //Find max_num_3b_knots
  int max_num_3b_knots = 0;
  if (this->BsplineConfig.degree ==3) {
    auto n3b_knots_map_un = n3b_knots_map.unchecked<3>();
    auto n3b_num_knots_un = n3b_num_knots.unchecked<2>();
    auto n3b_feature_sizes_un = n3b_feature_sizes.unchecked<1>();

    for (int interxn=0; interxn < num_of_interxns[1]; interxn++) {
      max_num_3b_knots = std::max(max_num_3b_knots, n3b_num_knots_un(interxn,0));
      max_num_3b_knots = std::max(max_num_3b_knots, n3b_num_knots_un(interxn,1));
      max_num_3b_knots = std::max(max_num_3b_knots, n3b_num_knots_un(interxn,2));
    }

    constants_3b = std::vector<std::vector<std::vector<std::vector<double>>>>
                    (num_of_interxns[1], 
                     std::vector<std::vector<std::vector<double>>>(3, 
                         std::vector<std::vector<double>>(max_num_3b_knots-4,
                             std::vector<double>(16,0)))); 

    constants_3b_deri = std::vector<std::vector<std::vector<std::vector<double>>>>
                    (num_of_interxns[1], 
                     std::vector<std::vector<std::vector<double>>>(3, 
                         std::vector<std::vector<double>>(max_num_3b_knots-4,
                             std::vector<double>(9,0))));

    n3b_num_knots_array = std::vector<std::vector<int>> (num_of_interxns[1], 
            std::vector<int>(3, 0));
    n3b_knots_array = 
        std::vector<std::vector<std::vector<double>>> (num_of_interxns[1],
                std::vector<std::vector<double>>(3, 
                    std::vector<double>(max_num_3b_knots, 0)));

    for (int interxn=0; interxn < num_of_interxns[1]; interxn++) {
      n3b_num_knots_array[interxn][0] = n3b_num_knots_un(interxn,0); 
      n3b_num_knots_array[interxn][1] = n3b_num_knots_un(interxn,1); 
      n3b_num_knots_array[interxn][2] = n3b_num_knots_un(interxn,2);

      for (int i=0; i < 3; i++) {
        for (int knot_no = 0; knot_no < max_num_3b_knots; knot_no++)
          n3b_knots_array[interxn][i][knot_no] = n3b_knots_map_un(interxn,i,knot_no);

        for (int knot_no = 0; knot_no < n3b_num_knots_un(interxn,i)-4; knot_no++) {
          double *temp_knots = new double[5] {n3b_knots_map_un(interxn,i,knot_no),
                                              n3b_knots_map_un(interxn,i,knot_no+1),
                                              n3b_knots_map_un(interxn,i,knot_no+2),
                                              n3b_knots_map_un(interxn,i,knot_no+3),
                                              n3b_knots_map_un(interxn,i,knot_no+4)};

          std::vector<double> c = get_constants(temp_knots);

          for (int j=0; j < 16; j++)
            constants_3b[interxn][i][knot_no][j] = (std::isinf(c[j]) ||
                                                    std::isnan(c[j])) ? 0 : c[j];
    
          delete[] temp_knots;
        }
      }
      
      //for derivative
      for (int i=0; i < 3; i++) {
        for (int knot_no = 1; knot_no < n3b_num_knots_un(interxn,i) - 4; knot_no++) {
          // 3 intervals, 4 knots
          double *temp_knots1 = new double[4] {n3b_knots_map_un(interxn,i,knot_no),
                                               n3b_knots_map_un(interxn,i,knot_no+1),
                                               n3b_knots_map_un(interxn,i,knot_no+2),
                                               n3b_knots_map_un(interxn,i,knot_no+3)};
          std::vector<double> c1 = get_dnconstants(temp_knots1,
                                                    3/(temp_knots1[3]-temp_knots1[0]));
          for (int j=0; j < 9; j++)
              constants_3b_deri[interxn][i][knot_no][j] = (std::isinf(c1[j]) || 
                                                            std::isnan(c1[j])) ? 0 : c1[j];
          delete[] temp_knots1;
        }
      }

      //int num_knot_ij = n3b_num_knots_array_un(interxn,)
      reprn_length = reprn_length + n3b_feature_sizes_un(interxn);
    }

    //get symmetry weights
    auto n3b_symm_array_un = n3b_symm_array.unchecked<1>();
    flat_weights.resize(reprn_length-tot_2b_features_size-1-nelements);
    template_mask.resize(reprn_length-tot_2b_features_size-1-nelements);
    int temp_count = 0;
    for (int interxn=0; interxn < num_of_interxns[1]; interxn++) {
      std::vector<double> template_array_flatten = 
          this->BsplineConfig.get_symmetry_weights(n3b_symm_array_un(interxn),
                                                n3b_knots_array[interxn][0],
                                                n3b_knots_array[interxn][1],
                                                n3b_knots_array[interxn][2],
                                                n3b_num_knots_un(interxn,0),
                                                n3b_num_knots_un(interxn,1),
                                                n3b_num_knots_un(interxn,2),
                                                0,
                                                0);
      for (int i=0; i<template_array_flatten.size(); i++){
        if (template_array_flatten[i]>0){
          flat_weights[temp_count] = template_array_flatten[i];
          template_mask[temp_count] = i;
          temp_count++;
        }
      } 
    }
  } //if degree==3
}

UltraFastFeaturize::~UltraFastFeaturize()
{}
//TODO: In the current implementation the data coming from python is getting copied
//Instead we can use py::buffer, this will allow access to the data without copying
/*Eg. void process_array(py::buffer b) {
    py::buffer_info info = b.request(); // Request buffer info

    // Check if the buffer is indeed a NumPy array or compatible
    if (info.format != py::format_descriptor<float>::format())
        throw std::runtime_error("Incompatible format; expected a float array.");

    // Access the buffer's data pointer directly
    float *data = static_cast<float *>(info.ptr);
    size_t N = info.size;

    // Example processing: scale each element in the array
    for (size_t i = 0; i < N; i++) {
        data[i] *= 2.0;
    }
}*/
//One of the requirement of the above is that the numpy array in python is Contiguous
//ie arr_contiguous = np.ascontiguousarray(arr)
void UltraFastFeaturize::set_geom_data(py::array_t<double, py::array::c_style> _atoms_array,
                                  py::array_t<double, py::array::c_style> _energy_array,
                                  py::array_t<double, py::array::c_style> _forces_array,
                                  py::array_t<double, py::array::c_style> _cell_array,
                                  py::array_t<int, py::array::c_style> _crystal_index,
                                  py::array_t<int, py::array::c_style> _supercell_factors,
                                  py::array_t<int, py::array::c_style> _geom_posn,
                                  py::list _structure_names, py::list _column_names)
                                  //py::array_t<double, py::array::c_style> _supercell_array)
{
  atoms_array = _atoms_array;
  energy_array = _energy_array;
  forces_array = _forces_array;
  cell_array = _cell_array;
  crystal_index = _crystal_index;
  supercell_factors = _supercell_factors;
  geom_posn = _geom_posn;
  auto geom_posn_un = geom_posn.unchecked<1>();

  //atoms_array.shape[0] == crystal_index.shape[0]
  if (atoms_array.shape(0) != crystal_index.shape(0))
    throw std::length_error("atoms_array.shape[0] != crystal_index.shape[0]");

  //Check if len(atoms_array) == geom_posn[-1]
  if (atoms_array.shape(0) != geom_posn_un(geom_posn.shape(0)-1))
    throw std::length_error("atoms_array.shape[0] != geom_posn[geom_posn.shape[0]-1]");
 
  //Check if forces_array.shape[0] == atoms_array.shape[0]
  if (atoms_array.shape(0) != forces_array.shape(0))
    throw std::length_error("atoms_array.shape[0] != forces_array.shape[0]");

  //cell_array.shape[0] == supercell_factors.shape[0] == geom_posn.shape[0]-1
  if (cell_array.shape(0) != supercell_factors.shape(0) && 
          supercell_factors.shape(0) != geom_posn.shape(0)-1)
    throw std::length_error("cell_array.shape[0] != supercell_factors.shape[0] != geom_posn.shape[0]-1");

  if (cell_array.shape(0) != energy_array.shape(0))
    throw std::length_error("cell_array.shape[0] != energy_array.shape[0]");

  //auto supercell_factors_un = supercell_factors.unchecked<2>();

  //Loop over all crystal structures and find the max num of neigh an atom
  //can have

  if (cell_array.shape(0) !=  _structure_names.size())
    throw std::length_error("structure_names is not of the right length");

  for (const auto &item : _structure_names) {
    if (py::isinstance<py::str>(item)) 
      structure_names.push_back(item.cast<std::string>());
    else 
      throw std::runtime_error("List item in structure_names is not string");
  }

  if (_column_names.size() != reprn_length)
    throw std::length_error("column_names is not of the right length");

  for (const auto &item : _column_names) {
    if (py::isinstance<py::str>(item))
      column_names.push_back(item.cast<std::string>());
    else
      throw std::runtime_error("List item in column_names is not string");
  }

}

py::array UltraFastFeaturize::featurize(int _batch_size, bool return_Neigh,
                                    std::string& _filename, bool featurize_3b)
{
  if (featurize_3b) {
    if (this->BsplineConfig.degree !=3)
      throw std::domain_error("featurize_3b is True but bspling_config contains\n\
              no 3body info! I refuse to continue!");
  }
  else {
    if (this->BsplineConfig.degree ==3)
      throw std::domain_error("featurize_3b is false but bspling_config contains\n\
              3body info! I refuse to continue!");
  }

  incomplete = false;
  //Create Neighs to store distance of neighbors by interaction
  //Remember Neighs has to be contigous as we are returning a 3d numpy array
  //in std::vectors only the first dimension is garunteed to be contigous

  //set batch_size
  batch_size = _batch_size;
  //change batch_size if needed
  if (static_cast<int>(atoms_array.shape(0)) < batch_size)
    batch_size = static_cast<int>(atoms_array.shape(0));

  //Handle filename
  filename = _filename;
  std::ifstream file(filename);
  if (file.good()){
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    // Format time
    std::stringstream ss;
    ss << std::put_time(std::localtime(&now_c), "%Y_%m_%d_%H_%M_%S");
    std::string datetime = ss.str();
    // Append formatted time to filename
    filename = filename + "_" + datetime;
  }
  //Create HDF5 file
  H5::H5File feature_file(filename, H5F_ACC_TRUNC);
  write_hdf5_counter=0;

  //Get unchecked proxy object for fast access
  auto atoms_array_un = atoms_array.unchecked<2>();
  auto energy_array_un = energy_array.unchecked<1>();
  auto forces_array_un = forces_array.unchecked<2>(); 
  auto crystal_index_un = crystal_index.unchecked<1>();
  auto cell_array_un = cell_array.unchecked<3>();
  auto geom_posn_un = geom_posn.unchecked<1>();
  auto supercell_factors_un = supercell_factors.unchecked<2>();
  auto n3b_symm_array_un = n3b_symm_array.unchecked<1>();
  auto n3b_feature_sizes_un = n3b_feature_sizes.unchecked<1>();

  UltraFastNeighs ultra_fast_neighs = UltraFastNeighs(atoms_array_un,
                                                      crystal_index_un, 
                                                      cell_array_un,
                                                      geom_posn_un,
                                                      supercell_factors_un,
                                                      num_of_interxns,
                                                      n2b_types,
                                                      rmin_max_2b_sq,
                                                      rcut_max_sq);

  ///////Create Neigh list

  // Total number of batches
  num_batches =  static_cast<int>(std::ceil(static_cast<double>(atoms_array_un.shape(0))/batch_size));

  //Declare dimesnions of Neigh
  int depth, rows, cols;

  // Loop over batches
  for (int batch_numb =0; batch_numb<num_batches; batch_numb++) {

    //Get batch_start, batch_end; these indices 
    //are for atoms_array
    int batch_start = batch_numb*batch_size;
    int batch_end = batch_start+batch_size;
    if (batch_end>=atoms_array_un.shape(0)) {
      batch_end = static_cast<int>(atoms_array_un.shape(0));
      batch_size = batch_end-batch_start;
    }

    //Decide the depth of Neighs
    depth = batch_size;

    //Decide rows in Neigh
    //rows = static_cast<int>(this->BsplineConfig.n2b_interactions);
    rows = num_of_interxns[0];

    //Decide max_num_neigh in this batch
    //Loop over all crystal structures in this batch and find the max num of 
    //neigh an atom can have
    max_num_neigh = 0;
    neigh_in_sphere = 0;
    double max_sphere_vol = sqrt(rcut_max_sq)+1.5;
    max_sphere_vol = 4*3.141593*max_sphere_vol*max_sphere_vol*max_sphere_vol/3;
    /*for (int i=0; i<num_of_interxns[0]; i++) {
      double r = sqrt(rmin_max_2b_sq[2*i+1])+1.5; //Add extra thickness
      double vol = 4*3.141593*r*r*r/3;
      max_sphere_vol = std::max(max_sphere_vol,vol);
    }*/
    //get crystal index of all the atoms in this batch;  using the crystal index 
    //get the supercell_factors of all the atoms in this batch
    for (int i=crystal_index_un(batch_start); i<=crystal_index_un(batch_end-1); i++){
      int supercell_size = supercell_factors_un(i,0)*supercell_factors_un(i,1)*supercell_factors_un(i,2);
      supercell_size = supercell_size*(geom_posn_un(i+1)-geom_posn_un(i));
      max_num_neigh = std::max(max_num_neigh,supercell_size);
      
      int start_posn = geom_posn_un(i);
      int end_posn = geom_posn_un(i+1);
      int num_atoms = end_posn-start_posn;
      
      double vol = (cell_array_un(i,0,0)*
                    ((cell_array_un(i,1,1)*cell_array_un(i,2,2)) - 
                     (cell_array_un(i,1,2)*cell_array_un(i,2,1)))) -
                   (cell_array_un(i,0,1)*
                    ((cell_array_un(i,1,0)*cell_array_un(i,2,2)) - 
                     (cell_array_un(i,1,2)*cell_array_un(i,2,0)))) +
                   (cell_array_un(i,0,2)*
                    ((cell_array_un(i,1,0)*cell_array_un(i,2,1)) - 
                     (cell_array_un(i,1,1)*cell_array_un(i,2,0))));
      neigh_in_sphere = std::max(neigh_in_sphere, max_sphere_vol*num_atoms/vol);
    }
    if (neigh_in_sphere<1.0)
      neigh_in_sphere = 100;
    //cols = static_cast<int>(max_num_neigh);
    cols = static_cast<int>(std::ceil(neigh_in_sphere));
    
    ////----------------Find Neighs-------------------////
    //If Neighs has never been initalized
    if (Neighs.empty()) {
      Neighs = std::vector<double>(depth*rows*cols,0);
      Neighs_del = std::vector<double>(depth*rows*cols*3,0);
      Tot_num_Neighs = std::vector<int>(depth*rows,0);
    }
    
    //Clear and shrink any existing Neighs
    else {
      Neighs.clear();
      Neighs.shrink_to_fit();
      Neighs.resize(depth*rows*cols);
      std::fill(Neighs.begin(), Neighs.end(), 0);

      Neighs_del.clear();
      Neighs_del.shrink_to_fit();
      Neighs_del.resize(depth*rows*cols*3);
      std::fill(Neighs_del.begin(), Neighs_del.end(), 0);

      Tot_num_Neighs.clear();
      Tot_num_Neighs.shrink_to_fit();
      Tot_num_Neighs.resize(depth*rows,0);
      std::fill(Tot_num_Neighs.begin(), Tot_num_Neighs.end(), 0);
    }

    //Find Neighs and set them in Neighs
    ultra_fast_neighs.set_Neighs(batch_start, batch_end, Neighs, Neighs_del,
                                Tot_num_Neighs, rows, cols);

    ////----------------ALL Neighs have been found---////
       
    ////----------------Find Representation---------////
    //Create array to store representation of this batch
    //Assign Representation
    //atomic_Reprn = std::vector<double> (batch_size*reprn_length);
    if (atomic_Reprn.empty()) {
      //atomic_Reprn = std::vector<double> (batch_size*reprn_length, 0);
      atomic_Reprn = std::vector<double> (batch_size*4*reprn_length, 0);
      //1 energy and 3 forces
    }
    //Clear and shrink any existing atomic_Reprn
    else {
      atomic_Reprn.clear();
      atomic_Reprn.shrink_to_fit();
      //atomic_Reprn.resize(batch_size*reprn_length);
      atomic_Reprn.resize(batch_size*4*reprn_length);
      std::fill(atomic_Reprn.begin(), atomic_Reprn.end(), 0);
    }
    
    ////----------------Find 2b Representation------////
    //2b representation is of type atomic, ie each atom has representation
    //In the end all atomic representation are added for 
    //atoms in the same crystal giving the crystal representation
    //ie batch representation
    //NOTE: This crystal representation can be incomplete as atoms 
    //from the same crystal can be split over different batches

    ////2b loop
    //Loop over central atoms in this batch
    for (int atom1=batch_start; atom1<batch_end; atom1++) {
      //int d = atom1-batch_start; //d=0 is the first atom in this batch
      int d = 4*(atom1-batch_start);
      int Z1 = atoms_array_un(atom1,0);
      for (int i=0; i<nelements; i++)
         if (Z1 == elements[i])
           atomic_Reprn[d*reprn_length+1+i] += 1;

      atomic_Reprn[(d+1)*reprn_length+0] = forces_array_un(atom1,0);
      atomic_Reprn[(d+2)*reprn_length+0] = forces_array_un(atom1,1);
      atomic_Reprn[(d+3)*reprn_length+0] = forces_array_un(atom1,2);

      
      //loop over interactions
      int n2b_interactions = num_of_interxns[0]; //total 2-body interactions
      int basis_start_posn = (1 + nelements); //update basis start pos for every interxn
      for (int interxn=0; interxn<n2b_interactions; interxn++){
        const std::vector<double> &knots = n2b_knots_array[interxn];
        const int num_knots = n2b_num_knots_array[interxn];
        const int num_neighs = Tot_num_Neighs[(d/4)*rows+interxn];
        double rmin = sqrt(rmin_max_2b_sq[2*interxn]);
        double rmax = sqrt(rmin_max_2b_sq[2*interxn+1]);

        //loop over all neighs of atom1 for interxn
        for (int atom2=0; atom2<num_neighs; atom2++){ 
          double r = Neighs[(d/4)*(rows*cols)+(interxn*cols)+atom2];

          if ((rmin <= r) && (r < rmax)) {
          double rsq = r*r;
          double rth = rsq*r;
        
          int knot_posn = num_knots-4;
          while (r<=knots[knot_posn])
            knot_posn--;

          int basis_posn = basis_start_posn+knot_posn;
          atomic_Reprn[d*reprn_length+basis_posn] += 
              (constants_2b[interxn][knot_posn][0] +
              (r*constants_2b[interxn][knot_posn][1]) +
              (rsq*constants_2b[interxn][knot_posn][2]) +
              (rth*constants_2b[interxn][knot_posn][3]));

          atomic_Reprn[d*reprn_length+basis_posn-1] += 
              (constants_2b[interxn][knot_posn-1][4] +
              (r*constants_2b[interxn][knot_posn-1][5]) +
              (rsq*constants_2b[interxn][knot_posn-1][6]) +
              (rth*constants_2b[interxn][knot_posn-1][7]));

          atomic_Reprn[d*reprn_length+basis_posn-2] += 
              (constants_2b[interxn][knot_posn-2][8] +
              (r*constants_2b[interxn][knot_posn-2][9]) +
              (rsq*constants_2b[interxn][knot_posn-2][10]) +
              (rth*constants_2b[interxn][knot_posn-2][11]));

          atomic_Reprn[d*reprn_length+basis_posn-3] +=
              (constants_2b[interxn][knot_posn-3][12] +
              (r*constants_2b[interxn][knot_posn-3][13]) +
              (rsq*constants_2b[interxn][knot_posn-3][14]) +
              (rth*constants_2b[interxn][knot_posn-3][15]));
          
          double basis1, basis2, basis3;

          basis1 = (constants_2b_deri1[interxn][knot_posn][0] + 
                  r*constants_2b_deri1[interxn][knot_posn][1] + 
                  rsq*constants_2b_deri1[interxn][knot_posn][2]);

          basis2 = (constants_2b_deri1[interxn][knot_posn-1][3] + 
                  r*constants_2b_deri1[interxn][knot_posn-1][4] + 
                  rsq*constants_2b_deri1[interxn][knot_posn-1][5]);

          basis3 = (constants_2b_deri1[interxn][knot_posn-2][6] + 
                  r*constants_2b_deri1[interxn][knot_posn-2][7] + 
                  rsq*constants_2b_deri1[interxn][knot_posn-2][8]);

          //fpair
          //Don't know why the factor of 2 but for some reason I get the right answer
          double *fpair = new double[4];
          fpair[0] = 2*basis1;
          fpair[1] = 2*(basis2-basis1);
          fpair[2] = 2*(basis3-basis2);
          fpair[3] = -2*basis3;

          int temp_index2 = ((d/4)*(rows*cols*3))+(interxn*cols*3)+(atom2*3);
          double delx = Neighs_del[temp_index2];
          double dely = Neighs_del[temp_index2+1];
          double delz = Neighs_del[temp_index2+2];

          //fx
          atomic_Reprn[(d+1)*reprn_length+basis_posn] +=(fpair[0]*delx);
          atomic_Reprn[(d+1)*reprn_length+basis_posn-1] +=(fpair[1]*delx);
          atomic_Reprn[(d+1)*reprn_length+basis_posn-2] +=(fpair[2]*delx);
          atomic_Reprn[(d+1)*reprn_length+basis_posn-3] +=(fpair[3]*delx);
          
          //fy
          atomic_Reprn[(d+2)*reprn_length+basis_posn] +=(fpair[0]*dely);
          atomic_Reprn[(d+2)*reprn_length+basis_posn-1] +=(fpair[1]*dely);
          atomic_Reprn[(d+2)*reprn_length+basis_posn-2] +=(fpair[2]*dely);
          atomic_Reprn[(d+2)*reprn_length+basis_posn-3] +=(fpair[3]*dely);

          //fz
          atomic_Reprn[(d+3)*reprn_length+basis_posn] +=(fpair[0]*delz);
          atomic_Reprn[(d+3)*reprn_length+basis_posn-1] +=(fpair[1]*delz);
          atomic_Reprn[(d+3)*reprn_length+basis_posn-2] +=(fpair[2]*delz);
          atomic_Reprn[(d+3)*reprn_length+basis_posn-3] +=(fpair[3]*delz);

          delete[] fpair;
          } //rmin_sq, rmax_sq

        }// End of loop over neighs of atom1 for interxn
        basis_start_posn += (num_knots-4);
      }// End of interx loop
    } //End of atom1 loop
    
    if (featurize_3b) { 
      ////3b loop
      int n2b_interactions = num_of_interxns[0];
      int n3b_interactions = num_of_interxns[1];
      //Loop over central atoms in this batch
      tempftest.resize((3*(n3b_num_knots_array[0][0]-4)*
                        (n3b_num_knots_array[0][1]-4)*
                        (n3b_num_knots_array[0][2]-4)), 0);
      for (int atom1=batch_start; atom1<batch_end; atom1++){
        int d = atom1-batch_start;
        int Z1 = atoms_array_un(atom1,0);
        //loop over 3b interaction
        int index_for_symm_weights = 0;
        int basis_start_posn = 1 + nelements + tot_2b_features_size;
        for (int interxn=0; interxn<n3b_interactions; interxn++){
          //get template_mask start and end index for this interxn
          int template_mask_start = index_for_symm_weights;
          int template_mask_end = template_mask_start+n3b_feature_sizes_un(interxn);
          
          //get 3b symmetry
          int n3b_symmetry = n3b_symm_array_un(interxn);
          
          //get Neigh list indices for this 3b interaction --> index_ij, index_ik
          std::vector<int> ij_pair {n3b_types[3*interxn], n3b_types[3*interxn+1]};
          std::vector<int> ik_pair {n3b_types[3*interxn], n3b_types[3*interxn+2]};

          int index_ij = -1;
          int index_ik = -1;

          //TODO: Find out why we need inv_ij and inv_ik
          int inv_ij = 0;
          int inv_ik = 0;

          for (int interxn_2b=0; interxn_2b<n2b_interactions; interxn_2b++){
              std::vector<int> pair {n2b_types[2*interxn_2b], 
                                        n2b_types[2*interxn_2b+1]};

            if ((ij_pair[0] == pair[0]) && 
                    (ij_pair[1] == pair[1])){
              index_ij = interxn_2b;
              inv_ij = 1;
            }
            else if ((ij_pair[0] == pair[1]) && 
                    (ij_pair[1] == pair[0])){
              index_ij = interxn_2b;
              inv_ij = -1;
            }

            if ((ik_pair[0] == pair[0]) && 
                    (ik_pair[1] == pair[1])){
              index_ik = interxn_2b;
              inv_ik = 1;
            }
            else if ((ik_pair[0] == pair[1]) &&
                    (ik_pair[1] == pair[0])){
              index_ik = interxn_2b;
              inv_ik = -1;
            }
          }

          if ((index_ij==-1) || (index_ik==-1)){
            std::string error_mesg = "Indices for 3b interaction=(" + 
                std::to_string(n3b_types[3*interxn]) + "," +
                std::to_string(n3b_types[3*interxn+1]) + "," +
                std::to_string(n3b_types[3*interxn+2]) +
                ") not found in the Neighs list";
            throw std::domain_error(error_mesg);
          }

          //match the current atom type to the atom types in the 3-body pair
          //if no match found skip 3-body interaction
          bool Z1_in_3b_interxn = false;
          int Z1_index_in_3b_interxn = -1;
          if (Z1==n3b_types[3*interxn]){
            Z1_in_3b_interxn = true;
            Z1_index_in_3b_interxn = 0;
          }
          else if (Z1==n3b_types[3*interxn+1]){
            Z1_in_3b_interxn = true;
            Z1_index_in_3b_interxn = 1;
          }
          else if (Z1==n3b_types[3*interxn+2]){
            Z1_in_3b_interxn = true;
            Z1_index_in_3b_interxn = 2;
          }

          //For traingle permutation- A-B-C, B-A-C and C-B-A
          bool swappable_ij = false;
          if (ij_pair[0] == ij_pair[1])
              swappable_ij = true;

          bool swappable_ik = false;
          if (ik_pair[0] == ik_pair[1])
              swappable_ik = true;

          //make local atomic_3b_Reprn_matrix_energy, atomic_3b_Reprn_matrix_fx,
          //atomic_3b_Reprn_matrix_fy, atomic_3b_Reprn_matrix_fz
          
          const int num_neighs_ij = Tot_num_Neighs[d*rows+index_ij];
          const int num_neighs_ik = Tot_num_Neighs[d*rows+index_ik];
          
          if ((num_neighs_ij>0) && (num_neighs_ik>0) && Z1_in_3b_interxn) {

          int ij_num_knots = n3b_num_knots_array[interxn][0];
          int ik_num_knots = n3b_num_knots_array[interxn][1];
          int jk_num_knots = n3b_num_knots_array[interxn][2];

          const int bl = ij_num_knots-4;
          const int bm = ik_num_knots-4;
          const int bn = jk_num_knots-4;


          //make sub arrays of knots and constants
          const std::vector<double> &knots_ij = n3b_knots_array[interxn][0];
          const std::vector<double> &knots_ik = n3b_knots_array[interxn][1];
          const std::vector<double> &knots_jk = n3b_knots_array[interxn][2];

          const std::vector<std::vector<double>> &constants_ij = constants_3b[interxn][0];
          const std::vector<std::vector<double>> &constants_ik = constants_3b[interxn][1];
          const std::vector<std::vector<double>> &constants_jk = constants_3b[interxn][2];

          const std::vector<std::vector<double>> &constants_ij_deri = constants_3b_deri[interxn][0];
          const std::vector<std::vector<double>> &constants_ik_deri = constants_3b_deri[interxn][1];
          const std::vector<std::vector<double>> &constants_jk_deri = constants_3b_deri[interxn][2];

          std::vector<double> atomic_3b_Reprn_matrix_energy ((bl*bm*bn), 0);
          std::vector<double> atomic_3b_Reprn_matrix_fx ((bl*bm*bn), 0);
          std::vector<double> atomic_3b_Reprn_matrix_fy ((bl*bm*bn), 0);
          std::vector<double> atomic_3b_Reprn_matrix_fz ((bl*bm*bn), 0);

          //make atomic_3b_Reprn_matrix_flatten_energy,
          std::vector<double> atomic_3b_Reprn_matrix_flatten_energy 
              (n3b_feature_sizes_un(interxn), 0);

          //fx, fy, fz
          std::vector<double> atomic_3b_Reprn_matrix_flatten_fx
              (n3b_feature_sizes_un(interxn), 0);
          std::vector<double> atomic_3b_Reprn_matrix_flatten_fy
              (n3b_feature_sizes_un(interxn), 0);
          std::vector<double> atomic_3b_Reprn_matrix_flatten_fz
              (n3b_feature_sizes_un(interxn), 0);
          
          //loop over neighbors in Neigh[atom1][index_ij]
          //loop over neighbors in Neigh[atom1][index_ik]
          int atom2_upper_limit = num_neighs_ij;
          int atom3_lower_limit = 0;

          if (index_ij == index_ik){
            atom2_upper_limit = num_neighs_ij-1;
          }
          for (int atom2=0; atom2<atom2_upper_limit; atom2++){
            if (index_ij == index_ik)
              atom3_lower_limit = atom2+1;
            for (int atom3=atom3_lower_limit; atom3<num_neighs_ik; atom3++){
              
              const double r_ij = Neighs[(d*rows*cols)+(index_ij*cols)+atom2];
              const double r_ik = Neighs[(d*rows*cols)+(index_ik*cols)+atom3];
              
              const int temp_index2 = (d*rows*cols*3)+(index_ij*cols*3)+(atom2*3);
              const double delx_ij = Neighs_del[temp_index2]; //-->delx_ij/r_ij
              const double dely_ij = Neighs_del[temp_index2+1];
              const double delz_ij = Neighs_del[temp_index2+2];

              const int temp_index3 = (d*rows*cols*3)+(index_ik*cols*3)+(atom3*3);
              const double delx_ik = Neighs_del[temp_index3]; //-->delx_ik/r_ik
              const double dely_ik = Neighs_del[temp_index3+1];
              const double delz_ik = Neighs_del[temp_index3+2];
                
              //distance jk? get from del_ij, rij, rik and del_ik --> r_ij, r_ik, r_jk
              const double delx_jk_p = (delx_ik*r_ik)-(delx_ij*r_ij);//(delx_ij*r_ij)-(delx_ik*r_ik);
              const double dely_jk_p = (dely_ik*r_ik)-(dely_ij*r_ij);//(dely_ij*r_ij)-(dely_ik*r_ik);
              const double delz_jk_p = (delz_ik*r_ik)-(delz_ij*r_ij);//(delz_ij*r_ij)-(delz_ik*r_ik);

              const double r_jk = sqrt((delx_jk_p*delx_jk_p) + 
                                      (dely_jk_p*dely_jk_p) + 
                                      (delz_jk_p*delz_jk_p));

              const double delx_jk = delx_jk_p/r_jk;
              const double dely_jk = dely_jk_p/r_jk;
              const double delz_jk = delz_jk_p/r_jk;

              //py::print(atom1,atom2,atom3,r_ij,r_ik,r_jk, delx_ij, delx_ik, delx_jk);

              //triangle permutation i-j-k ==> A-B-C
              //py::print("ijk",atom1,r_ij,r_ik,r_jk);
              if ((rmin_max_3b[6*interxn] <= r_ij) && 
                  (rmin_max_3b[6*interxn+1] > r_ij) && 
                  (rmin_max_3b[6*interxn+2] <= r_ik) && 
                  (rmin_max_3b[6*interxn+3] > r_ik) &&
                  (rmin_max_3b[6*interxn+4] <= r_jk) &&
                  (rmin_max_3b[6*interxn+5] > r_jk)) {

                //rsq_ij, rth_ij
                const double rsq_ij = r_ij*r_ij;
                const double rth_ij = rsq_ij*r_ij;
                  
                //rsq_ik, rth_ik
                const double rsq_ik = r_ik*r_ik;
                const double rth_ik = rsq_ik*r_ik;
                    
                //rsq_jk, rth_jk
                const double rsq_jk = r_jk*r_jk;
                const double rth_jk = rsq_jk*r_jk;

                //knot_posn_ij, knot_posn_ik, knot_posn_jk
                int knot_posn_ij = bl;
                int knot_posn_ik = bm;
                int knot_posn_jk = bn;

                while (r_ij<=knots_ij[knot_posn_ij])
                  knot_posn_ij--;

                while (r_ik<=knots_ik[knot_posn_ik])
                  knot_posn_ik--;

                while (r_jk<=knots_jk[knot_posn_jk])
                  knot_posn_jk--;
 
                //compute_3b_energy_feature(r_ij, r_ik, r_jk,
                //                          rsq_ij, rsq_ik, rsq_jk,
                //                          rth_ij, rth_ik, rth_jk,
                //                          knot_posn_ij, knot_posn_ik, knot_posn_jk,
                //                          bl, bm, bn,
                //                          knots_ij, knots_ik, knots_jk,
                //                          constants_ij, constants_ik, constants_jk,
                //                          atomic_3b_Reprn_matrix_energy);

                std::array<double,4> basis_ij = get_basis_set(r_ij, rsq_ij, rth_ij,
                                                              constants_ij, knot_posn_ij);

                std::array<double,4> basis_ik = get_basis_set(r_ik, rsq_ik, rth_ik,
                                                              constants_ik, knot_posn_ik);

                std::array<double,4> basis_jk = get_basis_set(r_jk, rsq_jk, rth_jk,
                                                              constants_jk, knot_posn_jk);

                //py::print("basis_ij",basis_ij[0],basis_ij[1],basis_ij[2],basis_ij[3]);
                //py::print("basis_ik",basis_ik[0],basis_ik[1],basis_ik[2],basis_ik[3]);
                //py::print("basis_jk",basis_jk[0],basis_jk[1],basis_jk[2],basis_jk[3]);

                 
                //basis_posn same as knot_posn
                for (int x=0; x<4; x++) {
                  for (int y=0; y<4; y++) {
                    for (int z=0; z<4; z++) {
                      const int temp_index = ((knot_posn_ij-x)*bm*bn) + 
                                                ((knot_posn_ik-y)*bn) + 
                                                (knot_posn_jk-z);
                      atomic_3b_Reprn_matrix_energy[temp_index] += basis_ij[x]*basis_ik[y]*basis_jk[z];
                    }
                  }
                }

                std::array<double,3> basis_ij_deri = 
                    get_basis_deri_set(r_ij, rsq_ij, constants_ij_deri, knot_posn_ij);
                
                double fpair_ij[4];
                fpair_ij[0] = basis_ij_deri[0];
                fpair_ij[1] = (basis_ij_deri[1]-basis_ij_deri[0]);
                fpair_ij[2] = (basis_ij_deri[2]-basis_ij_deri[1]);
                fpair_ij[3] = -1*basis_ij_deri[2];
                
                std::array<double,3> basis_ik_deri = 
                    get_basis_deri_set(r_ik, rsq_ik, constants_ik_deri, knot_posn_ik);

                double fpair_ik[4];
                fpair_ik[0] = basis_ik_deri[0];
                fpair_ik[1] = (basis_ik_deri[1]-basis_ik_deri[0]);
                fpair_ik[2] = (basis_ik_deri[2]-basis_ik_deri[1]);
                fpair_ik[3] = -1*basis_ik_deri[2];
                
                std::array<double,3> basis_jk_deri = 
                    get_basis_deri_set(r_jk, rsq_jk, constants_jk_deri, knot_posn_jk);

                double fpair_jk[4];
                fpair_jk[0] = basis_jk_deri[0];
                fpair_jk[1] = (basis_jk_deri[1]-basis_jk_deri[0]);
                fpair_jk[2] = (basis_jk_deri[2]-basis_jk_deri[1]);
                fpair_jk[3] = -1*basis_jk_deri[2];
                
                //py::print("fpair_ij",fpair_ij[0],fpair_ij[1],fpair_ij[2],fpair_ij[3]);
                //py::print("fpair_ik",fpair_ik[0],fpair_ik[1],fpair_ik[2],fpair_ik[3]);
                //py::print("fpair_jk",fpair_jk[0],fpair_jk[1],fpair_jk[2],fpair_jk[3]);
                for (int x=0; x<4; x++) {
                  for (int y=0; y<4; y++) {
                    for (int z=0; z<4; z++) {
                      const int temp_index = ((knot_posn_ij-x)*bm*bn) + 
                                                ((knot_posn_ik-y)*bn) + 
                                                (knot_posn_jk-z);
                      atomic_3b_Reprn_matrix_fx[temp_index] += fpair_ij[x]*basis_ik[y]*basis_jk[z]*delx_ij + 
                                                               basis_ij[x]*fpair_ik[y]*basis_jk[z]*delx_ik;
                      //if ((knot_posn_ij-x == 3) && (knot_posn_ik-y == 4) && (knot_posn_jk-z == 7))
                      //  py::print(r_ij,r_ik,r_jk,basis_ij[x],basis_ik[y],basis_jk[z]);
                     //if (atom1==batch_start+2){
                        //py::print("i-j-k",(knot_posn_ij-x),(knot_posn_ik-y),(knot_posn_jk-z),fpair_ij[x]*basis_ik[y]*basis_jk[z]*delx_ij,basis_ij[x]*fpair_ik[y]*basis_jk[z]*delx_ik);
                        //tempftest[temp_index] += fpair_ij[x]*basis_ik[y]*basis_jk[z]*delx_ij +
                        //                         basis_ij[x]*fpair_ik[y]*basis_jk[z]*delx_ik;

                        //const int temp_index2 = 1*bl*bm*bn + ((knot_posn_ij-x)*bm*bn) + ((knot_posn_ik-y)*bn) + (knot_posn_jk-z);

                        //tempftest[temp_index2] += fpair_ij[x]*basis_ik[y]*basis_jk[z]*(-1*delx_ij) + 
                                                  basis_ij[x]*basis_ik[y]*fpair_jk[z]*(delx_jk);

                        //const int temp_index3 = 2*bl*bm*bn + ((knot_posn_ij-x)*bm*bn) + ((knot_posn_ik-y)*bn) + (knot_posn_jk-z);
                        //tempftest[temp_index3] += basis_ij[x]*fpair_ik[y]*basis_jk[z]*(-1*delx_ik)+
                        //                          basis_ij[x]*basis_ik[y]*fpair_jk[z]*(-1*delx_jk);
                      //}

                      atomic_3b_Reprn_matrix_fy[temp_index] += fpair_ij[x]*basis_ik[y]*basis_jk[z]*dely_ij +
                                                               basis_ij[x]*fpair_ik[y]*basis_jk[z]*dely_ik;

                      atomic_3b_Reprn_matrix_fz[temp_index] += fpair_ij[x]*basis_ik[y]*basis_jk[z]*delz_ij +
                                                               basis_ij[x]*fpair_ik[y]*basis_jk[z]*delz_ik;
                    }
                  }
                }
              }  
              //triangle permuatation B-A-C --> j-i-k --> 2-1-3
              if (swappable_ij){
                  const double r_21 = r_ij;
                  const double r_23 = r_jk;
                  const double r_13 = r_ik;

                  //py::print("jik",atom1,r_21,r_23,r_13);

                  if ((rmin_max_3b[6*interxn] <= r_21) &&
                      (rmin_max_3b[6*interxn+1] > r_21) &&
                      (rmin_max_3b[6*interxn+2] <= r_23) &&
                      (rmin_max_3b[6*interxn+3] > r_23) &&
                      (rmin_max_3b[6*interxn+4] <= r_13) &&
                      (rmin_max_3b[6*interxn+5] > r_13)){

                  const double rsq_21 = r_21*r_21;
                  const double rth_21 = rsq_21*r_21;

                  const double rsq_23 = r_23*r_23;
                  const double rth_23 = rsq_23*r_23;

                  const double rsq_13 = r_13*r_13;
                  const double rth_13 = rsq_13*r_13;

                  int knot_posn_21 = bl;
                  int knot_posn_23 = bm;
                  int knot_posn_13 = bn;

                  while (r_21<=knots_ij[knot_posn_21])
                      knot_posn_21--;

                  while (r_23<=knots_ik[knot_posn_23])
                    knot_posn_23--;

                  while (r_13<=knots_jk[knot_posn_13])
                    knot_posn_13--;


                  const double delx_21 = -1*delx_ij;
                  const double dely_21 = -1*dely_ij;
                  const double delz_21 = -1*delz_ij;

                  //const double delx_23 = delx_jk;
                  //const double dely_23 = dely_jk;
                  //const double delz_23 = delz_jk;

                  const double delx_13 = delx_ik;
                  const double dely_13 = dely_ik;
                  const double delz_13 = delz_ik;

                  //-------------------------------new ij
                  const std::array<double, 4> basis_21 = get_basis_set(r_21, rsq_21, rth_21,
                                                                        constants_ij, knot_posn_21);
                  const std::array<double, 3> basis_21_deri = 
                                                get_basis_deri_set(r_21, rsq_21, constants_ij_deri, knot_posn_21);
                  double fpair_21[4];
                  fpair_21[0] = basis_21_deri[0];
                  fpair_21[1] = (basis_21_deri[1]-basis_21_deri[0]);
                  fpair_21[2] = (basis_21_deri[2]-basis_21_deri[1]);
                  fpair_21[3] = -1*basis_21_deri[2];

                  //-------------------------------new ik
                  const std::array<double, 4> basis_23 = get_basis_set(r_23, rsq_23, rth_23,
                                                                        constants_ik, knot_posn_23);
                  
                  //-------------------------------new jk
                  const std::array<double, 4> basis_13 = get_basis_set(r_13, rsq_13, rth_13, 
                                                                        constants_jk, knot_posn_13);
                  const std::array<double, 3> basis_13_deri = 
                                                get_basis_deri_set(r_13, rsq_13, constants_jk_deri, knot_posn_13);
                  double fpair_13[4];
                  fpair_13[0] = basis_13_deri[0];
                  fpair_13[1] = (basis_13_deri[1]-basis_13_deri[0]);
                  fpair_13[2] = (basis_13_deri[2]-basis_13_deri[1]);
                  fpair_13[3] = -1*basis_13_deri[2];

                  //py::print("swappable_ij");
                  //py::print("j-i-k",r_21, r_23, r_13, delx_21, delx_23, delx_13, knot_posn_21, knot_posn_23, knot_posn_13);
                  //py::print(basis_21[0],basis_21[1],basis_21[2],basis_21[3]);
                  //py::print(basis_23[0],basis_23[1],basis_23[2],basis_23[3]);
                  //py::print(basis_13[0],basis_13[1],basis_13[2],basis_13[3]);

                  //py::print(fpair_21[0],fpair_21[1],fpair_21[2],fpair_21[3]);
                  //py::print(fpair_13[0],fpair_13[1],fpair_13[2],fpair_13[3]);
                  for (int x=0; x<4; x++) {
                    for (int y=0; y<4; y++) {
                      for (int z=0; z<4; z++) {
                        const int temp_index = ((knot_posn_21-x)*bm*bn) +
                                                    ((knot_posn_23-y)*bn) +
                                                    (knot_posn_13-z);
                        //-1 because 2 is central and 1 is neighbor and we want forces on 1
                        atomic_3b_Reprn_matrix_fx[temp_index] += (-1*fpair_21[x]*basis_23[y]*basis_13[z]*delx_21) +
                                                                 basis_21[x]*basis_23[y]*fpair_13[z]*(delx_13);

                        //if ((knot_posn_21-x == 3) && (knot_posn_23-y == 4) && (knot_posn_13-z == 7))
                        //  py::print(r_21, r_23, r_13, basis_21[x],basis_23[y],basis_13[z]);
                          
                        atomic_3b_Reprn_matrix_fy[temp_index] += (-1*fpair_21[x]*basis_23[y]*basis_13[z]*dely_21) +
                                                                 basis_21[x]*basis_23[y]*fpair_13[z]*(dely_13);
                          
                        atomic_3b_Reprn_matrix_fz[temp_index] += (-1*fpair_21[x]*basis_23[y]*basis_13[z]*delz_21) +
                                                                 basis_21[x]*basis_23[y]*fpair_13[z]*(delz_13);

                       //if (atom1==batch_start){
                       //   tempftest[temp_index] += (-1*fpair_21[x]*basis_23[y]*basis_13[z]*delx_21) +
                       //                            basis_21[x]*basis_23[y]*fpair_13[z]*(delx_13);
                       //}
                      }
                    }
                  }
                  }
              }
              //triangle permuatation C-B-A --> k-j-i --> 3-2-1
              if (swappable_ik){
                  const double r_32 = r_jk;
                  const double r_31 = r_ik;
                  const double r_21 = r_ij;
                  
                  //py::print("kji",atom1,r_32,r_31,r_21);

                  if ((rmin_max_3b[6*interxn] <= r_32) &&
                      (rmin_max_3b[6*interxn+1] > r_32) &&
                      (rmin_max_3b[6*interxn+2] <= r_31) &&
                      (rmin_max_3b[6*interxn+3] > r_31) &&
                      (rmin_max_3b[6*interxn+4] <= r_21) &&
                      (rmin_max_3b[6*interxn+5] > r_21)){

                  //-------------------------------new ij
                  const double rsq_32 = r_32*r_32;
                  const double rth_32 = rsq_32*r_32;

                  //-------------------------------new ik
                  const double rsq_31 = r_31*r_31;
                  const double rth_31 = rsq_31*r_31;

                  //-------------------------------new jk
                  const double rsq_21 = r_21*r_21;
                  const double rth_21 = rsq_21*r_21;

                  int knot_posn_32 = bl;
                  int knot_posn_31 = bm;
                  int knot_posn_21 = bn;
                  
                  while (r_32<=knots_ij[knot_posn_32])
                    knot_posn_32--;
                  
                  while (r_31<=knots_ik[knot_posn_31])
                    knot_posn_31--;
                  
                  while (r_21<=knots_jk[knot_posn_21])
                    knot_posn_21--;
                  
                  //const double delx_32 = -1*delx_jk;
                  //const double dely_32 = -1*dely_jk;
                  //const double delz_32 = -1*delz_jk;

                  const double delx_31 = -1*delx_ik;
                  const double dely_31 = -1*dely_ik;
                  const double delz_31 = -1*delz_ik;

                  const double delx_21 = -1*delx_ij;
                  const double dely_21 = -1*dely_ij;
                  const double delz_21 = -1*delz_ij;


                  //-------------------------------new ij
                  const std::array<double, 4> basis_32 = get_basis_set(r_32, rsq_32, rth_32,
                                                                        constants_ij, knot_posn_32);
                  const std::array<double, 3> basis_32_deri = 
                                                get_basis_deri_set(r_32, rsq_32, constants_ij_deri, knot_posn_32);
                  double fpair_32[4];
                  fpair_32[0] = basis_32_deri[0];
                  fpair_32[1] = (basis_32_deri[1]-basis_32_deri[0]);
                  fpair_32[2] = (basis_32_deri[2]-basis_32_deri[1]);
                  fpair_32[3] = -1*basis_32_deri[2];

                  //-------------------------------new ik
                  const std::array<double, 4> basis_31 = get_basis_set(r_31, rsq_31, rth_31,
                                                                        constants_ik, knot_posn_31);
                  const std::array<double, 3> basis_31_deri = 
                                                get_basis_deri_set(r_31, rsq_31, constants_ik_deri, knot_posn_31);
                  double fpair_31[4];
                  fpair_31[0] = basis_31_deri[0];
                  fpair_31[1] = (basis_31_deri[1]-basis_31_deri[0]);
                  fpair_31[2] = (basis_31_deri[2]-basis_31_deri[1]);
                  fpair_31[3] = -1*basis_31_deri[2];

                  //-------------------------------new jk
                  const std::array<double, 4> basis_21 = get_basis_set(r_21, rsq_21, rth_21,
                                                                        constants_jk, knot_posn_21);
                  const std::array<double, 3> basis_21_deri = 
                                                get_basis_deri_set(r_21, rsq_21, constants_jk_deri, knot_posn_21);
                  double fpair_21[4];
                  fpair_21[0] = basis_21_deri[0];
                  fpair_21[1] = (basis_21_deri[1]-basis_21_deri[0]);
                  fpair_21[2] = (basis_21_deri[2]-basis_21_deri[1]);
                  fpair_21[3] = -1*basis_21_deri[2];

                  //py::print("swappable_ik");
                  //py::print("k-j-i",r_32, r_31, r_21, delx_32, delx_31, delx_21, knot_posn_32, knot_posn_31, knot_posn_21);
                  //py::print(basis_32[0],basis_32[1],basis_32[2],basis_32[3]);
                  //py::print(basis_31[0],basis_31[1],basis_31[2],basis_31[3]);
                  //py::print(basis_21[0],basis_21[1],basis_21[2],basis_21[3]);

                  //py::print(fpair_32[0],fpair_32[1],fpair_32[2],fpair_32[3]);
                  //py::print(fpair_31[0],fpair_31[1],fpair_31[2],fpair_31[3]);
                  //py::print(fpair_21[0],fpair_21[1],fpair_21[2],fpair_21[3]);
                  
                  for (int x=0; x<4; x++) {
                    for (int y=0; y<4; y++) {
                      for (int z=0; z<4; z++) {
                        const int temp_index = ((knot_posn_32-x)*bm*bn) +
                                                ((knot_posn_31-y)*bn) +
                                                (knot_posn_21-z);

                        atomic_3b_Reprn_matrix_fx[temp_index] += (-1*basis_32[x]*fpair_31[y]*basis_21[z]*delx_31)+
                                                                 (-1*basis_32[x]*basis_31[y]*fpair_21[z]*delx_21);

                        //if ((knot_posn_32-x == 3) && (knot_posn_31-y == 4) && (knot_posn_21-z == 7))
                        //  py::print(r_32, r_31, r_21, basis_32[x],basis_31[y],basis_21[z]);

                        atomic_3b_Reprn_matrix_fy[temp_index] += (-1*basis_32[x]*fpair_31[y]*basis_21[z]*dely_31)+
                                                                 (-1*basis_32[x]*basis_31[y]*fpair_21[z]*dely_21);

                        atomic_3b_Reprn_matrix_fz[temp_index] += (-1*basis_32[x]*fpair_31[y]*basis_21[z]*delz_31)+
                                                                 (-1*basis_32[x]*basis_31[y]*fpair_21[z]*delz_21);
                       //if (atom1==batch_start){
                       //   tempftest[temp_index] += (-1*basis_32[x]*fpair_31[y]*basis_21[z]*delx_31)+
                       //                            (-1*basis_32[x]*basis_31[y]*fpair_21[z]*delx_21);
                       //}
                      }
                    }
                  }
                  }
              } //swappable_ik
              //} //r_ij, r_ik min max if
            } //close loop atom3
          } //close loop atom2
          
          //symmetrize the matrix ie compress
          n3b_compress(atomic_3b_Reprn_matrix_energy,
                       atomic_3b_Reprn_matrix_flatten_energy,
                       n3b_symmetry, template_mask_start, template_mask_end, bl, bm, bn);

          int d4 = d*4;
          for (int i=basis_start_posn; i < basis_start_posn+n3b_feature_sizes_un(interxn); i++) {
            atomic_Reprn[d4*reprn_length+i] += atomic_3b_Reprn_matrix_flatten_energy[i-basis_start_posn];
          }

          n3b_compress(atomic_3b_Reprn_matrix_fx,
                       atomic_3b_Reprn_matrix_flatten_fx,
                       n3b_symmetry, template_mask_start, template_mask_end, bl, bm, bn);
          d4 = d4+1;
          for (int i=basis_start_posn; i < basis_start_posn+n3b_feature_sizes_un(interxn); i++) {
            atomic_Reprn[d4*reprn_length+i] += atomic_3b_Reprn_matrix_flatten_fx[i-basis_start_posn];
          }

          n3b_compress(atomic_3b_Reprn_matrix_fy,
                       atomic_3b_Reprn_matrix_flatten_fy,
                       n3b_symmetry, template_mask_start, template_mask_end, bl, bm, bn);
          d4 = d4+1;
          for (int i=basis_start_posn; i < basis_start_posn+n3b_feature_sizes_un(interxn); i++) {
            atomic_Reprn[d4*reprn_length+i] += atomic_3b_Reprn_matrix_flatten_fy[i-basis_start_posn];
          }
          
          n3b_compress(atomic_3b_Reprn_matrix_fz,
                       atomic_3b_Reprn_matrix_flatten_fz,
                       n3b_symmetry, template_mask_start, template_mask_end, bl, bm, bn);
          d4 = d4+1;
          for (int i=basis_start_posn; i < basis_start_posn+n3b_feature_sizes_un(interxn); i++) {
            atomic_Reprn[d4*reprn_length+i] += atomic_3b_Reprn_matrix_flatten_fz[i-basis_start_posn];
          }
          
          } // if num_neighs_ij, num_neighs_ik

        basis_start_posn += n3b_feature_sizes_un(interxn);
        index_for_symm_weights += n3b_feature_sizes_un(interxn);
        }//loop over interxn
      }//close loop over central atoms in this batch
    }
    
    //Add all atomic representation of atoms that part of the same crystal
    //
    //total crystals in this batch
    int crystal_start = crystal_index_un(batch_start);
    ////batch_end-1 is the last atom of this batch
    int crystal_end = crystal_index_un(batch_end-1);
    tot_crystals = crystal_end - crystal_start + 1; // +1 as indexing start from 0
    //find total atoms in this batch
    tot_atoms = batch_end - batch_start;
    
    //incomplete crystal representation from the previous batch
    //Add to the very first crystal
   
    if (crystal_Reprn.empty()) {
      crystal_Reprn = std::vector<double> ((tot_crystals+(tot_atoms*3))*reprn_length);
    }
    //Clear and shrink any existing crystal_Reprn
    else {
      crystal_Reprn.clear();
      crystal_Reprn.shrink_to_fit();

      if (incomplete){
        crystal_Reprn.resize((tot_crystals+(tot_atoms*3))*reprn_length);
        std::fill(crystal_Reprn.begin(), crystal_Reprn.end(), 0);

        int prev_data_len = incomplete_crystal_Reprn.size()/reprn_length;
        crystal_Reprn.resize((prev_data_len+(tot_crystals-1)+(tot_atoms*3))*reprn_length);
        for (int prev=0; prev<prev_data_len; prev++){
          for (int i=0; i<reprn_length; i++)
            crystal_Reprn[prev*reprn_length+i] = incomplete_crystal_Reprn[prev*reprn_length+i];
        }

      }
      else{
        crystal_Reprn.resize((tot_crystals+(tot_atoms*3))*reprn_length);
        std::fill(crystal_Reprn.begin(), crystal_Reprn.end(), 0);
      }
    }
    

    if (incomplete){
      int prev_data_len = incomplete_crystal_Reprn.size()/reprn_length;
      atom_count = (prev_data_len-1)/3;
    }
    else{
      atom_count = 0;
      prev_CI = crystal_index_un(batch_start);
    }
    
    int IfCR = 0; //index for crystal representation
    for (int atom1=batch_start; atom1<batch_end; atom1++) {
      int CI1 = crystal_index_un(atom1); //crystal index of current atom
      int d = 4*(atom1-batch_start); //d=0 is the first atom in this batch
                                     //d=3 is the second atom in this batch
      int atoms_in_ccrys = geom_posn_un(CI1+1)-geom_posn_un(CI1); //atom in curent crystal

      if (CI1 != prev_CI){
        //natoms_in_Pcrys --> num atom in previous crystal
        int natoms_in_Pcrys = geom_posn_un(prev_CI+1) - geom_posn_un(prev_CI);
        IfCR = IfCR + (natoms_in_Pcrys*3) + 1;
        atom_count = 0;
        prev_CI = CI1;
      }
      //index for crystal_Repren --> dC
      //int dC = crystal_index_un(atom1) - crystal_start; //dC=0 is the first crystal
                                                        //in this batch
      for (int i=0; i<reprn_length; i++){
        //eng feature
        crystal_Reprn[IfCR*reprn_length+i] += atomic_Reprn[d*reprn_length+i];
        //fx
        crystal_Reprn[(IfCR+(atom_count*3)+1)*reprn_length+i] = atomic_Reprn[(d+1)*reprn_length+i];
        //fy
        crystal_Reprn[(IfCR+(atom_count*3)+2)*reprn_length+i] = atomic_Reprn[(d+2)*reprn_length+i];
        //fz
        crystal_Reprn[(IfCR+(atom_count*3)+3)*reprn_length+i] = atomic_Reprn[(d+3)*reprn_length+i];
      }
      //Write energy value to energy feature
      crystal_Reprn[IfCR*reprn_length+0] = energy_array_un(CI1);
      atom_count++;
    }
    
    //crystal_Reprn will have atmost 1 incomplete crystal representation
    //If it has incomplete crystal representation it will be the last crystal
    //IfCR is the position of the last crystal
    
    //Determine if the last crystal representation is incomplete
    ////batch_end-1 is the last atom of this batch
    int last_crystal_CI = crystal_index_un(batch_end-1);
    int start_posn = geom_posn_un(last_crystal_CI);
    int end_posn = geom_posn_un(last_crystal_CI+1);
    int num_atoms = end_posn-start_posn; //-->total num of atoms in the last
                                         //crystal of this batch

    int num_atoms_from_CR = 0; //Num of atoms deduced from the last crystal
                               //representation
    for (int i=0; i<nelements; i++) {
      num_atoms_from_CR += static_cast<int>(
                            std::ceil(
                                crystal_Reprn[IfCR*reprn_length+1+i]));
    }
    
    //Create incomplete_crystal_Reprn
    if (num_atoms_from_CR!=num_atoms){ //last crystal representation is incomplete
      //py::print("Incomplete detected!!!!!");
      incomplete = true;
      tot_complete_crystals = tot_crystals-1;
      //Create incomplete_crystal_Reprn
      if (incomplete_crystal_Reprn.empty())
        incomplete_crystal_Reprn = std::vector<double> ((1+(num_atoms_from_CR*3))*reprn_length,0);
      else{
        incomplete_crystal_Reprn.clear();
        incomplete_crystal_Reprn.shrink_to_fit();
        incomplete_crystal_Reprn.resize((1+(num_atoms_from_CR*3))*reprn_length);
        std::fill(incomplete_crystal_Reprn.begin(), 
                incomplete_crystal_Reprn.end(), 0);
      }
      //store energy to be added to next batch
      for (int i=0; i<reprn_length; i++)
        incomplete_crystal_Reprn[i] = 
            crystal_Reprn[IfCR*reprn_length+i];
      //store forces
      for (int atom1=0; atom1<num_atoms_from_CR; atom1++){
        for (int i=0; i<reprn_length; i++){
          incomplete_crystal_Reprn[((atom1*3)+1)*reprn_length+i] = 
              crystal_Reprn[(IfCR+(atom1*3)+1)*reprn_length+i];

          incomplete_crystal_Reprn[((atom1*3)+2)*reprn_length+i] = 
              crystal_Reprn[(IfCR+(atom1*3)+2)*reprn_length+i];

          incomplete_crystal_Reprn[((atom1*3)+3)*reprn_length+i] = 
              crystal_Reprn[(IfCR+(atom1*3)+3)*reprn_length+i];
        }
      }
    }
    else{
      incomplete = false;
      tot_complete_crystals = tot_crystals;
      incomplete_crystal_Reprn.clear();
      incomplete_crystal_Reprn.shrink_to_fit();
      incomplete_crystal_Reprn.resize(0);

    }
    //Write all complete crystal representations to hdf5 file
    if (tot_crystals==1) {//Just 1 crystal in this batch
      if (num_atoms_from_CR==num_atoms) //crystal representation complete
        //write hdf5
        write_hdf5(1+(num_atoms_from_CR*3), reprn_length, batch_numb,
                   feature_file, crystal_Reprn, crystal_start,
                   1, column_names, geom_posn);
    }
    else{
        //write hdf5 from complete crystal representation in this batch
        hsize_t h5_num_rows = tot_crystals;
        if (num_atoms_from_CR!=num_atoms)
          h5_num_rows = IfCR;
        else
          h5_num_rows = IfCR + 1 + (num_atoms_from_CR*3);
        write_hdf5(h5_num_rows, reprn_length,
                   batch_numb, feature_file,
                   crystal_Reprn, crystal_start, 
                   tot_complete_crystals, column_names, geom_posn);
    }
  } //Loop over batch
  

  feature_file.close();

  if (return_Neigh){
    py::buffer_info Neigh_buff_info(
      Neighs.data(),      /* Pointer to buffer */
      sizeof(double),     /* Size of one scalar */
      py::format_descriptor<double>::format(),    /* Python struct-style format descriptor */
      3,                  /* Number of dimensions */
      { depth, rows, cols},  /* Buffer dimensions */
      { sizeof(double) * rows * cols,        /* Strides (in bytes) for each index */
      sizeof(double) * cols, 
      sizeof(double) }
    );
    return py::array(Neigh_buff_info);
  }
  
  /*else{
    py::buffer_info atomic_Reprn_buff_info(
      atomic_Reprn.data(),
      sizeof(double),
      py::format_descriptor<double>::format(),
      2,
      { batch_size*4, reprn_length},
      { sizeof(double)*reprn_length,
      sizeof(double) }
    );
    return py::array(atomic_Reprn_buff_info);
  }

  else{
    py::buffer_info atomic_Reprn_buff_info(
      Neighs_del.data(),
      sizeof(double),
      py::format_descriptor<double>::format(),
      3,
      { depth, rows, cols*3},
      { sizeof(double)*rows * cols*3,
      sizeof(double) * cols*3,
      sizeof(double)}
    );
    return py::array(atomic_Reprn_buff_info);
  }*/
  else{
    if (!incomplete){
    py::buffer_info atomic_Reprn_buff_info(
      crystal_Reprn.data(),
      sizeof(double),
      py::format_descriptor<double>::format(),
      2,
      { (tot_crystals+(tot_atoms*3)), reprn_length },
      { sizeof(double)*reprn_length,
      sizeof(double) }
    );
    return py::array(atomic_Reprn_buff_info);
    }
    else{
    py::buffer_info atomic_Reprn_buff_info(
      crystal_Reprn.data(),
      sizeof(double),
      py::format_descriptor<double>::format(),
      2,
      { (prev_data_len+tot_crystals-1+(tot_atoms*3)),reprn_length},
      { sizeof(double)*reprn_length,
      sizeof(double) }
    );
    return py::array(atomic_Reprn_buff_info);
    }
  }
}

py::array UltraFastFeaturize::get_elements()
{
  py::buffer_info elements_buff_info(
    elements.data(),
    sizeof(int),
    py::format_descriptor<int>::format(),
    1,
    {nelements},
    {sizeof(int)}
  );
  return py::array(elements_buff_info);
}

std::string UltraFastFeaturize::get_filename()
{
  return filename;
}

void UltraFastFeaturize::write_hdf5(const hsize_t num_rows, const hsize_t num_cols,
                               const int batch_num, const H5::H5File &file_fp,
                               const std::vector<double> &Data,
                               int first_crystal_CI, int tot_complete_crystals,
                               std::vector<std::string> &column_names,
                               py::array_t<int, py::array::c_style> &geom_posn)
{
  auto geom_posn_un = geom_posn.unchecked<1>();
  // Create a unique group name for each batch
  std::string group_name = "features_" + std::to_string(write_hdf5_counter);
  H5::Group group = file_fp.createGroup(group_name);

  //Write column names at axis0
  hsize_t dims_column_names[1] = {column_names.size()};

  H5::DataSpace dataspace_column_names(1, dims_column_names);
  
  // Convert the strings to C-style strings (char*) for HDF5.
  size_t max_length = 0;
  std::vector<const char*> cStrList_columns;
  for (int i=0; i<column_names.size(); i++) {
      const auto& str = column_names[i];
      cStrList_columns.push_back(str.c_str());
      max_length = std::max(max_length, str.size()+1);
  }

  H5::StrType datatype_column_names(H5::PredType::C_S1, H5T_VARIABLE);
  H5Tset_cset(datatype_column_names.getId(), H5T_CSET_UTF8);

  H5::DataSet dataset_column_names = group.createDataSet("axis0", datatype_column_names,
                                                        dataspace_column_names);
        
  dataset_column_names.write(&cStrList_columns[0], datatype_column_names);

  dataset_column_names.close(); 

  //make CI_to_write and desc_count arrays
  int local_CI_count = 1;
  int *CI_to_write = new int [num_rows];
  int *desc_count = new int [num_rows];
  int counter = 0;

  for (int i=first_crystal_CI; i<first_crystal_CI+tot_complete_crystals; i++) {
    int start_posn = geom_posn_un(i);
    int end_posn = geom_posn_un(i+1);
    int num_atoms = end_posn-start_posn;

    for (int j=0; j<(num_atoms*3)+1; j++){
      CI_to_write[counter] = local_CI_count;
      desc_count[counter] = j; 
      counter++;
    }

    local_CI_count++;
  }

  //Write crystal index 1111,222222 to axis1_label0
  hsize_t dims_CI[1] = {num_rows};

  H5::DataSpace dataspace_CI(1, dims_CI);

  H5::DataSet dataset_CI = group.createDataSet("axis1_label0", H5::PredType::NATIVE_INT,
                                                        dataspace_CI);
  dataset_CI.write(CI_to_write, H5::PredType::NATIVE_INT);
  dataset_CI.close();

  //Write row counts 01234,012345 to axis1_label1
  hsize_t dims_row_count[1] = {num_rows};

  H5::DataSpace dataspace_row_count(1, dims_row_count);

  H5::DataSet dataset_row_count = group.createDataSet("axis1_label1", H5::PredType::NATIVE_INT,
                                                        dataspace_row_count);
  dataset_row_count.write(desc_count, H5::PredType::NATIVE_INT);
  dataset_row_count.close();
  
  //Write structure names to axis1_level0
  hsize_t dims_struct_names[1] = {static_cast<hsize_t>(tot_complete_crystals)};

  H5::DataSpace dataspace_struct_names(1, dims_struct_names);
  
  // Convert the strings to C-style strings (char*) for HDF5.
  max_length = 0;
  std::vector<const char*> cStrList;
  for (int i=first_crystal_CI; i<first_crystal_CI+tot_complete_crystals; i++) {
      const auto& str = structure_names[i];
      cStrList.push_back(str.c_str());
      max_length = std::max(max_length, str.size()+1);
  }

  H5::StrType datatype_struct_names(H5::PredType::C_S1, H5T_VARIABLE);
  H5Tset_cset(datatype_struct_names.getId(), H5T_CSET_UTF8);

  H5::DataSet dataset_struct_names = group.createDataSet("axis1_level0", datatype_struct_names,
                                                        dataspace_struct_names);
        
  dataset_struct_names.write(&cStrList[0], datatype_struct_names);

  dataset_struct_names.close(); 

  //Write 'energy', fx_0, fx_1, ...; only for max size to axis1_level1
  ////Find max crystal size in this batch
  int max_crystal_size = 0;
  for (int i=first_crystal_CI; i<first_crystal_CI+tot_complete_crystals; i++) {
    int start_posn = geom_posn_un(i);
    int end_posn = geom_posn_un(i+1);
    int num_atoms = end_posn-start_posn;
    max_crystal_size = std::max(max_crystal_size,num_atoms);
  }
  
  std::vector<std::string> descriptor_names;
  descriptor_names.push_back("energy");
  for (int i=0; i<max_crystal_size; i++){
    descriptor_names.push_back("fx_"+std::to_string(i));
    descriptor_names.push_back("fy_"+std::to_string(i));
    descriptor_names.push_back("fz_"+std::to_string(i));
  }

  hsize_t dims_descriptor_names[1] = {descriptor_names.size()};

  H5::DataSpace dataspace_descriptor_names(1, dims_descriptor_names);

  // Convert the strings to C-style strings (char*) for HDF5.
  max_length = 0;
  std::vector<const char*> cStrList_desc;
  for (int i=0; i<descriptor_names.size(); i++) {
      const auto& str = descriptor_names[i];
      cStrList_desc.push_back(str.c_str());
      max_length = std::max(max_length, str.size()+1);
  }
  H5::StrType datatype_descriptor_names(H5::PredType::C_S1, H5T_VARIABLE);
  H5Tset_cset(datatype_descriptor_names.getId(), H5T_CSET_UTF8);

  H5::DataSet dataset_descriptor_names = group.createDataSet("axis1_level1", datatype_descriptor_names,
                                                        dataspace_descriptor_names);
        
  dataset_descriptor_names.write(&cStrList_desc[0], datatype_descriptor_names);

  dataset_descriptor_names.close(); 
  
  //Write column names to block0_items
  H5::DataSet dataset_block0_items = group.createDataSet("block0_items", datatype_column_names,
                                                        dataspace_column_names);
        
  dataset_block0_items.write(&cStrList_columns[0], datatype_column_names);

  dataset_block0_items.close(); 
  
  //Write descriptors to block0_values
  hsize_t dims[2] = {num_rows, num_cols};

  H5::DataSpace dataspace(2, dims);

  H5::DataSet dataset = group.createDataSet("block0_values",
                                        H5::PredType::NATIVE_DOUBLE,
                                        dataspace);
  dataset.write(Data.data(), H5::PredType::NATIVE_DOUBLE);
  dataset.close();
  //
  //
  delete[] CI_to_write, desc_count;
  group.close();
  write_hdf5_counter++;
}

void UltraFastFeaturize::n3b_compress(std::vector<double> &atomic_3b_Reprn_matrix,
                                      std::vector<double> &atomic_3b_Reprn_flatten,
                                      int n3b_symm,
                                      int template_mask_start,
                                      int template_mask_end,
                                      int bl, int bm, int bn){
  
  std::vector<double> vec (atomic_3b_Reprn_matrix.size(), 0);
  std::vector<double> redundancy (template_mask_end-template_mask_start, 0);
  std::vector<double> vec_flat (template_mask_end-template_mask_start, 0);

  if (n3b_symm == 1) {
    for(int i=0; i<bl; i++) {
      for(int j=0; j<bm; j++) {
        for(int k=0; k<bn; k++) {
          int temp_index = i*bm*bn + j*bn + k;
          vec[temp_index] = atomic_3b_Reprn_matrix[temp_index];
        }
      }
    }
  }

  else if (n3b_symm == 2) {
    for(int i=0; i<bl; i++) {
      for(int j=0; j<bm; j++) {
        for(int k=0; k<bn; k++) {
          int temp_index = i*bm*bn + j*bn + k;
          int temp_index2 = j*bm*bn + i*bn + k;
          //py::print(i,j,k,atomic_3b_Reprn_matrix[temp_index],atomic_3b_Reprn_matrix[temp_index2]);
          vec[temp_index] = atomic_3b_Reprn_matrix[temp_index] + atomic_3b_Reprn_matrix[temp_index2];
        }
      }
    }
  }

  else if (n3b_symm == 3) {
    std::vector<double> temp2 (atomic_3b_Reprn_matrix.size(), 0);
    std::vector<double> temp3 (atomic_3b_Reprn_matrix.size(), 0);

    for(int i=0; i<bl; i++) {
      for(int j=0; j<bm; j++) {
        for(int k=0; k<bn; k++) {
          const int temp_index = i*bm*bn + j*bn + k;  //0,1,2
          const int temp_index2 = i*bm*bn + k*bn + j; //0,2,1
          const int temp_index3 = j*bm*bn + i*bn + k; //1,0,2
          const int temp_index4 = k*bm*bn + j*bn + i; //2,1,0

          vec[temp_index] = atomic_3b_Reprn_matrix[temp_index] + 
                            atomic_3b_Reprn_matrix[temp_index2] +
                            atomic_3b_Reprn_matrix[temp_index3] +
                            atomic_3b_Reprn_matrix[temp_index4];

          temp2[temp_index] = atomic_3b_Reprn_matrix[temp_index2]; //0,2,1
          temp3[temp_index] = atomic_3b_Reprn_matrix[temp_index3]; //1,0,2
        }
      }
    }

    for(int i=0; i<bl; i++) {
      for(int j=0; j<bm; j++) {
        for(int k=0; k<bn; k++) {
          const int temp_index = i*bm*bn + j*bn + k;
          const int temp_index5 = j*bm*bn + i*bn + k; //1,0,2
          const int temp_index6 = i*bm*bn + k*bn + j; //0,2,1
          vec[temp_index] += temp2[temp_index5] + temp3[temp_index6];
        }
      }
    }
  }

  for (int i=template_mask_start; i<template_mask_end; i++){
    atomic_3b_Reprn_flatten[i-template_mask_start] = vec[template_mask[i]]*flat_weights[i];
  }
}

/*For testing only*/
py::array UltraFastFeaturize::get_symmetry_weights(int interxn, int lead, int trail){
  auto n3b_symm_array_un = n3b_symm_array.unchecked<1>();
  auto n3b_num_knots_un = n3b_num_knots.unchecked<2>();
  int symm = n3b_symm_array_un(interxn);
  //template_array_flatten = std::vector<double> (4,0);
  template_array_flatten_test = this->BsplineConfig.get_symmetry_weights(symm,
                                                n3b_knots_array[interxn][0],
                                                n3b_knots_array[interxn][1],
                                                n3b_knots_array[interxn][2],
                                                n3b_num_knots_un(interxn,0),
                                                n3b_num_knots_un(interxn,1),
                                                n3b_num_knots_un(interxn,2),
                                                lead,
                                                trail);
  py::buffer_info template_array_flatten_buf(
    template_array_flatten_test.data(),
    sizeof(double),
    py::format_descriptor<double>::format(),
    1,
    {template_array_flatten_test.size()},
    {sizeof(double)}
  );

  return py::array(template_array_flatten_buf);
}

py::array UltraFastFeaturize::get_flat_weights(){
  py::buffer_info flat_weights_buf(
    flat_weights.data(),
    sizeof(double),
    py::format_descriptor<double>::format(),
    1,
    {flat_weights.size()},
    {sizeof(double)}
  );

  return py::array(flat_weights_buf);
}

py::array UltraFastFeaturize::get_template_mask(){
  py::buffer_info template_mask_buf(
    template_mask.data(),
    sizeof(int),
    py::format_descriptor<int>::format(),
    1,
    {template_mask.size()},
    {sizeof(int)}
  );

  return py::array(template_mask_buf);
}

void UltraFastFeaturize::compute_3b_energy_feature(const double r_ij, const double r_ik, const double r_jk,
                       const double rsq_ij, const double rsq_ik, const double rsq_jk,
                       const double rth_ij, const double rth_ik, const double rth_jk,
                       int knot_posn_ij, int knot_posn_ik, int knot_posn_jk,
                       const int bl, const int bm, const int bn,
                       const std::vector<double>& knots_ij,
                       const std::vector<double>& knots_ik,
                       const std::vector<double>& knots_jk,
                       const std::vector<std::vector<double>>& constants_ij,
                       const std::vector<std::vector<double>>& constants_ik,
                       const std::vector<std::vector<double>>& constants_jk,
                       std::vector<double>& atomic_3b_Reprn_matrix_energy)
{
                //basis_posn same as knot_posn
                double basis_ij[4];
                basis_ij[0] = constants_ij[knot_posn_ij][0] + 
                                (r_ij*constants_ij[knot_posn_ij][1]) +
                                (rsq_ij*constants_ij[knot_posn_ij][2]) +
                                (rth_ij*constants_ij[knot_posn_ij][3]);

                basis_ij[1] = constants_ij[knot_posn_ij-1][4] +
                                (r_ij*constants_ij[knot_posn_ij-1][5]) +
                                (rsq_ij*constants_ij[knot_posn_ij-1][6]) +
                                (rth_ij*constants_ij[knot_posn_ij-1][7]);

                basis_ij[2] = constants_ij[knot_posn_ij-2][8] +
                                (r_ij*constants_ij[knot_posn_ij-2][9]) +
                                (rsq_ij*constants_ij[knot_posn_ij-2][10]) +
                                (rth_ij*constants_ij[knot_posn_ij-2][11]);

                basis_ij[3] = constants_ij[knot_posn_ij-3][12] +
                                (r_ij*constants_ij[knot_posn_ij-3][13]) +
                                (rsq_ij*constants_ij[knot_posn_ij-3][14]) +
                                (rth_ij*constants_ij[knot_posn_ij-3][15]);

                double basis_ik[4];
                basis_ik[0] = constants_ik[knot_posn_ik][0] + 
                                (r_ik*constants_ik[knot_posn_ik][1]) +
                                (rsq_ik*constants_ik[knot_posn_ik][2]) +
                                (rth_ik*constants_ik[knot_posn_ik][3]);

                basis_ik[1] = constants_ik[knot_posn_ik-1][4] +
                                (r_ik*constants_ik[knot_posn_ik-1][5]) +
                                (rsq_ik*constants_ik[knot_posn_ik-1][6]) +
                                (rth_ik*constants_ik[knot_posn_ik-1][7]);

                basis_ik[2] = constants_ik[knot_posn_ik-2][8] +
                                (r_ik*constants_ik[knot_posn_ik-2][9]) +
                                (rsq_ik*constants_ik[knot_posn_ik-2][10]) +
                                (rth_ik*constants_ik[knot_posn_ik-2][11]);

                basis_ik[3] = constants_ik[knot_posn_ik-3][12] +
                                (r_ik*constants_ik[knot_posn_ik-3][13]) +
                                (rsq_ik*constants_ik[knot_posn_ik-3][14]) +
                                (rth_ik*constants_ik[knot_posn_ik-3][15]);


                double basis_jk[4];
                basis_jk[0] = constants_jk[knot_posn_jk][0] + 
                                (r_jk*constants_jk[knot_posn_jk][1]) +
                                (rsq_jk*constants_jk[knot_posn_jk][2]) +
                                (rth_jk*constants_jk[knot_posn_jk][3]);

                basis_jk[1] = constants_jk[knot_posn_jk-1][4] +
                                (r_jk*constants_jk[knot_posn_jk-1][5]) +
                                (rsq_jk*constants_jk[knot_posn_jk-1][6]) +
                                (rth_jk*constants_jk[knot_posn_jk-1][7]);

                basis_jk[2] = constants_jk[knot_posn_jk-2][8] +
                                (r_jk*constants_jk[knot_posn_jk-2][9]) +
                                (rsq_jk*constants_jk[knot_posn_jk-2][10]) +
                                (rth_jk*constants_jk[knot_posn_jk-2][11]);

                basis_jk[3] = constants_jk[knot_posn_jk-3][12] +
                                (r_jk*constants_jk[knot_posn_jk-3][13]) +
                                (rsq_jk*constants_jk[knot_posn_jk-3][14]) +
                                (rth_jk*constants_jk[knot_posn_jk-3][15]);

                for (int x=0; x<4; x++) {
                  for (int y=0; y<4; y++) {
                    for (int z=0; z<4; z++) {
                      const int temp_index = ((knot_posn_ij-x)*bm*bn) + 
                                                ((knot_posn_ik-y)*bn) + 
                                                (knot_posn_jk-z);
                      atomic_3b_Reprn_matrix_energy[temp_index] += basis_ij[x]*basis_ik[y]*basis_jk[z];
                    }
                  }
                }
}


std::array<double, 4>  UltraFastFeaturize::get_basis_set(const double r_ij,
                                                         const double rsq_ij,
                                                         const double rth_ij,
                                                         const std::vector<std::vector<double>>& constants_ij,
                                                         const int knot_posn_ij)
{
  //basis_posn same as knot_posn
  std::array<double, 4> basis_ij;
  basis_ij[0] = constants_ij[knot_posn_ij][0] + 
                                (r_ij*constants_ij[knot_posn_ij][1]) +
                                (rsq_ij*constants_ij[knot_posn_ij][2]) +
                                (rth_ij*constants_ij[knot_posn_ij][3]);

  basis_ij[1] = constants_ij[knot_posn_ij-1][4] +
                                (r_ij*constants_ij[knot_posn_ij-1][5]) +
                                (rsq_ij*constants_ij[knot_posn_ij-1][6]) +
                                (rth_ij*constants_ij[knot_posn_ij-1][7]);

  basis_ij[2] = constants_ij[knot_posn_ij-2][8] +
                                (r_ij*constants_ij[knot_posn_ij-2][9]) +
                                (rsq_ij*constants_ij[knot_posn_ij-2][10]) +
                                (rth_ij*constants_ij[knot_posn_ij-2][11]);

  basis_ij[3] = constants_ij[knot_posn_ij-3][12] +
                                (r_ij*constants_ij[knot_posn_ij-3][13]) +
                                (rsq_ij*constants_ij[knot_posn_ij-3][14]) +
                                (rth_ij*constants_ij[knot_posn_ij-3][15]);
    return basis_ij;
}

std::array<double, 3>  UltraFastFeaturize::get_basis_deri_set(const double r_ij,
                                                              const double rsq_ij,
                                                              const std::vector<std::vector<double>>& constants_ij_deri,
                                                              const int knot_posn_ij)
{
  std::array<double, 3> basis_ij_deri;
  basis_ij_deri[0] = constants_ij_deri[knot_posn_ij][0] +
                                     (r_ij*constants_ij_deri[knot_posn_ij][1]) +
                                     (rsq_ij*constants_ij_deri[knot_posn_ij][2]);

  basis_ij_deri[1] = constants_ij_deri[knot_posn_ij-1][3] +
                                     (r_ij*constants_ij_deri[knot_posn_ij-1][4]) +
                                     (rsq_ij*constants_ij_deri[knot_posn_ij-1][5]);
                       
  basis_ij_deri[2] = constants_ij_deri[knot_posn_ij-2][6] +
                                     (r_ij*constants_ij_deri[knot_posn_ij-2][7]) +
                                     (rsq_ij*constants_ij_deri[knot_posn_ij-2][8]);
  return basis_ij_deri;
}
/*void UltraFastFeaturize::write_hdf5(const hsize_t num_rows, const hsize_t num_cols,
                               const int batch_num, const H5::H5File &file_fp,
                               const std::vector<double> &Data,
                               int first_crystal_CI, int tot_complete_crystals,
                               std::vector<std::string> &column_names)
{
  // Create a unique group name for each batch
  //Group group1 = file.createGroup("/features_000");
  // Create a unique dataset name for each batch
  std::string dataset_name = "feature_" + std::to_string(write_hdf5_counter);

  // Create the data space for the dataset
  hsize_t dims[2] = {num_rows, num_cols};

  H5::DataSpace data_space(2, dims);

  H5::DataSet data_set = file_fp.createDataSet(dataset_name,
                                               H5::PredType::NATIVE_DOUBLE,
                                               data_space);
  // Write data to the dataset from the vector
  data_set.write(Data.data(), H5::PredType::NATIVE_DOUBLE);

  // Close the dataset
  data_set.close();
  data_space.close();
  write_hdf5_counter++;
}*/

/*void UltraFastFeaturize::evaluate_2b(const double &r, const double &rsq,
                                const double &rth, const int &num_knots,
                                const std::vector<double> &knots)
{
}*/

/*
py::list convert_vector_of_vectors(const std::vector<std::vector<int>>& input) {
    py::list list;
    for (const auto& vec : input) {
        list.append(py::cast(vec)); // Convert each std::vector<int> to a NumPy array
    }
    return list;
}*/
