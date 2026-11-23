/* read the executable's own symbols into names.h's list */
#ifdef _WIN32
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#else
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "names.h"
#include "native.h"

#ifdef _WIN32
/* the Windows image's own symbols, from the COFF table the link keeps. a
 * name's address is the image's base, its section's address and its value in
 * the section. COFF gives no size, so each name stands for 0 and names.h
 * stretches it to the next address. a section's code flag says whether a
 * name is a function. the image stays allocated, as each long name stays in
 * it */
#define COFF_SYMBOL 18
#define COFF_FILE 103 /* IMAGE_SYM_CLASS_FILE: the aux records hold the name */
#define COFF_SECTION_CODE 0x20u

static uint32_t le16(const uint8_t *at)
{
    return (uint32_t)at[0] | (uint32_t)at[1] << 8;
}

static uint32_t le32(const uint8_t *at)
{
    return le16(at) | le16(at + 2) << 16;
}

/* whether bytes from offset on lie in a file of size bytes */
static int within(uint64_t offset, uint64_t bytes, uint64_t size)
{
    return offset <= size && bytes <= size - offset;
}

/* the last component of a path, as a file record may give one */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *back = strrchr(path, '\\');

    if (back && (!slash || back > slash)) {
        slash = back;
    }
    return slash ? slash + 1 : path;
}

static uint8_t *read_whole(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    uint8_t *image = NULL;
    long length;

    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 &&
        fseek(file, 0, SEEK_SET) == 0 && (image = malloc((size_t)length))) {
        if (fread(image, 1, (size_t)length, file) == (size_t)length) {
            *size = (size_t)length;
        } else {
            free(image);
            image = NULL;
        }
    }
    fclose(file);
    return image;
}

/* add the symbols of the PE file at path. the result is 0 where it adds
 * them. a file that can't be read gives -1, and one with no symbol table 1.
 * the first pass adds the objects that follow task.c's file record, and the
 * second the rest, as the ELF reader does */
static int read_file(const char *path)
{
    size_t size = 0;
    uint8_t *image = read_whole(path, &size);
    uint32_t pe, count, table, strings, optional, section_count;
    uint64_t base;
    const uint8_t *sections;

    if (!image) {
        return -1;
    }
    if (size < 0x40 || image[0] != 'M' || image[1] != 'Z') {
        free(image);
        return 1;
    }
    pe = le32(image + 0x3c);
    if (!within(pe, 24, size) || memcmp(image + pe, "PE\0\0", 4)) {
        free(image);
        return 1;
    }
    section_count = le16(image + pe + 6);
    table = le32(image + pe + 12);
    count = le32(image + pe + 16);
    optional = le16(image + pe + 20);
    /* PE32+: the image base is the eight bytes at 24 of the optional header */
    if (optional < 32 || !within(pe + 24, optional, size) ||
        le16(image + pe + 24) != 0x20b) {
        free(image);
        return 1;
    }
    base = (uint64_t)le32(image + pe + 24 + 24) |
           (uint64_t)le32(image + pe + 24 + 28) << 32;
    sections = image + pe + 24 + optional;
    strings = table + count * COFF_SYMBOL;
    if (!table || !count ||
        !within(pe + 24 + optional, section_count * 40ull, size) ||
        !within(table, (uint64_t)count * COFF_SYMBOL + 4, size)) {
        free(image);
        return 1;
    }
    for (int pass = 0; pass < 2; pass++) {
        int in_task_c = 0;

        for (uint32_t i = 0; i < count;) {
            const uint8_t *record = image + table + (size_t)i * COFF_SYMBOL;
            uint32_t aux = record[17];
            int section = (int16_t)le16(record + 12);
            int storage = record[16];

            if (!within(table, (uint64_t)(i + 1 + aux) * COFF_SYMBOL, size)) {
                break;
            }
            if (storage == COFF_FILE) {
                /* the file's name fills the aux records, NUL padded */
                char file[COFF_SYMBOL * 4 + 1] = "";

                memcpy(file, record + COFF_SYMBOL,
                       aux > 4 ? 4 * COFF_SYMBOL : aux * COFF_SYMBOL);
                in_task_c = !strcmp(base_name(file), "task.c");
            } else if (section > 0 && (uint32_t)section <= section_count &&
                       (storage == 2 || storage == 3) && record[0] != '.') {
                const uint8_t *home = sections + (size_t)(section - 1) * 40;
                int code = (le32(home + 36) & COFF_SECTION_CODE) != 0;
                int task_static = in_task_c && !code;
                const char *text = NULL;

                if (!le32(record)) {
                    /* a long name lies in the string table, and must end
                     * within the file */
                    uint64_t at = (uint64_t)strings + le32(record + 4);

                    if (within(at, 1, size) &&
                        memchr(image + at, 0, size - (size_t)at)) {
                        text = (const char *)image + at;
                    }
                } else if (task_static == !pass) {
                    char short_name[9];

                    memcpy(short_name, record, 8);
                    short_name[8] = 0;
                    text = strdup(short_name);
                }
                if (text && task_static == !pass) {
                    names_symbol_add((uint32_t)(base + le32(home + 12) +
                                                le32(record + 8)),
                                     0, code, text);
                }
            }
            i += 1 + aux;
        }
    }
    names_symbols_ready();
    return 0;
}

