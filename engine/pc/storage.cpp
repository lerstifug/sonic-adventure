/* the PC backend's storage: a save is the file its name gives. a request
 * ends in its call, which posts its SA_EVENT_STORAGE before it returns. so
 * the call's result is the request's. a read of a save larger than its
 * buffer gives SA_ERR_FULL, and of a missing file SA_ERR_MISSING. a write
 * makes a new file beside the save, and any missing directory above. once
 * its bytes are on the disk, the new file is renamed over the save. so a
 * write cut short leaves the save as it was */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
/* minwindef.h's empty near and far would erase sa_engine.h's fields */
#undef near
#undef far
#define FLAGS (O_BINARY | O_NOINHERIT)
/* either slash ends a directory's name */
#define SLASHES "/\\"
#define make_directory(path) _mkdir(path)
#define fsync _commit
/* rename fails where the save exists, and MoveFileEx replaces it */
#define take_place(fresh, name)                                                \
    (MoveFileExA(fresh, name,                                                  \
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)           \
         ? 0                                                                   \
         : -1)
#elif defined(_EE)
/* the PS2's devices, PCSX2's host: among them, may take no fsync and no
 * rename over a file that exists. a write there makes its new file as
 * elsewhere, and where the rename fails, the save goes first and the rename
 * goes again, or else the new file's bytes are copied over the save */
#define FLAGS 0
#define SLASHES "/"
#define make_directory(path) mkdir(path, 0755)
#define fsync(fd) 0
#define take_place(fresh, name) placed_over(fresh, name)
#define LACKS_RENAME_OVER 1
#else
/* the PSP runs no second program, so its files have no such flag */
#ifdef O_CLOEXEC
#define FLAGS O_CLOEXEC
#else
#define FLAGS 0
#endif
#define SLASHES "/"
#define make_directory(path) mkdir(path, 0755)
#define take_place(fresh, name) rename(fresh, name)
#endif

#include "runtime.h"

