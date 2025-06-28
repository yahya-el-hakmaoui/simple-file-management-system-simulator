#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

#define MAX_FILES 128
#define BLOCK_SIZE 512
#define TOTAL_BLOCKS 1024
#define DISK_SIZE (BLOCK_SIZE * TOTAL_BLOCKS)
#define MAX_FILE_SIZE (BLOCK_SIZE * 10)
#define FAT_FREE -2
#define FAT_EOF -1

#define FAT_OFFSET 0
#define FAT_SIZE (sizeof(int) * TOTAL_BLOCKS)
#define ROOT_OFFSET FAT_SIZE
#define FILE_META_SIZE sizeof(File)
#define ROOT_SIZE (MAX_FILES * FILE_META_SIZE)
#define DATA_OFFSET (FAT_SIZE + ROOT_SIZE)

typedef enum { READ = 1, WRITE = 2, EXECUTE = 4 } Permission;

typedef struct File {
    char name[32];
    size_t size;
    time_t created;
    time_t modified;
    int permissions;
    int is_directory;
    int start_block;
    int parent_index;
    int used;
} File;

typedef struct {
    char *disk;
} FileSystem;

FileSystem fs;
File *current_dir = NULL;
int current_parent = -1;
char username[32] = "user";

// ------------------------ UTILITAIRES ------------------------
int *get_fat() {
    return (int *)(fs.disk + FAT_OFFSET);
}

File *get_root() {
    return (File *)(fs.disk + ROOT_OFFSET);
}

int allocate_block() {
    int *fat = get_fat();
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        if (fat[i] == FAT_FREE)
            return i;
    }
    return -1;
}

void free_chain(int block) {
    int *fat = get_fat();
    while (block != FAT_EOF && block != FAT_FREE) {
        int next = fat[block];
        fat[block] = FAT_FREE;
        block = next;
    }
}

void get_current_path(char *buffer, size_t size) {
    File *root = get_root();
    if (current_parent == -1) {
        snprintf(buffer, size, "/");
        return;
    }

    char temp[256] = "";
    int idx = current_parent;
    while (idx != -1) {
        char segment[64];
        snprintf(segment, sizeof(segment), "/%s", root[idx].name);
        memmove(temp + strlen(segment), temp, strlen(temp) + 1);
        memcpy(temp, segment, strlen(segment));
        idx = root[idx].parent_index;
    }
    snprintf(buffer, size, "%s", temp);
}

void print_prompt() {
    char path[256];
    get_current_path(path, sizeof(path));
    printf("\033[32m%s@fs\033[0m:\033[34m%s\033[0m$ ", username, path);
}

void print_help() {
    printf("Available commands:\n");
    printf("  help            - Show this help message\n");
    printf("  touch <name>    - Create a new file\n");
    printf("  mkdir <name>    - Create a new directory\n");
    printf("  ls              - List files in current directory\n");
    printf("  cd <dir>        - Change current directory\n");
    printf("  rm <name>       - Remove a file or directory\n");
    printf("  write <f> <txt> - Write text to file\n");
    printf("  cat <file>      - Display file content\n");
    printf("  exit            - Exit the filesystem\n");
}

// ------------------------ INITIALISATION ------------------------
void fs_init() {
    fs.disk = calloc(1, DISK_SIZE);
    int *fat = get_fat();
    for (int i = 0; i < TOTAL_BLOCKS; i++) fat[i] = FAT_FREE;

    File *root = get_root();
    for (int i = 0; i < MAX_FILES; i++) root[i].used = 0;

    current_dir = root;
    current_parent = -1;
}

// ------------------------ FONCTIONS PRINCIPALES ------------------------
File *fs_find_file(const char *name, int parent) {
    File *root = get_root();
    for (int i = 0; i < MAX_FILES; i++) {
        if (root[i].used && root[i].parent_index == parent && strcmp(root[i].name, name) == 0) {
            return &root[i];
        }
    }
    return NULL;
}

void fs_create_file(const char *name, int is_dir) {
    if (!name) return;
    File *root = get_root();

    for (int i = 0; i < MAX_FILES; i++) {
        if (!root[i].used) {
            if (fs_find_file(name, current_parent)) {
                printf("Error: file or directory already exists\n");
                return;
            }

            File *f = &root[i];
            memset(f, 0, sizeof(File));
            strncpy(f->name, name, sizeof(f->name));
            f->used = 1;
            f->is_directory = is_dir;
            f->size = 0;
            f->permissions = READ | WRITE | EXECUTE;
            f->created = f->modified = time(NULL);
            f->parent_index = current_parent;

            if (!is_dir) {
                int blk = allocate_block();
                if (blk == -1) {
                    printf("Error: no space\n");
                    f->used = 0;
                    return;
                }
                f->start_block = blk;
                get_fat()[blk] = FAT_EOF;
            } else {
                f->start_block = -1;
            }
            return;
        }
    }
    printf("Error: directory full\n");
}

void fs_delete_file_recursive(File *file) {
    File *root = get_root();
    if (file->is_directory) {
        for (int i = 0; i < MAX_FILES; i++) {
            if (root[i].used && root[i].parent_index == (file - root)) {
                fs_delete_file_recursive(&root[i]);
            }
        }
    } else {
        free_chain(file->start_block);
    }
    file->used = 0;
}

