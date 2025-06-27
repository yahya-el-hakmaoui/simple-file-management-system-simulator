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

typedef enum { READ = 1, WRITE = 2, EXECUTE = 4 } Permission;

typedef struct File File;
typedef struct Directory Directory;

struct File {
    char name[32];
    size_t size;
    time_t created;
    time_t modified;
    int permissions;
    int is_directory;
    int start_block; // Premier bloc dans FAT
    Directory *dir;
    File *parent;
};

struct Directory {
    File files[MAX_FILES];
    size_t file_count;
};

typedef struct {
    Directory root;
    Directory *current_dir;
    int *FAT;
    char *disk;
} FileSystem;

FileSystem fs;
File *current_dir_file = NULL;
char username[32] = "user";

// ========================== UTILITAIRES ==========================
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

void fs_init() {
    fs.disk = calloc(1, DISK_SIZE);
    fs.FAT = malloc(sizeof(int) * TOTAL_BLOCKS);
    for (int i = 0; i < TOTAL_BLOCKS; i++) fs.FAT[i] = FAT_FREE;
    fs.root.file_count = 0;
    fs.current_dir = &fs.root;
    current_dir_file = NULL;
}

int allocate_block() {
    for (int i = 0; i < TOTAL_BLOCKS; i++) {
        if (fs.FAT[i] == FAT_FREE) return i;
    }
    return -1;
}

void free_chain(int block) {
    while (block != FAT_EOF && block != FAT_FREE) {
        int next = fs.FAT[block];
        fs.FAT[block] = FAT_FREE;
        block = next;
    }
}

void get_current_path(char *buffer, size_t size, Directory *dir, const char *current_name) {
    if (dir == &fs.root || current_dir_file == NULL) {
        snprintf(buffer, size, "/");
        return;
    }

    char temp[256] = "";
    File *walker = current_dir_file;
    while (walker) {
        char segment[64];
        snprintf(segment, sizeof(segment), "/%s", walker->name);
        memmove(temp + strlen(segment), temp, strlen(temp) + 1);
        memcpy(temp, segment, strlen(segment));
        walker = walker->parent;
    }
    snprintf(buffer, size, "%s", temp);
}

void print_prompt() {
    char path[256];
    get_current_path(path, sizeof(path), fs.current_dir, "");
    printf("\033[32m%s@fs\033[0m:\033[34m%s\033[0m$ ", username, path);
}

File *fs_find_file(Directory *dir, const char *name) {
    for (size_t i = 0; i < dir->file_count; i++) {
        if (strcmp(dir->files[i].name, name) == 0) {
            return &dir->files[i];
        }
    }
    return NULL;
}

// ========================== FONCTIONS FS ==========================
void fs_create_file(const char *name, int is_dir) {
    if (!name) return;
    if (fs.current_dir->file_count >= MAX_FILES) {
        printf("Error: directory full\n");
        return;
    }
    if (fs_find_file(fs.current_dir, name)) {
        printf("Error: file or directory already exists\n");
        return;
    }

    File *file = &fs.current_dir->files[fs.current_dir->file_count++];
    strncpy(file->name, name, sizeof(file->name));
    file->size = 0;
    file->created = time(NULL);
    file->modified = time(NULL);
    file->permissions = READ | WRITE;
    file->is_directory = is_dir;
    file->parent = current_dir_file;

    if (is_dir) {
        file->dir = malloc(sizeof(Directory));
        file->dir->file_count = 0;
        file->start_block = -1;
    } else {
        int blk = allocate_block();
        if (blk == -1) {
            printf("Error: no free space\n");
            fs.current_dir->file_count--;
            return;
        }
        fs.FAT[blk] = FAT_EOF;
        file->start_block = blk;
        file->dir = NULL;
    }
}

void fs_delete_file_recursive(File *file) {
    if (file->is_directory) {
        for (size_t i = 0; i < file->dir->file_count; i++) {
            fs_delete_file_recursive(&file->dir->files[i]);
        }
        free(file->dir);
    } else {
        free_chain(file->start_block);
    }
}

