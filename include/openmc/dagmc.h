
#ifndef OPENMC_DAGMC_H
#define OPENMC_DAGMC_H

namespace openmc {
extern "C" const bool dagmc_enabled;
}

#ifdef DAGMC

#include "DagMC.hpp"
#include "openmc/xml_interface.h"
#include "openmc/position.h"
#include "uwuw.hpp"
#include "dagmcmetadata.hpp"

namespace openmc {

namespace model {
  extern moab::DagMC* DAG;
}

//==============================================================================
// Non-member functions
//==============================================================================

void load_dagmc_geometry();
void init_dagmc();
void init_dagmc_metadata(std::shared_ptr<dagmcMetaData>& dmd_ptr);
void init_uwuw_materials(std::shared_ptr<UWUW>& uwuw_ptr);
void init_dagmc_universe(int32_t dagmc_univ_id);
void init_dagmc_cells(std::shared_ptr<dagmcMetaData> dmd_ptr,
                      std::shared_ptr<UWUW> uwuw_ptr,
                      moab::EntityHandle& graveyard);
void init_dagmc_surfaces(std::shared_ptr<dagmcMetaData> dmd_ptr,
                         moab::EntityHandle& graveyard);
void create_dagmc_cell(int index,int32_t dagmc_univ_id);
void set_dagmc_cell_properties(int index,
                               std::shared_ptr<dagmcMetaData> dmd_ptr,
                               std::shared_ptr<UWUW> uwuw_ptr,
                               moab::EntityHandle& graveyard);
int get_material_id(moab::EntityHandle vol_handle,
                    std::shared_ptr<dagmcMetaData> dmd_ptr,
                    std::shared_ptr<UWUW> uwuw_ptr,
                    moab::EntityHandle& graveyard);
double get_material_temperature(moab::EntityHandle vol_handle,int mat_id);
void free_memory_dagmc();
void read_geometry_dagmc();
bool read_uwuw_materials(pugi::xml_document& doc);
bool get_uwuw_materials_xml(std::string& s);

} // namespace openmc

#endif // DAGMC

#endif // OPENMC_DAGMC_H