void fs_delete_file(const char *name) {
    File *target = fs_find_file(name, current_parent);
    if (!target) {
        printf("Error: file not found\n");
        return;
    }

    if (target->is_directory) {
        int nonempty = 0;
        File *root = get_root();
        for (int i = 0; i < MAX_FILES; i++) {
            if (root[i].used && root[i].parent_index == (target - root)) {
                nonempty = 1;
                break;
            }
        }

        if (nonempty) {
            printf("Directory not empty. Delete? (y/n): ");
            char ans[8]; fgets(ans, sizeof(ans), stdin);
            if (tolower(ans[0]) != 'y') return;
        }
    }
    fs_delete_file_recursive(target);
}

int compare_files(const void *a, const void *b) {
    File *fa = *(File **)a;
    File *fb = *(File **)b;
    return strcmp(fa->name, fb->name);
}

void fs_list() {
    File *root = get_root();
    File *files[MAX_FILES];
    int count = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        File *f = &root[i];
        if (f->used && f->parent_index == current_parent) {
            files[count++] = f;
        }
    }

    qsort(files, count, sizeof(File *), compare_files);

    for (int i = 0; i < count; i++) {
        File *f = files[i];
        char perm[4] = "---";
        if (f->permissions & READ) perm[0] = 'r';
        if (f->permissions & WRITE) perm[1] = 'w';
        if (f->permissions & EXECUTE) perm[2] = 'x';

        char created[32], modified[32];
        strftime(created, sizeof(created), "%Y-%m-%d %H:%M:%S", localtime(&f->created));
        strftime(modified, sizeof(modified), "%Y-%m-%d %H:%M:%S", localtime(&f->modified));

        printf("%c%s\t%lu bytes\tCreated: %s\tModified: %s\t\033[34m%s\033[0m\n",
               f->is_directory ? 'd' : '-', perm, f->size, created, modified, f->name);
    }
}


void fs_change_dir(const char *name) {
    if (!name) return;

    if (strcmp(name, "..") == 0) {
        if (current_parent != -1) {
            File *root = get_root();
            current_parent = root[current_parent].parent_index;
        }
        return;
    }

    File *f = fs_find_file(name, current_parent);
    if (!f || !f->is_directory) {
        printf("Error: directory not found\n");
        return;
    }

    current_parent = f - get_root();
}

void fs_write_file(const char *name, const char *content) {
    File *f = fs_find_file(name, current_parent);
    if (!f || f->is_directory) {
        printf("Error: invalid file\n");
        return;
    }

    free_chain(f->start_block);
    f->start_block = -1;

    size_t len = strlen(content);
    if (len > MAX_FILE_SIZE) len = MAX_FILE_SIZE;
    f->size = len;

    int prev = -1;
    size_t written = 0;

    while (written < len) {
        int blk = allocate_block();
        if (blk == -1) {
            printf("Disk full\n");
            return;
        }

        size_t chunk = (len - written > BLOCK_SIZE) ? BLOCK_SIZE : (len - written);
        memcpy(fs.disk + DATA_OFFSET + blk * BLOCK_SIZE, content + written, chunk);

        if (prev != -1)
            get_fat()[prev] = blk;
        else
            f->start_block = blk;

        get_fat()[blk] = FAT_EOF;
        prev = blk;
        written += chunk;
    }

    f->modified = time(NULL);
}

void fs_read_file(const char *name) {
    File *f = fs_find_file(name, current_parent);
    if (!f || f->is_directory) {
        printf("Error: invalid file\n");
        return;
    }

    int blk = f->start_block;
    size_t left = f->size;
    while (blk != FAT_EOF && left > 0) {
        size_t chunk = left > BLOCK_SIZE ? BLOCK_SIZE : left;
        fwrite(fs.disk + DATA_OFFSET + blk * BLOCK_SIZE, 1, chunk, stdout);
        left -= chunk;
        blk = get_fat()[blk];
    }
    printf("\n");
}

// ------------------------ MAIN ------------------------
int main() {
    fs_init();

    printf("Enter your username: ");
    fgets(username, sizeof(username), stdin);
    username[strcspn(username, "\n")] = 0;
    if (strlen(username) == 0) strcpy(username, "user");

    print_help();
    char line[256];

    while (1) {
        print_prompt();
        if (!fgets(line, sizeof(line), stdin)) break;
        char *cmd = strtok(line, " \n");
        if (!cmd) continue;

        if (strcmp(cmd, "exit") == 0) break;
        else if (strcmp(cmd, "touch") == 0) fs_create_file(strtok(NULL, " \n"), 0);
        else if (strcmp(cmd, "mkdir") == 0) fs_create_file(strtok(NULL, " \n"), 1);
        else if (strcmp(cmd, "ls") == 0) fs_list();
        else if (strcmp(cmd, "cd") == 0) fs_change_dir(strtok(NULL, " \n"));
        else if (strcmp(cmd, "rm") == 0) fs_delete_file(strtok(NULL, " \n"));
        else if (strcmp(cmd, "write") == 0) {
            char *name = strtok(NULL, " \n");
            char *content = strtok(NULL, "\n");
            if (!content) content = "";
            fs_write_file(name, content);
        } else if (strcmp(cmd, "cat") == 0) fs_read_file(strtok(NULL, " \n"));
        else if (strcmp(cmd, "help") == 0) print_help();
        else printf("Command not found. Type 'help' for list of commands.\n");
    }

    free(fs.disk);
    return 0;
}
