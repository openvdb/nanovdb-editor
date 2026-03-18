// voxelbvh_nanovdb_level_mask_flatten.slang

#include "voxelbvh_nanovdb_iterate.slang"

void iterate_main(pnanovdb_address_t node_arr, pnanovdb_address_t val_addr, uint level, uint n, pnanovdb_coord_t ijk)
{
    if (pnanovdb_address_is_null(val_addr))
    {
        return;
    }

    pnanovdb_uint64_t val = pnanovdb_read_uint64(buf_scratch, val_addr);

    pnanovdb_write_uint64(buf, val_addr, val);
}
