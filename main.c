#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>  // pour tolower()
#include "bitmap.h"

#define MAX_FILES 128
#define BLOCK_SIZE 512
#define DISK_SIZE (BLOCK_SIZE * 1024) // 512 KB
#define MAX_FILE_SIZE (BLOCK_SIZE * 10)

#define METADATA_BLOCKS 10
#define METADATA_SIZE (BLOCK_SIZE * METADATA_BLOCKS)

char username[32] = "user";

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
    size_t start_block;
    int dir_index;    // Index de Directory dans la table, -1 si pas un dir
    int parent_index; // Index du parent File dans la table, -1 si racine
};

struct Directory {
    int file_indices[MAX_FILES];  // indices des fichiers dans la table files
    size_t file_count;
};

typedef struct {
    File files[MAX_FILES];
    Directory directories[MAX_FILES]; // on associe un directory à chaque dossier
    size_t file_count;    // nombre total de fichiers/directories dans le FS
} Metadata;

typedef struct {
    Metadata metadata;
    Directory *current_dir;
    int current_dir_index; // index dans metadata.files
    Bitmap *bitmap;
    char *disk;
} FileSystem;

FileSystem fs;

// --- Fonctions pour manipuler metadata sur disque ---

// La zone METADATA est au début du disque fs.disk

void save_metadata() {
    memcpy(fs.disk, &fs.metadata, sizeof(Metadata));
}

void load_metadata() {
    memcpy(&fs.metadata, fs.disk, sizeof(Metadata));
    // Après chargement, mettre current_dir sur root (index 0)
    fs.current_dir_index = 0;
    fs.current_dir = &fs.metadata.directories[fs.metadata.files[0].dir_index];
}

// Retourne un pointeur vers File selon index
File *get_file(int index) {
    if (index < 0 || index >= (int)fs.metadata.file_count) return NULL;
    return &fs.metadata.files[index];
}

// Retourne pointeur vers Directory selon index
Directory *get_directory(int dir_index) {
    if (dir_index < 0 || dir_index >= MAX_FILES) return NULL;
    return &fs.metadata.directories[dir_index];
}

// Trouve un fichier dans un répertoire par nom, retourne index dans metadata.files ou -1
int find_file_in_dir(Directory *dir, const char *name) {
    for (size_t i = 0; i < dir->file_count; i++) {
        int fi = dir->file_indices[i];
        if (strcmp(fs.metadata.files[fi].name, name) == 0) {
            return fi;
        }
    }
    return -1;
}

// Ajoute fichier à directory (par index fichier)
int add_file_to_dir(Directory *dir, int file_index) {
    if (dir->file_count >= MAX_FILES) return 0;
    dir->file_indices[dir->file_count++] = file_index;
    return 1;
}

