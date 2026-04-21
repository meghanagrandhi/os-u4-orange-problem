// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".")    == 0) continue;
            if (strcmp(ent->d_name, "..")   == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes")  == 0) continue;
            if (strstr(ent->d_name, ".o")   != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1;
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index.
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    index->count = 0;
    FILE *f = fopen(".pes/index", "r");
    if (!f) return 0;  // No index yet — not an error

    char hex[HASH_HEX_SIZE + 1];
    unsigned int mode;
    uint64_t mtime, size;
    char path[512];

    while (fscanf(f, "%o %64s %" SCNu64 " %" SCNu64 " %511s",
                  &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) break;
        IndexEntry *e = &index->entries[index->count++];
        e->mode      = mode;
        e->mtime_sec = mtime;
        e->size      = size;
        snprintf(e->path, sizeof(e->path), "%s", path);
        hex_to_hash(hex, &e->hash);
    }
    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically.
// Returns 0 on success, -1 on error.
static int compare_index_entries(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

int index_save(const Index *index) {
    // Allocate sorted copy on heap to avoid stack overflow (Index is large)
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    *sorted = *index;
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_index_entries);

    const char *tmp = ".pes/index.tmp";
    FILE *f = fopen(tmp, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < sorted->count; i++) {
        IndexEntry *e = &sorted->entries[i];
        hash_to_hex(&e->hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %" PRIu64 " %s\n",
                e->mode, hex,
                (uint64_t)e->mtime_sec,
                (uint64_t)e->size,
                e->path);
    }
    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);
    return rename(tmp, ".pes/index");
}

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Stage a file for the next commit.
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    // Read file contents
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);
    if (file_size < 0) { fclose(f); return -1; }

    // Handle empty files safely
    void *buf = NULL;
    if (file_size > 0) {
        buf = malloc((size_t)file_size);
        if (!buf) { fclose(f); return -1; }
        if ((long)fread(buf, 1, (size_t)file_size, f) != file_size) {
            free(buf); fclose(f); return -1;
        }
    }
    fclose(f);

    // Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, buf ? buf : "", (size_t)file_size, &blob_id) != 0) {
        free(buf);
        return -1;
    }
    free(buf);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    // Update existing entry or insert new one
    IndexEntry *existing = index_find(index, path);
    if (!existing) {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        existing = &index->entries[index->count++];
    }

    existing->mode      = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;
    existing->hash      = blob_id;
    existing->mtime_sec = (uint64_t)st.st_mtime;
    existing->size      = (uint64_t)st.st_size;
    snprintf(existing->path, sizeof(existing->path), "%s", path);

    return index_save(index);
}
