/* the PS2's textures from the disc, in the PSP's textures.cpp's place: the
 * place of a texture of a global index, name and file, by the index of the
 * disc's textures (textures.bin), as engine/pc/pvr.cpp's find_place picks
 * it, and its texels from the pack beside it (textures.pak), which the host
 * made of each place ahead of time (engine/tools/textures_pack.cpp). the
 * console decodes nothing: it reads the texels and the GS's raster takes
 * them as they lie (gs.cpp) */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "draw.h"
#include "texels.h"

namespace pc {
/* gs.cpp's: the raster's texture of texels laid out as head says, which it
 * keeps, or 0 */
uint32_t ps2_texture_add_texels(const ps2::TexelsHead &head, void *texels);
} // namespace pc

namespace {

FILE *index_file, *pack_file;
std::vector<ps2::IndexFile> files;
std::vector<ps2::IndexPlace> places;
bool opened, missing;

bool open_files()
{
    ps2::IndexHead head;
    ps2::PackHead pack;

    if (opened || missing) {
        return opened;
    }
    index_file = fopen("textures.bin", "rb");
    pack_file = fopen("textures.pak", "rb");
    if (index_file == nullptr || pack_file == nullptr || fread(&head, sizeof head, 1, index_file) != 1 ||
        head.magic != ps2::INDEX_MAGIC || fread(&pack, sizeof pack, 1, pack_file) != 1 ||
        pack.magic != ps2::PACK_MAGIC || pack.places != head.places) {
        fprintf(stderr, "sa1: no textures: textures.bin and textures.pak, beside the program, are the build's "
                        "index and pack of the disc's textures\n");
        missing = true;
        return false;
    }
    files.resize(head.files);
    if (fread(files.data(), sizeof(ps2::IndexFile), head.files, index_file) != head.files) {
        missing = true;
        return false;
    }
    opened = true;
    return true;
}

/* the place in the index of the texture of global_index, name and file,
 * by engine/pc/pvr.cpp's find_place's rule: among the files file names, or
 * all, the first of the index and the name, else the first of the index,
 * else the first of the name. a global index of 0 goes by the name */
bool find_place(uint32_t global_index, const std::string &name, uint32_t file, uint32_t &found)
{
    bool by_rule = false, by_name = false, rule_named = false;
    uint32_t second = 0;
    long places_at = static_cast<long>(sizeof(ps2::IndexHead) + files.size() * sizeof(ps2::IndexFile));

    for (const ps2::IndexFile &f : files) {
        if (file != 0 && f.id != file) {
            continue;
        }
        places.resize(f.count);
        if (fseek(index_file, places_at + static_cast<long>(f.first * sizeof(ps2::IndexPlace)), SEEK_SET) != 0 ||
            fread(places.data(), sizeof(ps2::IndexPlace), f.count, index_file) != f.count) {
            continue;
        }
        for (uint32_t i = 0; i < f.count; i++) {
            const ps2::IndexPlace &p = places[i];
            bool same = name == std::string(p.name, strnlen(p.name, sizeof p.name));
            bool rule = global_index == 0 ? same : p.global_index == global_index;

            if (rule && (!by_rule || (same && !rule_named))) {
                found = f.first + i;
                by_rule = true;
                rule_named = same;
            } else if (same && !by_name && !name.empty()) {
                second = f.first + i;
                by_name = true;
            }
        }
        if (by_rule && rule_named) {
            break;
        }
    }
    if (!by_rule && by_name) {
        found = second;
    }
    return by_rule || by_name;
}

} // namespace

extern "C" uint32_t psp_texture_load(uint32_t global_index, const char *name, uint32_t file)
{
    uint32_t place = 0, at = 0;
    ps2::TexelsHead head;

    if (!open_files() || !find_place(global_index, name != nullptr ? name : "", file, place)) {
        return 0;
    }
    if (fseek(pack_file, static_cast<long>(sizeof(ps2::PackHead) + place * sizeof(uint32_t)), SEEK_SET) != 0 ||
        fread(&at, sizeof at, 1, pack_file) != 1 || at == 0 || fseek(pack_file, static_cast<long>(at), SEEK_SET) != 0 ||
        fread(&head, sizeof head, 1, pack_file) != 1) {
        return 0;
    }
    void *texels = memalign(128, head.bytes);

    if (texels == nullptr) {
        return 0;
    }
    if (fread(texels, 1, head.bytes, pack_file) != head.bytes) {
        free(texels);
        return 0;
    }
    uint32_t made = pc::ps2_texture_add_texels(head, texels);

    if (made == 0) {
        free(texels);
    }
    return made;
}