// Supprime fichier d'un directory (index fichier)
int remove_file_from_dir(Directory *dir, int file_index) {
    size_t i;
    for (i = 0; i < dir->file_count; i++) {
        if (dir->file_indices[i] == file_index) {
            break;
        }
    }
    if (i == dir->file_count) return 0;
    for (; i + 1 < dir->file_count; i++) {
        dir->file_indices[i] = dir->file_indices[i + 1];
    }
    dir->file_count--;
    return 1;
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

void fs_init() {
    fs.disk = calloc(1, DISK_SIZE);
    fs.bitmap = bitmap_create(DISK_SIZE / BLOCK_SIZE);
    memset(&fs.metadata, 0, sizeof(Metadata));

    // Création root file et directory
    File *root_file = &fs.metadata.files[0];
    Directory *root_dir = &fs.metadata.directories[0];

    strncpy(root_file->name, "/", sizeof(root_file->name));
    root_file->size = 0;
    root_file->created = root_file->modified = time(NULL);
    root_file->permissions = READ | WRITE | EXECUTE;
    root_file->is_directory = 1;
    root_file->start_block = 0;
    root_file->dir_index = 0;      // root directory index
    root_file->parent_index = -1;  // pas de parent

    root_dir->file_count = 0;

    fs.metadata.file_count = 1;

    fs.current_dir = root_dir;
    fs.current_dir_index = 0;

    save_metadata();
}

// Récupère le chemin actuel
void get_current_path(char *buffer, size_t size) {
    if (fs.current_dir_index == 0) {
        snprintf(buffer, size, "/");
        return;
    }

    char temp[256] = "";
    int idx = fs.current_dir_index;
    while (idx >= 0) {
        File *f = get_file(idx);
        char segment[64];
        snprintf(segment, sizeof(segment), "/%s", f->name);

        memmove(temp + strlen(segment), temp, strlen(temp) + 1);
        memcpy(temp, segment, strlen(segment));
        idx = f->parent_index;
    }
    snprintf(buffer, size, "%s", temp);
}

void print_prompt() {
    char path[256];
    get_current_path(path, sizeof(path));
    printf("\033[32m%s@fs\033[0m:\033[34m%s\033[0m$ ", username, path);
}

int find_free_block() {
    for (size_t i = 0; i < fs.bitmap->size_in_bits; i++) {
        if (!bitmap_get(fs.bitmap, i)) {
            return i;
        }
    }
    return -1;
}

// Trouve un index libre dans metadata.files pour créer un fichier/dossier
int find_free_file_index() {
    if (fs.metadata.file_count >= MAX_FILES) return -1;
    return fs.metadata.file_count;
}

void fs_create_file(const char *name, int is_dir) {
    if (!name || strlen(name) == 0) {
        printf("Error: no name specified\n");
        return;
    }

    if (fs.current_dir->file_count >= MAX_FILES) {
        printf("Error: directory full\n");
        return;
    }

    if (find_file_in_dir(fs.current_dir, name) != -1) {
        printf("Error: file or directory already exists\n");
        return;
    }

    int new_file_idx = find_free_file_index();
    if (new_file_idx == -1) {
        printf("Error: max files reached\n");
        return;
    }

    File *file = &fs.metadata.files[new_file_idx];
    Directory *dir = NULL;

    memset(file, 0, sizeof(File));
    strncpy(file->name, name, sizeof(file->name));
    file->size = 0;
    file->created = time(NULL);
    file->modified = time(NULL);
    file->permissions = READ | WRITE | EXECUTE;
    file->is_directory = is_dir;
    file->parent_index = fs.current_dir_index;

    if (is_dir) {
        // Créer un directory
        // Chercher index libre dans directories
        int dir_index = -1;
        for (int i = 0; i < MAX_FILES; i++) {
            int used = 0;
            for (size_t j = 0; j < fs.metadata.file_count; j++) {
                if (fs.metadata.files[j].dir_index == i) {
                    used = 1;
                    break;
                }
            }
            if (!used) {
                dir_index = i;
                break;
            }
        }
        if (dir_index == -1) {
            printf("Error: no free directory slots\n");
            return;
        }
        dir = &fs.metadata.directories[dir_index];
        dir->file_count = 0;
        file->dir_index = dir_index;
        file->start_block = 0; // pas utilisé pour dossier
    } else {
        // Trouver bloc libre pour fichier
        int block = find_free_block();
        if (block == -1) {
            printf("Error: no free space\n");
            return;
        }
        bitmap_set(fs.bitmap, block);
        file->start_block = block;
        file->dir_index = -1;
    }

    // Ajouter le fichier à la current directory
    if (!add_file_to_dir(fs.current_dir, new_file_idx)) {
        printf("Error: cannot add file to directory\n");
        if (!is_dir) bitmap_clear(fs.bitmap, file->start_block);
        return;
    }

    fs.metadata.file_count++;

    save_metadata();
}

File *fs_find_file_in_current_dir(const char *name) {
    int idx = find_file_in_dir(fs.current_dir, name);
    if (idx == -1) return NULL;
    return get_file(idx);
}

void fs_delete_file_recursive(int file_idx) {
    File *file = get_file(file_idx);
    if (!file) return;

    if (file->is_directory) {
        Directory *dir = get_directory(file->dir_index);
        // Supprimer récursivement tous les fichiers du dossier
        while (dir->file_count > 0) {
            int child_idx = dir->file_indices[0];
            fs_delete_file_recursive(child_idx);
            remove_file_from_dir(dir, child_idx);
            // Aussi retirer dans metadata.files ?
            // On décale files en fin et décrémente file_count
            // Pour simplifier, on ne décale pas mais on va marquer le fichier comme "deleted"
            // Ici, on laisse les trous dans files (pour éviter complexité)
            // Donc on ne supprime pas du tableau global, mais on le retire des directories.
        }
    } else {
        // libérer bloc bitmap
        bitmap_clear(fs.bitmap, file->start_block);
    }
    // On ne déplace pas le tableau files, on va juste marquer la taille à 0 et name vide
    file->size = 0;
    file->name[0] = '\0';
    file->permissions = 0;
    file->parent_index = -1;
    file->dir_index = -1;
}

int confirm_deletion(const char *name) {
    printf("Le dossier '%s' n'est pas vide. Voulez-vous vraiment le supprimer ? (y/n) : ", name);
    char answer[8];
    if (fgets(answer, sizeof(answer), stdin)) {
        if (tolower(answer[0]) == 'y') {
            return 1;
        }
    }
    return 0;
}

void fs_delete_file(const char *name) {
    if (!name || strlen(name) == 0) {
        printf("Error: no name specified\n");
        return;
    }

    int idx = find_file_in_dir(fs.current_dir, name);
    if (idx == -1) {
        printf("Error: file not found\n");
        return;
    }

    File *file = get_file(idx);
    Directory *dir = NULL;

    if (file->is_directory) {
        dir = get_directory(file->dir_index);
        if (dir->file_count > 0) {
            if (!confirm_deletion(name)) {
                printf("Suppression annulée.\n");
                return;
            }
        }
    }

    // Supprimer récursivement
    fs_delete_file_recursive(idx);

    // Retirer du répertoire courant
    remove_file_from_dir(fs.current_dir, idx);

    save_metadata();
}

void fs_write_file(const char *name, const char *content) {
    if (!name) {
        printf("Error: no file specified\n");
        return;
    }
    File *file = fs_find_file_in_current_dir(name);
    if (!file || file->is_directory) {
        printf("Error: file not found or is directory\n");
        return;
    }
    size_t len = strlen(content);
    if (len > MAX_FILE_SIZE) len = MAX_FILE_SIZE;
    memcpy(fs.disk + file->start_block * BLOCK_SIZE, content, len);
    file->size = len;
    file->modified = time(NULL);

    save_metadata();
}

void fs_read_file(const char *name) {
    if (!name) {
        printf("Error: no file specified\n");
        return;
    }
    File *file = fs_find_file_in_current_dir(name);
    if (!file || file->is_directory) {
        printf("Error: file not found or is directory\n");
        return;
    }
    fwrite(fs.disk + file->start_block * BLOCK_SIZE, 1, file->size, stdout);
    printf("\n");
}

int compare_files_by_name(const void *a, const void *b) {
    const File *fa = *(const File **)a;
    const File *fb = *(const File **)b;
    return strcmp(fa->name, fb->name);
}

void fs_list() {
    if (fs.current_dir->file_count == 0) return;

    // Créer un tableau de pointeurs pour trier facilement
    File *ptrs[MAX_FILES];
    for (size_t i = 0; i < fs.current_dir->file_count; i++) {
        ptrs[i] = get_file(fs.current_dir->file_indices[i]);
    }

    qsort(ptrs, fs.current_dir->file_count, sizeof(File *), compare_files_by_name);

    printf("total %zu\n", fs.current_dir->file_count);

    for (size_t i = 0; i < fs.current_dir->file_count; i++) {
        File *f = ptrs[i];
        char perm[4] = "---";
        if (f->permissions & READ) perm[0] = 'r';
        if (f->permissions & WRITE) perm[1] = 'w';
        if (f->permissions & EXECUTE) perm[2] = 'x';

        // Format date création
        char created_str[20];
        struct tm *tm_created = localtime(&f->created);
        strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M:%S", tm_created);

        // Format date modification
        char modified_str[20];
        struct tm *tm_modified = localtime(&f->modified);
        strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M:%S", tm_modified);

        printf("%c%s\t%lu bytes\tCreated: %s\tModified: %s\t\033[34m%s\033[0m\n",
               f->is_directory ? 'd' : '-',
               perm,
               f->size,
               created_str,
               modified_str,
               f->name);
    }
}


void fs_change_dir(const char *name) {
    if (!name) {
        printf("Error: no directory specified\n");
        return;
    }

    if (strcmp(name, "..") == 0) {
        File *current_dir_file = get_file(fs.current_dir_index);
        if (current_dir_file && current_dir_file->parent_index >= 0) {
            fs.current_dir_index = current_dir_file->parent_index;
            File *parent_file = get_file(fs.current_dir_index);
            fs.current_dir = get_directory(parent_file->dir_index);
        } else {
            fs.current_dir_index = 0;
            fs.current_dir = get_directory(fs.metadata.files[0].dir_index);
        }
        return;
    }

    int idx = find_file_in_dir(fs.current_dir, name);
    if (idx == -1) {
        printf("Error: directory not found\n");
        return;
    }
    File *f = get_file(idx);
    if (!f->is_directory) {
        printf("Error: not a directory\n");
        return;
    }
    fs.current_dir_index = idx;
    fs.current_dir = get_directory(f->dir_index);
}

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
        else if (strcmp(cmd, "rm") == 0) fs_delete_file(strtok(NULL, " \n"));
        else if (strcmp(cmd, "cd") == 0) fs_change_dir(strtok(NULL, " \n"));
        else if (strcmp(cmd, "write") == 0) {
            char *file_name = strtok(NULL, " \n");
            char *text = strtok(NULL, "\n");
            if (!file_name || !text) {
                printf("Usage: write <file> <text>\n");
            } else {
                fs_write_file(file_name, text);
            }
        }
        else if (strcmp(cmd, "cat") == 0) fs_read_file(strtok(NULL, " \n"));
        else if (strcmp(cmd, "help") == 0) print_help();
        else printf("Unknown command: %s\n", cmd);
    }

    bitmap_free(fs.bitmap);
    free(fs.disk);
    return 0;
}
