/* the PS2's textures from the disc, in the PSP's textures.cpp's place
 * (engine/ps2/TEXTURES.md). a texture of a global index, name and file comes
 * from the file's TIM2, textures/NAME.tm2 beside the program, which the
 * host made of each of the disc's texture files ahead of time
 * (engine/tools/textures_tm2.cpp): read whole at the game's own load of the
 * file, where records.c's sa1_textures_loaded makes the load's textures,
 * and kept in memory where textures/resident.txt names it. the console
 * decodes nothing.
 *
 * the pack stands in for a texture no TIM2 holds: the place of a texture by
 * the index of the disc's textures (textures.bin), as engine/pc/pvr.cpp's
 * find_place picks it, and its texels from the pack beside it
 * (textures.pak, engine/tools/textures_pack.cpp) */
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

extern "C" {
#include <kernel.h>
}

#include "draw.h"
#include "pspkernel.h"
#include "texels.h"
#include "tim2.h"

extern "C" {
/* the run's vsyncs, the runner's, and the game's mode and its stage's, which
 * say whether a read came during play (src/main.h, src/stage/stage.c) */
extern unsigned long native_vsyncs;
extern int game_mode;
extern short stage_mode;
/* textures pak in the settings: the pack's textures, as before the TIM2
 * files (engine/ps2/main.c) */
extern int ps2_textures_pak;
}

