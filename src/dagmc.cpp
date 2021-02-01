#include "openmc/dagmc.h"

#include "openmc/cell.h"
#include "openmc/constants.h"
#include "openmc/container_util.h"
#include "openmc/error.h"
#include "openmc/file_utils.h"
#include "openmc/geometry.h"
#include "openmc/geometry_aux.h"
#include "openmc/material.h"
#include "openmc/string_utils.h"
#include "openmc/settings.h"
#include "openmc/surface.h"

#include <fmt/core.h>

#include <string>
#include <sstream>
#include <algorithm>
#include <fstream>

namespace openmc {

#ifdef DAGMC
const bool dagmc_enabled = true;
#else
const bool dagmc_enabled = false;
#endif

}

#ifdef DAGMC

namespace openmc {

const std::string DAGMC_FILENAME = "dagmc.h5m";

namespace model {

moab::DagMC* DAG;

} // namespace model


std::string dagmc_file() {
  std::string filename = settings::path_input + DAGMC_FILENAME;
  if (!file_exists(filename)) {
    fatal_error("Geometry DAGMC file '" + filename + "' does not exist!");
  }
  return filename;
}

bool get_uwuw_materials_xml(std::string& s) {
  std::string filename = dagmc_file();
  UWUW uwuw(filename.c_str());

  std::stringstream ss;
  bool uwuw_mats_present = false;
  if (uwuw.material_library.size() != 0) {
    uwuw_mats_present = true;
    // write header
    ss << "<?xml version=\"1.0\"?>\n";
    ss << "<materials>\n";
    const auto& mat_lib = uwuw.material_library;
    // write materials
    for (auto mat : mat_lib) { ss << mat.second->openmc("atom"); }
    // write footer
    ss << "</materials>";
    s = ss.str();
  }

  return uwuw_mats_present;
}

bool read_uwuw_materials(pugi::xml_document& doc) {
  std::string s;
  bool found_uwuw_mats = get_uwuw_materials_xml(s);
  if (found_uwuw_mats) {
    pugi::xml_parse_result result = doc.load_string(s.c_str());
    if (!result) {
      throw std::runtime_error{"Error reading UWUW materials"};
    }
  }
  return found_uwuw_mats;
}

bool write_uwuw_materials_xml() {
  std::string s;
  bool found_uwuw_mats = get_uwuw_materials_xml(s);
    // if there is a material library in the file
  if (found_uwuw_mats) {
    // write a material.xml file
    std::ofstream mats_xml("materials.xml");
    mats_xml << s;
    mats_xml.close();
  }

  return found_uwuw_mats;
}

int legacy_assign_material(std::string mat_string)
{
  int mat_id;
  bool mat_found_by_name = false;
  // attempt to find a material with a matching name
  to_lower(mat_string);
  for (const auto& m : model::materials) {
    std::string m_name = m->name();
    to_lower(m_name);
    if (mat_string == m_name) {
      // assign the material with that name
      if (!mat_found_by_name) {
        mat_found_by_name = true;
        mat_id = m->id_;
      // report error if more than one material is found
      } else {
        fatal_error(fmt::format(
          "More than one material found with name {}. Please ensure materials "
          "have unique names if using this property to assign materials.",
          mat_string));
      }
    }
  }

  // if no material was set using a name, assign by id
  if (!mat_found_by_name) {
    try {
      auto id = std::stoi(mat_string);
      mat_id = id;
    } catch (const std::invalid_argument&) {
      fatal_error(fmt::format(
        "Could not convert material name {} to id", mat_string));
    }
  }

  if (settings::verbosity >= 10) {
    const auto& m = model::materials[model::material_map.at(mat_id)];
    std::stringstream msg;
    msg << "DAGMC material " << mat_string << " was assigned";
    if (mat_found_by_name) {
      msg << " using material name: " << m->name_;
    } else {
      msg << " using material id: " << m->id_;
    }
    write_message(msg.str(), 10);
  }

  return mat_id;
}

int uwuw_assign_material(moab::EntityHandle vol_handle,
                         std::shared_ptr<dagmcMetaData> dmd_ptr,
                         std::shared_ptr<UWUW> uwuw_ptr)
{
  std::string uwuw_mat = dmd_ptr->volume_material_property_data_eh[vol_handle];
  if (uwuw_ptr->material_library.count(uwuw_mat) == 0) {
    fatal_error(fmt::format("Material with value {} not found in the "
                            "UWUW material library", uwuw_mat));
  }

  // Note: material numbers are set by UWUW
  return uwuw_ptr->material_library.get_material(uwuw_mat).metadata["mat_number"].asInt();
}

void load_dagmc_geometry()
{

  // Set the model::DAG pointer and initialise mesh data
  init_dagmc();

  // Create a material library
  std::shared_ptr<UWUW> uwuw_ptr;
  init_uwuw_materials(uwuw_ptr);

  // Parse DAGMC metadata
  std::shared_ptr<dagmcMetaData> dmd_ptr;
  init_dagmc_metadata(dmd_ptr);

  // Initialise cells and save which entity is the graveyard
  moab::EntityHandle graveyard = 0;
  init_dagmc_cells(dmd_ptr,uwuw_ptr,graveyard);

  // Initialise surfaces
  init_dagmc_surfaces(dmd_ptr,graveyard);

  return;
}

void init_dagmc()
{
  if (!model::DAG) {
    model::DAG = new moab::DagMC();
  }

  // Load the DAGMC geometry
  moab::ErrorCode rval = model::DAG->load_file(dagmc_file().c_str());
  MB_CHK_ERR_CONT(rval);

  // Initialize acceleration data structures
  rval = model::DAG->init_OBBTree();
  MB_CHK_ERR_CONT(rval);

  // Apply the "temp" keyword tag to any volumes in material groups with this tag
  std::vector<std::string> keywords {"temp"};
  std::map<std::string, std::string> dum;
  std::string delimiters = ":/";
  rval = model::DAG->parse_properties(keywords, dum, delimiters.c_str());
  MB_CHK_ERR_CONT(rval);
}


void init_dagmc_metadata(std::shared_ptr<dagmcMetaData>& dmd_ptr)
{
  // Create dagmcMetaData pointer
  dmd_ptr = std::make_shared<dagmcMetaData>(model::DAG, false, false);

  // Parse model metadata
  dmd_ptr->load_property_data();
}

void init_uwuw_materials(std::shared_ptr<UWUW>& uwuw_ptr)
{
  // Create UWUW instance and store pointer
  uwuw_ptr = std::make_shared<UWUW>(dagmc_file().c_str());

  // Notify user if UWUW materials are going to be used
  if (!uwuw_ptr->material_library.empty()) {
    write_message("Found UWUW Materials in the DAGMC geometry file.", 6);
  }
}

void init_dagmc_universe(int32_t dagmc_univ_id)
{
  // Populate the Universe vector and dict
  auto it = model::universe_map.find(dagmc_univ_id);
  if (it == model::universe_map.end()) {
    model::universes.push_back(std::make_unique<Universe>());
    model::universes.back()->id_ = dagmc_univ_id;
    model::universe_map[dagmc_univ_id] = model::universes.size() - 1;
  }
}

void init_dagmc_cells(std::shared_ptr<dagmcMetaData> dmd_ptr,
                      std::shared_ptr<UWUW> uwuw_ptr,
                      moab::EntityHandle& graveyard)
{
  // Universe is always 0 for DAGMC runs
  int32_t dagmc_univ_id = 0;
  init_dagmc_universe(dagmc_univ_id);

  // Get number of cells (volumes) from DAGMC
  int n_cells = model::DAG->num_entities(3);

  // Loop over the cells
  for (int i = 0; i < n_cells; i++) {

    // DagMC indices are offset by one (convention stemming from MCNP)
    unsigned int index = i+1;
    int id= model::DAG->id_by_index(3, index);

    // set cell ids using global IDs
    DAGCell* c = new DAGCell();
    c->dag_index_ = index;
    c->id_ = id;
    c->dagmc_ptr_ = model::DAG;
    c->universe_ = dagmc_univ_id; // set to zero for now
    c->fill_ = C_NONE; // no fill, single universe

    // Save cell
    model::cells.emplace_back(c);
    model::cell_map[c->id_] = i;
    model::universes[model::universe_map[dagmc_univ_id]]->cells_.push_back(i);


    // Set cell properties based on metadata
    moab::EntityHandle vol_handle = model::DAG->entity_by_index(3, index);

    // Set cell material
    int mat_id = get_material_id(vol_handle,dmd_ptr,uwuw_ptr,graveyard);
    c->material_.push_back(mat_id);

    // Set cell temperature if not a void material
    if (mat_id != MATERIAL_VOID){
      double temp = get_material_temperature(vol_handle,mat_id);
      c->sqrtkT_.push_back(std::sqrt(K_BOLTZMANN * temp));
    };


  }

  // allocate the cell overlap count if necessary
  if (settings::check_overlaps) {
    model::overlap_check_count.resize(model::cells.size(), 0);
  }

  if (!graveyard) {
    warning("No graveyard volume found in the DagMC model."
            "This may result in lost particles and rapid simulation failure.");
  }
}

void init_dagmc_surfaces(std::shared_ptr<dagmcMetaData> dmd_ptr,
                         moab::EntityHandle& graveyard)
{
  // Get number of surfaces from DAGMC
  int n_surfaces = model::DAG->num_entities(2);

  // Loop over the surfaces
  for (int i = 0; i < n_surfaces; i++) {
    moab::EntityHandle surf_handle = model::DAG->entity_by_index(2, i+1);

    // set cell ids using global IDs
    DAGSurface* s = new DAGSurface();
    s->dag_index_ = i+1;
    s->id_ = model::DAG->id_by_index(2, s->dag_index_);
    s->dagmc_ptr_ = model::DAG;

    if (contains(settings::source_write_surf_id, s->id_)) {
      s->surf_source_ = true;
    }

    // set BCs
    std::string bc_value = dmd_ptr->get_surface_property("boundary", surf_handle);
    to_lower(bc_value);
    if (bc_value.empty() || bc_value == "transmit" || bc_value == "transmission") {
      // Leave the bc_ a nullptr
    } else if (bc_value == "vacuum") {
      s->bc_ = std::make_shared<VacuumBC>();
    } else if (bc_value == "reflective" || bc_value == "reflect" || bc_value == "reflecting") {
      s->bc_ = std::make_shared<ReflectiveBC>();
    } else if (bc_value == "white") {
      fatal_error("White boundary condition not supported in DAGMC.");
    } else if (bc_value == "periodic") {
      fatal_error("Periodic boundary condition not supported in DAGMC.");
    } else {
      fatal_error(fmt::format("Unknown boundary condition \"{}\" specified "
        "on surface {}", bc_value, s->id_));
    }

    // graveyard check
    moab::Range parent_vols;
    moab::ErrorCode rval = model::DAG->moab_instance()->get_parent_meshsets(surf_handle, parent_vols);
    MB_CHK_ERR_CONT(rval);

    // if this surface belongs to the graveyard
    if (graveyard && parent_vols.find(graveyard) != parent_vols.end()) {
      // set graveyard surface BC's to vacuum
      s->bc_ = std::make_shared<VacuumBC>();
    }

    // add to global array and map
    model::surfaces.emplace_back(s);
    model::surface_map[s->id_] = i;
  }
}

int get_material_id(moab::EntityHandle vol_handle,
                    std::shared_ptr<dagmcMetaData> dmd_ptr,
                    std::shared_ptr<UWUW> uwuw_ptr,
                    moab::EntityHandle& graveyard)
{

  // Determine volume material assignment
  std::string mat_str = dmd_ptr->get_volume_property("material", vol_handle);
  if (mat_str.empty()) {
    fatal_error(fmt::format("Volume handle {} has no material assignment.", vol_handle));
  }
  to_lower(mat_str);


  // Find id for special case: void material
  if (mat_str == "void" || mat_str == "vacuum" || mat_str == "graveyard") {
    // If we found the graveyard, save handle
    if(mat_str == "graveyard"){
      graveyard = vol_handle;
    }
    return openmc::MATERIAL_VOID;
  }

  // Find id for non-void mats
  if (!uwuw_ptr->material_library.empty()) {
    // Look up material in uwuw if present
    return uwuw_assign_material(vol_handle,dmd_ptr,uwuw_ptr);
  }
  else {
    return legacy_assign_material(mat_str);
  }

}

double get_material_temperature(moab::EntityHandle vol_handle,int mat_id)
{
  if (model::DAG->has_prop(vol_handle, "temp")) {
    // check for temperature assignment
    std::string temp_value;
    moab::ErrorCode rval = model::DAG->prop_value(vol_handle, "temp", temp_value);
    MB_CHK_ERR_CONT(rval);
    return std::stod(temp_value);
  } else {
    const auto& mat = model::materials[model::material_map.at(mat_id)];
    return mat->temperature();
  }
}

void read_geometry_dagmc()
{
  write_message("Reading DAGMC geometry...", 5);
  load_dagmc_geometry();

  model::root_universe = find_root_universe();
}

void free_memory_dagmc()
{
  delete model::DAG;
}


}
#endif
