/* sounds_vag DISC DIR writes DIR/sound/CRC.vag for each programs bank of the
 * disc's MLT files, its samples as the SPU2's VAG (engine/ps2/vab.h,
 * engine/ps2/SOUND.md's stage 2), and DIR/sound/report.csv, on the build's
 * host.
 *
 * an MLT file is the sound driver's unit of banks: its head, a record of
 * each bank, then the banks' bytes, which the game loads whole
 * (engine/sa1/sound.c's sdMultiUnitDownload). so a bank's bytes at a load
 * are the file's, and their CRC32 names the bank's VAG. a file is written
 * only where its bytes changed, so a run folder's copy takes only those */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <map>
#include <string>
#include <vector>

#include "vab.h"

namespace {

bool read_all(const std::string &path, std::vector<uint8_t> &bytes)
{
    FILE *in = fopen(path.c_str(), "rb");
    uint8_t block[1 << 16];
    size_t got;

    if (in == nullptr) {
        return false;
    }
    bytes.clear();
    while ((got = fread(block, 1, sizeof block, in)) != 0) {
        bytes.insert(bytes.end(), block, block + got);
    }
    fclose(in);
    return true;
}

/* path written with bytes, unless it holds them already */
bool write_changed(const std::string &path, const std::vector<uint8_t> &bytes, bool &changed)
{
    std::vector<uint8_t> was;

    changed = !read_all(path, was) || was != bytes;
    if (!changed) {
        return true;
    }
    FILE *out = fopen(path.c_str(), "wb");

    return out != nullptr && fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size() && fclose(out) == 0;
}

/* the MLT files under dir, by path */
void find_mlt(const std::string &dir, std::vector<std::string> &paths)
{
    DIR *d = opendir(dir.c_str());
    struct dirent *e;

    if (d == nullptr) {
        return;
    }
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name, path = dir + "/" + name;
        struct stat st;

        if (name == "." || name == ".." || stat(path.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            find_mlt(path, paths);
        } else if (name.size() > 4 && strcasecmp(name.c_str() + name.size() - 4, ".MLT") == 0) {
            paths.push_back(path);
        }
    }
    closedir(d);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "sounds_vag DISC DIR\n");
        return 2;
    }
    std::string dir = std::string(argv[2]) + "/sound";
    std::vector<std::string> paths;
    std::map<uint32_t, std::string> written; /* each bank's CRC, once */
    uint32_t banks = 0, samples = 0, changed_count = 0;
    uint64_t total = 0;

    mkdir(argv[2], 0755);
    mkdir(dir.c_str(), 0755);
    find_mlt(argv[1], paths);
    std::string report = "file,bank,crc,samples,vag_bytes\n";

    for (const std::string &path : paths) {
        std::vector<uint8_t> unit;

        if (!read_all(path, unit) || unit.size() < 0x20 || memcmp(unit.data(), "SMLT", 4) != 0) {
            continue;
        }
        uint32_t count = ps2::vab_read32(unit.data() + 8);

        for (uint32_t b = 0; b < count && 0x20 + 0x20 * (b + 1) <= unit.size(); b++) {
            const uint8_t *record = unit.data() + 0x20 + 0x20 * b;
            uint32_t offset = ps2::vab_read32(record + 16), size = ps2::vab_read32(record + 20);

            if (size < 0x30 || offset > unit.size() || size > unit.size() - offset ||
                memcmp(unit.data() + offset, "SMPB", 4) != 0) {
                continue;
            }
            const uint8_t *bank = unit.data() + offset;
            uint32_t crc = ps2::vab_crc(bank, size);

            if (written.count(crc)) {
                continue;
            }
            std::vector<ps2::VabSample> found = ps2::vab_samples(bank, size);
            std::vector<uint8_t> vag, file;

            for (ps2::VabSample &s : found) {
                std::vector<uint8_t> one = ps2::vab_encode(bank, size, s);

                s.offset = static_cast<uint32_t>(vag.size());
                vag.insert(vag.end(), one.begin(), one.end());
            }
            ps2::VabHead head = {ps2::VAB_MAGIC, static_cast<uint32_t>(found.size()), static_cast<uint32_t>(vag.size()), 0};

            file.resize(sizeof head + found.size() * sizeof(ps2::VabSample));
            memcpy(file.data(), &head, sizeof head);
            if (!found.empty()) {
                memcpy(file.data() + sizeof head, found.data(), found.size() * sizeof(ps2::VabSample));
            }
            file.insert(file.end(), vag.begin(), vag.end());
            char name[32];
            bool changed = false;

            snprintf(name, sizeof name, "%08x.vag", crc);
            if (!write_changed(dir + "/" + name, file, changed)) {
                fprintf(stderr, "sounds_vag: can't write %s\n", name);
                return 1;
            }
            written[crc] = path;
            banks++;
            samples += static_cast<uint32_t>(found.size());
            total += vag.size();
            changed_count += changed;
            char line[512];

            snprintf(line, sizeof line, "%s,%u,%08x,%u,%u\n", path.substr(path.find_last_of('/') + 1).c_str(), b, crc,
                     static_cast<unsigned>(found.size()), static_cast<unsigned>(vag.size()));
            report += line;
        }
    }
    bool changed = false;

    if (!write_changed(dir + "/report.csv", std::vector<uint8_t>(report.begin(), report.end()), changed)) {
        return 1;
    }
    printf("sounds_vag: %zu MLT files, %u programs banks, %u samples, %.1f MB of VAG, %u rewritten\n", paths.size(),
           banks, samples, static_cast<double>(total) / 1048576.0, changed_count);
    return 0;
}