namespace pc {
/* gs.cpp's: the raster's texture of texels laid out as head says, which it
 * keeps, or 0 */
uint32_t ps2_texture_add_texels(const ps2::TexelsHead &head, void *texels);
/* and of a TIM2's picture, which draws from the lender's bytes */
uint32_t ps2_texture_add_picture(const ps2::Lent &lent);
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

/* the texture of global_index, name and file from the pack, or 0 */
uint32_t pack_load(uint32_t global_index, const char *name, uint32_t file)
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

/* the TIM2 files (engine/ps2/tim2.h), each read whole into one buffer: a
 * name's file, its pictures' places in it, and whether it stays in memory
 * from its first read to the run's end (textures/resident.txt) */
struct Picture {
    uint32_t global_index;
    std::string name;
    uint32_t at; /* its head's byte in the file */
};
struct Tim2 {
    std::string name;
    uint8_t *bytes = nullptr;
    uint32_t size = 0;
    std::vector<Picture> pictures;
    /* it stays in memory to the run's end, and its textures in a fixed
     * place in the GS's memory too, whose numbers the raster keeps, by
     * picture */
    bool resident = false, fixed = false;
    std::map<uint32_t, uint32_t> fixed_made;
    /* the raster's textures of its pictures, which draw from its bytes */
    uint32_t lent = 0;
};

/* each texture file's name id, the game's, to its TIM2's name
 * (textures/files.txt); the residents' names and tiers; the files read */
std::map<uint32_t, std::string> tm2_names;
std::map<std::string, std::string> residents;
std::map<std::string, Tim2 *> tm2_open;
bool tm2_read, tm2_missing;
/* whether a load of the game's is making its list's textures: the files
 * read for it stay till its end */
int tm2_batch;

void read_tables()
{
    char line[160];

    if (tm2_read) {
        return;
    }
    tm2_read = true;
    FILE *in = fopen("textures/files.txt", "r");

    if (in == nullptr) {
        fprintf(stderr, "sa1: no textures/files.txt beside the program: the textures come from the pack\n");
        tm2_missing = true;
        return;
    }
    while (fgets(line, sizeof line, in) != nullptr) {
        unsigned id;
        char name[96];

        if (sscanf(line, "%x %95s", &id, name) == 2) {
            tm2_names[id] = name;
        }
    }
    fclose(in);
    /* resident.txt: a line for each name kept, its tier after it */
    if ((in = fopen("textures/resident.txt", "r")) != nullptr) {
        while (fgets(line, sizeof line, in) != nullptr) {
            char name[96], tier[32];

            if (line[0] != '#' && sscanf(line, "%95s %31s", name, tier) == 2) {
                residents[name] = tier;
            }
        }
        fclose(in);
    }
}

/* whether the player is playing: a stage, field or trial at its play, or
 * its fade from black, in which it plays too (src/main.h's GAME_MODE_ACTION,
 * GAME_MODE_FIELD and GAME_MODE_TRIAL, src/stage/stage.c's STAGE_FADE_IN
 * and STAGE_PLAY). a read at any other time comes in the game's own pause */
bool during_play()
{
    return (game_mode == 4 || game_mode == 5 || game_mode == 9) && (stage_mode == 4 || stage_mode == 14);
}

void close_tm2(Tim2 *t)
{
    tm2_open.erase(t->name);
    free(t->bytes);
    delete t;
}

/* the TIM2 of the game's file of name id file, read now where it isn't
 * open, or null */
Tim2 *open_tm2(uint32_t file)
{
    read_tables();
    auto named = tm2_names.find(file);

    if (tm2_missing || named == tm2_names.end()) {
        return nullptr;
    }
    auto open = tm2_open.find(named->second);

    if (open != tm2_open.end()) {
        return open->second;
    }
    std::string path = "textures/" + named->second + ".tm2";
    uint32_t began = sceKernelGetSystemTimeLow();
    FILE *in = fopen(path.c_str(), "rb");

    if (in == nullptr) {
        return nullptr;
    }
    fseek(in, 0, SEEK_END);
    long size = ftell(in);

    fseek(in, 0, SEEK_SET);
    Tim2 *t = new Tim2;

    t->name = named->second;
    t->size = static_cast<uint32_t>(size);
    t->bytes = static_cast<uint8_t *>(memalign(128, static_cast<size_t>(size)));
    if (t->bytes == nullptr || size < static_cast<long>(sizeof(ps2::Tim2Head)) ||
        fread(t->bytes, 1, static_cast<size_t>(size), in) != static_cast<size_t>(size)) {
        fclose(in);
        free(t->bytes);
        delete t;
        return nullptr;
    }
    fclose(in);
    ps2::Tim2Head head;

    memcpy(&head, t->bytes, sizeof head);
    uint32_t at = head.format == 1 ? ps2::TIM2_ALIGN : sizeof head;

    for (uint32_t i = 0; i < head.pictures && at + sizeof(ps2::Tim2Picture) <= t->size; i++) {
        ps2::Tim2Picture p;
        ps2::Tim2Terms terms;

        memcpy(&p, t->bytes + at, sizeof p);
        memcpy(&terms, t->bytes + at + sizeof p + sizeof(ps2::Tim2Extended), sizeof terms);
        const char *name = reinterpret_cast<const char *>(t->bytes + at + sizeof p + sizeof(ps2::Tim2Extended) +
                                                          sizeof terms);

        t->pictures.push_back({terms.global_index, std::string(name, strnlen(name, 64)), at});
        at += p.total;
    }
    auto tier = residents.find(t->name);

    t->resident = tier != residents.end();
    t->fixed = t->resident && tier->second == "fixed";
    tm2_open[t->name] = t;
    /* the GS's DMA reads the texels from memory, past the EE's cache */
    FlushCache(0);
    fprintf(stderr, "sa1: textures %s.tm2, %u bytes, %u pictures, read in %u us at vsync %lu, %s%s\n",
            t->name.c_str(), t->size, static_cast<unsigned>(t->pictures.size()),
            static_cast<unsigned>(sceKernelGetSystemTimeLow() - began), native_vsyncs,
            during_play() ? "during play" : "at a pause",
            t->fixed ? ", fixed" : t->resident ? ", resident" : "");
    return t;
}

/* the raster's texture of a picture, drawn from t's bytes as they lie: its
 * texels in the GS's format, and its CLUT. a fixed file's picture is made
 * once, and the raster keeps it past the game's releases */
uint32_t make_picture(Tim2 &t, const Picture &picture)
{
    uint32_t index = static_cast<uint32_t>(&picture - t.pictures.data());
    auto kept = t.fixed_made.find(index);

    if (kept != t.fixed_made.end()) {
        return kept->second;
    }
    ps2::Tim2Picture p;
    ps2::Tim2Terms terms;
    const uint8_t *head = t.bytes + picture.at;

    memcpy(&p, head, sizeof p);
    memcpy(&terms, head + sizeof p + sizeof(ps2::Tim2Extended), sizeof terms);
    const uint8_t *texels = head + p.head_bytes;
    ps2::Lent lent = {terms.width,
                      terms.height,
                      p.width,
                      p.height,
                      p.image_type == ps2::TIM2_4   ? ps2::GS_PSM_4
                      : p.image_type == ps2::TIM2_8 ? ps2::GS_PSM_8
                      : p.image_type == ps2::TIM2_32 ? ps2::GS_PSM_32
                                                     : ps2::GS_PSM_16,
                      texels,
                      p.clut_colors != 0 ? texels + p.image_bytes : nullptr,
                      p.clut_colors,
                      (terms.flags & ps2::TERMS_CUTS) != 0,
                      &t,
                      t.fixed};
    uint32_t id = pc::ps2_texture_add_picture(lent);

    if (id != 0) {
        t.lent++;
        if (t.fixed) {
            t.fixed_made[index] = id;
        }
    }
    return id;
}

/* the picture of global_index, or else of name, in t: find_place's rule
 * within one file */
const Picture *find_picture(const Tim2 &t, uint32_t global_index, const std::string &name)
{
    const Picture *by_name = nullptr;

    for (const Picture &p : t.pictures) {
        if (global_index != 0 && p.global_index == global_index && (name.empty() || p.name == name)) {
            return &p;
        }
        if (global_index != 0 && p.global_index == global_index && by_name == nullptr) {
            by_name = &p;
        }
    }
    if (by_name != nullptr) {
        return by_name;
    }
    for (const Picture &p : t.pictures) {
        if (!name.empty() && p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

/* a file read outside a load's batch, or past its end, closes unless it
 * stays or a texture still draws from it */
void close_spent()
{
    for (auto it = tm2_open.begin(); it != tm2_open.end();) {
        Tim2 *t = (it++)->second;

        if (!t->resident && t->lent == 0) {
            close_tm2(t);
        }
    }
}

} // namespace

namespace pc {
/* the raster dropped one of a file's textures, and no stream reads it: the
 * file closes once none is left, unless it stays */
void ps2_texture_returned(void *lender)
{
    Tim2 *t = static_cast<Tim2 *>(lender);

    if (t->lent > 0 && --t->lent == 0 && !t->resident && tm2_batch == 0) {
        close_tm2(t);
    }
}
} // namespace pc

/* the texture of a global index, name and file, from the file's TIM2: read
 * now where it isn't open, at the game's own load where the load names its
 * textures (records.c's sa1_textures_loaded), else at a draw. the pack
 * stands in where no TIM2 holds it, or where the settings' textures pak
 * asks */
extern "C" uint32_t psp_texture_load(uint32_t global_index, const char *name, uint32_t file)
{
    std::string named = name != nullptr ? name : "";
    uint32_t made = 0;

    if (!ps2_textures_pak) {
        Tim2 *t = file != 0 ? open_tm2(file) : nullptr;
        const Picture *p = t != nullptr ? find_picture(*t, global_index, named) : nullptr;

        /* a texture of no file's, or one its file lacks: any open file's */
        for (auto it = tm2_open.begin(); p == nullptr && it != tm2_open.end(); ++it) {
            t = it->second;
            p = find_picture(*t, global_index, named);
        }
        if (p != nullptr) {
            made = make_picture(*t, *p);
        }
        if (tm2_batch == 0) {
            close_spent();
        }
        if (made != 0) {
            return made;
        }
        static int said;

        if (said++ < 40) {
            auto named_file = tm2_names.find(file);

            fprintf(stderr, "sa1: no TIM2 holds the texture of global index %u, %s, of %s: the pack's\n",
                    static_cast<unsigned>(global_index), named.c_str(),
                    named_file != tm2_names.end() ? named_file->second.c_str() : "no file");
        }
    }
    return pack_load(global_index, name, file);
}

/* a load of the game's makes its textures between these: its files stay
 * open till the end, for the next of its textures, and close then unless
 * they stay */
extern "C" void ps2_textures_begin(void)
{
    tm2_batch++;
}

extern "C" void ps2_textures_end(void)
{
    if (tm2_batch > 0 && --tm2_batch == 0) {
        close_spent();
    }
}
