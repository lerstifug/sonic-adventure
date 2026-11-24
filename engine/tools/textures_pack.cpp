/* textures_pack DISC DIR writes DIR's textures.bin and textures.pak, the
 * PS2's index and pack of the disc's textures, on the build's host.
 *
 * the index (engine/pc/pvr.cpp's, under SA_SMALL) says where each of the
 * disc's textures lies: a small console builds it at its first start, from
 * every PVM of the disc, PRS files decoded whole among them, and the PS2
 * has no memory for the largest's decoding. the host builds the same index
 * from the same disc by the same source, and both are little-endian.
 *
 * the pack holds each place's texture decoded and made the GS's texels
 * ahead of time (engine/ps2/texels.h), in the index's order, so the console
 * reads a texture's bytes and decodes nothing: a PRS file's textures there
 * would each take the file's decoding from its start, a third of a second
 * on the EE */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "disc.h"
#include "pvr.h"
#include "texels.h"

namespace {

bool read_all(const std::string &path, std::vector<uint8_t> &bytes)
{
    FILE *in = fopen(path.c_str(), "rb");

    if (in == nullptr) {
        return false;
    }
    bytes.clear();
    uint8_t block[1 << 16];
    size_t got;

    while ((got = fread(block, 1, sizeof block, in)) != 0) {
        bytes.insert(bytes.end(), block, block + got);
    }
    fclose(in);
    return true;
}

void put(std::vector<uint8_t> &out, const void *data, size_t bytes)
{
    const uint8_t *from = static_cast<const uint8_t *>(data);

    out.insert(out.end(), from, from + bytes);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "textures_pack DISC DIR\n");
        return 2;
    }
    std::string root = argv[1], dir = argv[2];
    pc::Disc disc;
    pc::Textures textures;

    /* the index goes to textures.bin in the working directory, as a run's
     * does beside its program */
    if (chdir(dir.c_str()) != 0) {
        fprintf(stderr, "textures_pack: can't go to %s\n", dir.c_str());
        return 1;
    }
    remove("textures.bin");
    disc.add_root(root);
    textures.index(disc, "SONICADV");

    std::vector<uint8_t> index;

    if (!read_all("textures.bin", index) || index.size() < sizeof(ps2::IndexHead)) {
        fprintf(stderr, "textures_pack: no index was written\n");
        return 1;
    }
    ps2::IndexHead head;

    memcpy(&head, index.data(), sizeof head);
    if (head.magic != ps2::INDEX_MAGIC ||
        index.size() < sizeof head + head.files * sizeof(ps2::IndexFile) + head.places * sizeof(ps2::IndexPlace)) {
        fprintf(stderr, "textures_pack: the index is no index\n");
        return 1;
    }
    const auto *files = reinterpret_cast<const ps2::IndexFile *>(index.data() + sizeof head);
    const auto *places = reinterpret_cast<const ps2::IndexPlace *>(files + head.files);
    std::vector<uint32_t> at(head.places, 0);
    std::vector<uint8_t> body, bytes, plain, texels;
    ps2::PackHead pack = {ps2::PACK_MAGIC, head.places};
    size_t start = sizeof pack + head.places * sizeof(uint32_t);
    uint32_t made = 0, refused = 0;

    for (uint32_t f = 0; f < head.files; f++) {
        const ps2::IndexFile &file = files[f];
        std::string path(file.path, strnlen(file.path, sizeof file.path));
        const std::vector<uint8_t> *holds = &bytes;

        if (!disc.read(path, bytes)) {
            fprintf(stderr, "textures_pack: can't read %s\n", path.c_str());
            refused += file.count;
            continue;
        }
        if (file.compressed) {
            if (!pc::prs_decode(bytes, plain)) {
                fprintf(stderr, "textures_pack: can't decode %s\n", path.c_str());
                refused += file.count;
                continue;
            }
            holds = &plain;
        }
        for (uint32_t p = file.first; p < file.first + file.count && p < head.places; p++) {
            pc::Image image;
            std::string why;
            ps2::TexelsHead th;

            if (!pc::pvr_decode(*holds, places[p].at, image, why) || !ps2::texels_of(image, th, texels)) {
                refused++;
                continue;
            }
            /* each texture starts at 16 bytes, which the EE's reads take
             * whole */
            while ((start + body.size()) % 16 != 0) {
                body.push_back(0);
            }
            at[p] = static_cast<uint32_t>(start + body.size());
            put(body, &th, sizeof th);
            put(body, texels.data(), texels.size());
            made++;
        }
    }
    FILE *out = fopen("textures.pak", "wb");

    if (out == nullptr || fwrite(&pack, sizeof pack, 1, out) != 1 ||
        fwrite(at.data(), sizeof(uint32_t), at.size(), out) != at.size() ||
        fwrite(body.data(), 1, body.size(), out) != body.size() || fclose(out) != 0) {
        fprintf(stderr, "textures_pack: can't write textures.pak\n");
        return 1;
    }
    printf("textures_pack: %u textures, %u refused, %zu bytes\n", made, refused, start + body.size());
    return 0;
}