namespace {

#ifdef LACKS_RENAME_OVER
int placed_over(const char *fresh, const char *name)
{
    char block[4096];
    size_t got;
    bool whole = true;
    FILE *in, *out;

    if (rename(fresh, name) == 0) {
        return 0;
    }
    remove(name);
    if (rename(fresh, name) == 0) {
        return 0;
    }
    if (!(in = fopen(fresh, "rb"))) {
        return -1;
    }
    if (!(out = fopen(name, "wb"))) {
        fclose(in);
        return -1;
    }
    while ((got = fread(block, 1, sizeof block, in)) != 0) {
        whole = whole && fwrite(block, 1, got, out) == got;
    }
    whole = !ferror(in) && whole;
    fclose(in);
    whole = fclose(out) == 0 && whole;
    remove(fresh);
    if (!whole) {
        fprintf(stderr, "engine: can't copy %s over %s, errno %d\n", fresh, name, errno);
    }
    return whole ? 0 : -1;
}
#endif

/* whether a failed open of path found no file there. the PS2's devices
 * give that their own errno, so there a file stat can't find is missing */
bool missing(const char *path)
{
#ifdef _EE
    struct stat about;

    return errno == ENOENT || stat(path, &about) != 0;
#else
    (void)path;
    return errno == ENOENT;
#endif
}

/* flush the directory holding name after renaming a save into place. the
 * PS2 writes its save where it lies, with no directory to flush */
void sync_parent(const char *name)
{
#if !defined(_WIN32) && !defined(_EE)
    std::string path(name);
    size_t slash = path.find_last_of(SLASHES);
    std::string dir = slash == std::string::npos
                          ? "."
                          : (slash == 0 ? "/" : path.substr(0, slash));
    int fd = open(dir.c_str(), O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
#else
    (void)name;
#endif
}

/* take a request's handle where its event has room */
sa_result begin(sa_engine &engine, sa_handle &handle)
{
    if (engine.event_count + engine.event_reserved >= engine.desc.events) {
        return SA_ERR_FULL;
    }
    handle = engine.resources.take(SA_KIND_REQUEST);
    return handle != SA_NULL ? SA_OK : SA_ERR_FULL;
}

/* post the request's event in the room begin found. give its handle back */
sa_result end(sa_engine &engine, sa_handle handle, uint32_t bytes,
              sa_handle *request)
{
    sa_event event{};

    sa::event_reserve(engine);
    event.kind = SA_EVENT_STORAGE;
    event.handle = handle;
    event.result = static_cast<int32_t>(bytes);
    event.tick = engine.tick;
    event.time_ns = sa_clock_ns(&engine);
    sa::event_post(engine, event);
    engine.resources.give(handle);
    *request = handle;
    return SA_OK;
}

/* path's file, made to write, and each directory above it where none is */
int made(std::string path)
{
    const int flags = O_WRONLY | O_CREAT | O_EXCL | FLAGS;
    int fd = open(path.c_str(), flags, 0644);

    if (fd < 0 && errno == ENOENT) {
        for (size_t at = path.find_first_of(SLASHES, 1);
             at != std::string::npos;
             at = path.find_first_of(SLASHES, at + 1)) {
            char slash = path[at];

            path[at] = '\0';
            make_directory(path.c_str());
            path[at] = slash;
        }
        fd = open(path.c_str(), flags, 0644);
    }
    return fd;
}

bool written(int fd, const uint8_t *data, uint32_t bytes)
{
    while (bytes != 0) {
        ssize_t wrote = write(fd, data, bytes);

        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
        data += wrote;
        bytes -= static_cast<uint32_t>(wrote);
    }
    return true;
}

} // namespace

sa_result sa_storage_read(sa_engine *engine, const char *name, void *buffer,
                          uint32_t bytes, sa_handle *request)
{
    sa_handle handle = SA_NULL;
    struct stat about;
    FILE *in;
    sa_result result;
    bool whole;

    if (engine == nullptr || name == nullptr || request == nullptr ||
        (buffer == nullptr && bytes != 0) ||
        bytes > static_cast<uint32_t>(INT32_MAX)) {
        return SA_ERR_INVALID;
    }
    if (!(in = fopen(name, "rb"))) {
        return missing(name) ? SA_ERR_MISSING : SA_ERR_IO;
    }
    if (fstat(fileno(in), &about) != 0) {
        fclose(in);
        return SA_ERR_IO;
    }
    if (about.st_size < 0 || static_cast<uint64_t>(about.st_size) > bytes) {
        fclose(in);
        return SA_ERR_FULL;
    }
    if ((result = begin(*engine, handle)) != SA_OK) {
        fclose(in);
        return result;
    }
    whole = fread(buffer, 1, static_cast<size_t>(about.st_size), in) ==
            static_cast<size_t>(about.st_size);
    fclose(in);
    if (!whole) {
        engine->resources.give(handle);
        return SA_ERR_IO;
    }
    return end(*engine, handle, static_cast<uint32_t>(about.st_size), request);
}

sa_result sa_storage_write(sa_engine *engine, const char *name,
                           const void *data, uint32_t bytes, sa_handle *request)
{
    sa_handle handle = SA_NULL;
    std::string fresh;
    sa_result result;
    int fd;
    bool done;

    if (engine == nullptr || name == nullptr || request == nullptr ||
        (data == nullptr && bytes != 0) ||
        bytes > static_cast<uint32_t>(INT32_MAX)) {
        return SA_ERR_INVALID;
    }
    if ((result = begin(*engine, handle)) != SA_OK) {
        return result;
    }
    fresh = std::string(name) + ".new";
#ifdef _EE
    /* the PS2's devices take the C library's files, as its log writes, and
     * PCSX2's host: takes no rename over a file there: a save is written
     * where it lies. a write cut short there leaves it part written, which
     * the card's checks then refuse (engine/native/card.c). the PS2's own
     * memory card, by its library, is to take the save's place */
    (void)fd;
    (void)fresh;
    {
        FILE *out = fopen(name, "wb");

        done = out != nullptr && fwrite(data, 1, bytes, out) == bytes;
        if (out != nullptr) {
            done = fclose(out) == 0 && done;
        }
        if (!done) {
            fprintf(stderr, "engine: can't write %s: %s, errno %d\n", name,
                    out == nullptr ? "the open failed" : "the write failed", errno);
            engine->resources.give(handle);
            return SA_ERR_IO;
        }
        return end(*engine, handle, bytes, request);
    }
#else
    /* clear any stale temp file left by an interrupted save before O_EXCL */
    (void)unlink(fresh.c_str());
    fd = made(fresh);
    done = fd >= 0 && written(fd, static_cast<const uint8_t *>(data), bytes) &&
           fsync(fd) == 0;
    if (fd >= 0) {
        done = close(fd) == 0 && done;
    }
#endif
    if (!done || take_place(fresh.c_str(), name) != 0) {
        unlink(fresh.c_str());
        engine->resources.give(handle);
        return SA_ERR_IO;
    }
    sync_parent(name);
    return end(*engine, handle, bytes, request);
}
