// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions:     tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"
//
// Example single entry (conceptual):
//   "100644 hello.txt\0" followed by 32 raw bytes of SHA-256

#include "tree.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// Forward declaration (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── Mode Constants ─────────────────────────────────────────────────────────

#define MODE_FILE      0100644
#define MODE_EXEC      0100755
#define MODE_DIR       0040000

// ─── PROVIDED ───────────────────────────────────────────────────────────────

// Determine the object mode for a filesystem path.
uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;

    if (S_ISDIR(st.st_mode))  return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

// Parse binary tree data into a Tree struct safely.
// Returns 0 on success, -1 on parse error.
int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        // 1. Safely find the space character for the mode
        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1; // Malformed data

        // Parse mode into an isolated buffer
        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = strtol(mode_str, NULL, 8);

        ptr = space + 1; // Skip space

        // 2. Safely find the null terminator for the name
        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1; // Malformed data

        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0'; // Ensure null-terminated

        ptr = null_byte + 1; // Skip null byte

        // 3. Read the 32-byte binary hash
        if (ptr + HASH_SIZE > end) return -1; 
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

// Helper for qsort to ensure consistent tree hashing
static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

typedef struct {
    uint32_t mode;
    ObjectID hash;
    char path[512];
} TreeIndexEntry;

typedef struct {
    TreeIndexEntry entries[MAX_TREE_ENTRIES];
    int count;
} TreeIndexSnapshot;

static int tree_has_entry(const Tree *tree, const char *name) {
    for (int i = 0; i < tree->count; i++) {
        if (strcmp(tree->entries[i].name, name) == 0) return 1;
    }
    return 0;
}

static int load_tree_index(TreeIndexSnapshot *snapshot) {
    snapshot->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return -1;

    while (snapshot->count < MAX_TREE_ENTRIES) {
        TreeIndexEntry *entry = &snapshot->entries[snapshot->count];
        char hex[HASH_HEX_SIZE + 1];
        unsigned long long mtime_sec;
        unsigned int size;

        int fields = fscanf(f, "%o %64s %llu %u %511[^\n]\n",
                            &entry->mode, hex, &mtime_sec, &size, entry->path);
        if (fields == EOF) break;
        if (fields != 5 || hex_to_hash(hex, &entry->hash) != 0) {
            fclose(f);
            return -1;
        }

        snapshot->count++;
        (void)mtime_sec;
        (void)size;
    }

    fclose(f);
    return 0;
}

static int build_tree_level(const TreeIndexSnapshot *index, const char *prefix, ObjectID *id_out) {
    Tree tree = {0};
    size_t prefix_len = strlen(prefix);

    for (int i = 0; i < index->count; i++) {
        const TreeIndexEntry *entry = &index->entries[i];
        if (strncmp(entry->path, prefix, prefix_len) != 0) continue;

        const char *relative = entry->path + prefix_len;
        if (*relative == '\0') continue;

        const char *slash = strchr(relative, '/');

        if (!slash) {
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *tree_entry = &tree.entries[tree.count];
            tree_entry->mode = entry->mode;
            tree_entry->hash = entry->hash;
            snprintf(tree_entry->name, sizeof(tree_entry->name), "%s", relative);
            tree.count++;
            continue;
        }

        size_t name_len = (size_t)(slash - relative);
        if (name_len == 0 || name_len >= sizeof(((TreeEntry *)0)->name)) return -1;

        char dir_name[256];
        memcpy(dir_name, relative, name_len);
        dir_name[name_len] = '\0';
        if (tree_has_entry(&tree, dir_name)) continue;

        if (tree.count >= MAX_TREE_ENTRIES) return -1;
        TreeEntry *tree_entry = &tree.entries[tree.count];

        char child_prefix[1024];
        snprintf(child_prefix, sizeof(child_prefix), "%s%s/", prefix, dir_name);

        tree_entry->mode = MODE_DIR;
        snprintf(tree_entry->name, sizeof(tree_entry->name), "%s", dir_name);
        if (build_tree_level(index, child_prefix, &tree_entry->hash) != 0) return -1;
        tree.count++;
    }

    void *raw_tree = NULL;
    size_t raw_len = 0;
    if (tree_serialize(&tree, &raw_tree, &raw_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, raw_tree, raw_len, id_out);
    free(raw_tree);
    return rc;
}

// Serialize a Tree struct into binary format for storage.
// Caller must free(*data_out).
// Returns 0 on success, -1 on error.
int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    // Estimate max size: (6 bytes mode + 1 byte space + 256 bytes name + 1 byte null + 32 bytes hash) per entry
    size_t max_size = tree->count * 296; 
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    // Create a mutable copy to sort entries (Git requirement)
    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, sorted_tree.count, sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        
        // Write mode and name (%o writes octal correctly for Git standards)
        int written = sprintf((char *)buffer + offset, "%o %s", entry->mode, entry->name);
        offset += written + 1; // +1 to step over the null terminator written by sprintf
        
        // Write binary hash
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out = offset;
    return 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Build a tree hierarchy from the current index and write all tree
// objects to the object store.
//
// HINTS - Useful functions and concepts for this phase:
//   - index_load      : load the staged files into memory
//   - strchr          : find the first '/' in a path to separate directories from files
//   - strncmp         : compare prefixes to group files belonging to the same subdirectory
//   - Recursion       : you will likely want to create a recursive helper function 
//                       (e.g., `write_tree_level(entries, count, depth)`) to handle nested dirs.
//   - tree_serialize  : convert your populated Tree struct into a binary buffer
//   - object_write    : save that binary buffer to the store as OBJ_TREE
//
// Returns 0 on success, -1 on error.
int tree_from_index(ObjectID *id_out) {
    TreeIndexSnapshot index;
    if (load_tree_index(&index) != 0) return -1;
    return build_tree_level(&index, "", id_out);
}