void fs_delete_file(const char *name) {
    for (size_t i = 0; i < fs.current_dir->file_count; i++) {
        File *file = &fs.current_dir->files[i];
        if (strcmp(file->name, name) == 0) {
            if (file->is_directory && file->dir->file_count > 0) {
                printf("Directory not empty. Delete? (y/n): ");
                char ans[8]; fgets(ans, sizeof(ans), stdin);
                if (tolower(ans[0]) != 'y') return;
            }
            fs_delete_file_recursive(file);
            for (size_t j = i; j < fs.current_dir->file_count - 1; j++)
                fs.current_dir->files[j] = fs.current_dir->files[j + 1];
            fs.current_dir->file_count--;
            return;
        }
    }
    printf("Error: file not found\n");
}

void fs_write_file(const char *name, const char *content) {
    File *file = fs_find_file(fs.current_dir, name);
    if (!file || file->is_directory) {
        printf("Error: invalid file\n");
        return;
    }

    free_chain(file->start_block);
    file->start_block = -1;

    size_t len = strlen(content);
    if (len > MAX_FILE_SIZE) len = MAX_FILE_SIZE;
    file->size = len;

    int prev_block = -1;
    size_t written = 0;

    while (written < len) {
        int blk = allocate_block();
        if (blk == -1) {
            printf("Disk full\n");
            return;
        }
        size_t chunk = (len - written > BLOCK_SIZE) ? BLOCK_SIZE : (len - written);
        memcpy(fs.disk + blk * BLOCK_SIZE, content + written, chunk);

        if (prev_block != -1)
            fs.FAT[prev_block] = blk;
        else
            file->start_block = blk;

        fs.FAT[blk] = FAT_EOF;
        prev_block = blk;
        written += chunk;
    }

    file->modified = time(NULL);
}

void fs_read_file(const char *name) {
    File *file = fs_find_file(fs.current_dir, name);
    if (!file || file->is_directory) {
        printf("Error: invalid file\n");
        return;
    }

    int blk = file->start_block;
    size_t left = file->size;

    while (blk != FAT_EOF && left > 0) {
        size_t chunk = left > BLOCK_SIZE ? BLOCK_SIZE : left;
        fwrite(fs.disk + blk * BLOCK_SIZE, 1, chunk, stdout);
        left -= chunk;
        blk = fs.FAT[blk];
    }
    printf("\n");
}

int compare_files(const void *a, const void *b) {
    const File *fa = (const File *)a;
    const File *fb = (const File *)b;
    return strcmp(fa->name, fb->name);
}

void fs_list() {
    if (fs.current_dir->file_count == 0) return;

    File sorted[MAX_FILES];
    memcpy(sorted, fs.current_dir->files, sizeof(File) * fs.current_dir->file_count);
    qsort(sorted, fs.current_dir->file_count, sizeof(File), compare_files);

    for (size_t i = 0; i < fs.current_dir->file_count; i++) {
        File *f = &sorted[i];
        char perm[4] = "---";
        if (f->permissions & READ) perm[0] = 'r';
        if (f->permissions & WRITE) perm[1] = 'w';
        if (f->permissions & EXECUTE) perm[2] = 'x';

        printf("%c%s\t%lu bytes\t%s\n",
               f->is_directory ? 'd' : '-', perm, f->size, f->name);
    }
}

void fs_change_dir(const char *name) {
    if (!name) return;
    if (strcmp(name, "..") == 0) {
        if (current_dir_file && current_dir_file->parent) {
            fs.current_dir = current_dir_file->parent->dir;
            current_dir_file = current_dir_file->parent;
        } else {
            fs.current_dir = &fs.root;
            current_dir_file = NULL;
        }
        return;
    }
    File *f = fs_find_file(fs.current_dir, name);
    if (!f || !f->is_directory) {
        printf("Error: directory not found\n");
        return;
    }
    fs.current_dir = f->dir;
    current_dir_file = f;
}

// ========================== MAIN LOOP ==========================
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
        }
        else if (strcmp(cmd, "cat") == 0) fs_read_file(strtok(NULL, " \n"));
        else if (strcmp(cmd, "help") == 0) print_help();
        else printf("Command not found. Type 'help' for list of commands.\n");
    }

    free(fs.FAT);
    free(fs.disk);
    return 0;
}
