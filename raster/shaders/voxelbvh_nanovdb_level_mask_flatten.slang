// voxelbvh_nanovdb_level_mask_flatten.slang

#include "voxelbvh_nanovdb_iterate.slang"

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n, pnanovdb_coord_t ijk)
{
    if (pnanovdb_address_is_null(val_addr))
    {
        return;
    }

    // Note: single grid assumption
    pnanovdb_grid_handle_t grid = { pnanovdb_address_null() };
    pnanovdb_uint32_t grid_type = pnanovdb_grid_get_grid_type(buf, grid);
    pnanovdb_tree_handle_t tree = pnanovdb_grid_get_tree(buf, grid);
    pnanovdb_root_handle_t root = pnanovdb_tree_get_root(buf, tree);

    uint val_accum = 0u;
    for (uint l = 0u; l < 12; l++)
    {
        uint lmask = ~((1u << l) - 1u);
        pnanovdb_coord_t ijkmask = ijk & lmask;

        pnanovdb_address_t read_val_addr = pnanovdb_root_get_value_address(grid_type, buf, root, ijkmask);

        pnanovdb_uint32_t val = pnanovdb_read_uint32(buf, read_val_addr);

        val_accum = val_accum | (val & (1u << l));
    }

    pnanovdb_write_uint32(buf_scratch, val_addr, val_accum);
}