/* a read that fails leaves the symbols unread, so the next call reads
 * again */
int native_read_symbols(void)
{
    static int done;
    char path[MAX_PATH];
    int failed = 0;

    if (!done) {
        DWORD length = GetModuleFileNameA(NULL, path, sizeof path);

        failed = length && length < sizeof path ? read_file(path) : -1;
    }
    done = !failed;
    return failed;
}
#else
/* a symbol's type sits in the same bits in either class */
#define ST_TYPE(info) ((info) & 0xf)

/* whether bytes from offset on lie in a file of size bytes */
static int within(uint64_t offset, uint64_t bytes, uint64_t size)
{
    return offset <= size && bytes <= size - offset;
}

/* section i, or null unless it is a symbol table that lies in the image
 * with its strings. the image must hold its section headers. a string table
 * must end in a NUL, or a name could run past the image */
static const ElfW(Shdr) * symbol_table(const uint8_t *image, size_t size, int i)
{
    const ElfW(Ehdr) *elf = (const ElfW(Ehdr) *)image;
    const ElfW(Shdr) *sections = (const ElfW(Shdr) *)(image + elf->e_shoff);
    const ElfW(Shdr) * names;

    if (sections[i].sh_type != SHT_SYMTAB ||
        sections[i].sh_link >= elf->e_shnum) {
        return NULL;
    }
    names = &sections[sections[i].sh_link];
    return within(sections[i].sh_offset, sections[i].sh_size, size) &&
                   within(names->sh_offset, names->sh_size, size) &&
                   names->sh_size &&
                   !image[names->sh_offset + names->sh_size - 1]
               ? &sections[i]
               : NULL;
}

/* add the symbols of the ELF file at path. the result is 0 where it adds
 * them. a file that can't be read gives -1, and one with no symbol table 1.
 * neither adds a name. names_symbol answers a name's first object added,
 * and more than one source has a task_levels. so the first pass adds the
 * objects that follow task.c's STT_FILE, and the second the rest */
static int read_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    struct stat st;
    int sized = fd >= 0 && fstat(fd, &st) == 0;
    size_t size = sized ? (size_t)st.st_size : 0;
    const uint8_t *image = MAP_FAILED;
    const ElfW(Ehdr) * elf;
    const ElfW(Shdr) * sections;
    int tables = 0;

    /* mmap refuses a length of 0. an empty file reads as one with no table */
    if (size) {
        image = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    }
    if (fd >= 0) {
        close(fd);
    }
    if (!sized || (size && image == MAP_FAILED)) {
        return -1;
    }
    elf = (const ElfW(Ehdr) *)image;
    if (size >= sizeof *elf &&
        within(elf->e_shoff, (uint64_t)elf->e_shnum * sizeof *sections, size)) {
        for (int i = 0; i < elf->e_shnum; i++) {
            tables += symbol_table(image, size, i) != NULL;
        }
    }
    if (!tables) {
        if (size) {
            munmap((void *)image, size);
        }
        return 1;
    }
    /* each name stays in the image, which stays mapped */
    sections = (const ElfW(Shdr) *)(image + elf->e_shoff);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < elf->e_shnum; i++) {
            const ElfW(Shdr) *table = symbol_table(image, size, i);
            const ElfW(Shdr) * strings;
            const ElfW(Sym) * symbols;
            const char *names;
            size_t count;
            int in_task_c = 0;

            if (!table) {
                continue;
            }
            strings = &sections[table->sh_link];
            symbols = (const ElfW(Sym) *)(image + table->sh_offset);
            names = (const char *)(image + strings->sh_offset);
            count = table->sh_size / sizeof(ElfW(Sym));
            for (size_t j = 0; j < count; j++) {
                const char *name = names + symbols[j].st_name;
                int type = ST_TYPE(symbols[j].st_info);
                int task_static = in_task_c && type == STT_OBJECT;

                if (symbols[j].st_name >= strings->sh_size) {
                    continue;
                }
                if (type == STT_FILE) {
                    in_task_c = !strcmp(name, "task.c");
                } else if ((type == STT_FUNC || type == STT_OBJECT) &&
                           symbols[j].st_value && task_static == !pass) {
                    names_symbol_add((uint32_t)symbols[j].st_value,
                                     (uint32_t)symbols[j].st_size,
                                     type == STT_FUNC, name);
                }
            }
        }
    }
    names_symbols_ready();
    return 0;
}

/* a read that fails leaves the symbols unread, so the next call reads
 * again */
int native_read_symbols(void)
{
    static int done;
    int failed = done ? 0 : read_file("/proc/self/exe");

    done = !failed;
    return failed;
}
#endif

uint32_t native_name_id(void *function)
{
    return names_name_id((uint32_t)(uintptr_t)function);
}

char *native_symbol(const char *name)
{
    native_read_symbols();
    return native_widen(names_symbol(name));
}
