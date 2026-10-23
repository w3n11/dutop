#define _POSIX_C_SOURCE 200809L
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// command options
static int opt_silent = 0;
static int opt_one_dev = 0;
static int opt_ignore_dot = 0;

/*
    Either a file or directory record in target directory (CWD or agument-specified).

    Attributes:
    - `name` of the file/directory
    - `size` of the file or combined size of all the files and directories in the dir
*/
typedef struct {
    char name[PATH_MAX];
    size_t size;
} entry;

/*
    LIFO stack.

    Attributes:
    - `paths` paths to directories left to be searched in, must be malloc'd
    - `size` current pos of the top of the stack
    - `capacity` maximum capacity (automatically reallocated when needed)

    Methods (associated functions):
    - `stack_init` initializes the stack
    - `push` pushes a path on the top of the stack
    - `pop` pops a path from the top of the stack
    - `free_stack` destroys the stack and frees all allocated memory
*/
typedef struct {
    char ** paths;
    size_t size;
    size_t capacity;
} paths_stack_t;


/*
    Fancy print function for errors with variable amount of arguments.

    All arguments shall be valid null terminated strings.
*/
void print_error(const char * format, ...) {
    if (opt_silent) return;
    fprintf(stderr, "treemap: ");

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

/*
    Safe free for a LIFO stack.

    ---
    The stack holds dynamically allocated directory paths (`char *`) which are yet to be searched in.
*/
void free_stack(paths_stack_t ** stack) {
    if (!stack || !*stack) return;
    for (size_t i = 0; i < (*stack)->size; i++) {
        free((*stack)->paths[i]);
    }
    free((*stack)->paths);
    free(*stack);
    *stack = NULL;
}

/*
    Initialize a LIFO stack.

    ---
    The stack holds dynamically allocated directory paths (`char *`) which are yet to be searched in.
*/
void stack_init(paths_stack_t ** stack) {
    *stack = NULL;
    *stack = malloc(sizeof(**stack));
    if (!*stack) return;
    (*stack)->capacity = 16;
    (*stack)->size = 0;
    (*stack)->paths = malloc((*stack)->capacity * sizeof(char *));
    if (!(*stack)->paths) {
        free(*stack);
        *stack = NULL;
    }
}

/*
    Push into a LIFO stack.

    ---
    The stack holds dynamically allocated directory paths (`char *`) which are yet to be searched in.
*/
int push(paths_stack_t ** stack, char * path) {
    if ((*stack)->size + 1 >= (*stack)->capacity) {
        size_t new_capacity = (*stack)->capacity * 2;
        char ** new_ptr = realloc((*stack)->paths, new_capacity * sizeof(char *));
        if (!new_ptr) {
            free_stack(stack);
            return -1;
        }
        (*stack)->paths = new_ptr;
        (*stack)->capacity = new_capacity;
    }
    (*stack)->paths[(*stack)->size++] = path;
    return 0;
}

/*
    Pop from a LIFO stack.

    Args:
    - `stack` ref to ref to the LIFO stack

    Returns:
    - the next path added on top
    - `NULL` otherwise

    ---
    The stack holds dynamically allocated directory paths (`char *`) which are yet to be searched in.
*/
char * pop(paths_stack_t ** stack) {
    if (!*stack || (*stack)->size == 0) return NULL;
    return (*stack)->paths[--(*stack)->size];
}

/*
    Search the directory and calculate its size. The name and size are saved in the `entry`.

    Args:
    - `entry` the record of the name of the directory along with its calculated size
    - `dir_path` the directory path to search in
    - `source_dev` the filesystem id (used to isolate files in one dev: option -x)

    Returns:
    - `0` if no errors occured, else `-1`
*/
int get_entry_from_dir(entry * entry, char * dir_path, dev_t source_dev) {
    int retval = -1;
    paths_stack_t * stack = NULL;
    DIR * dir = NULL;
    entry->size = 0;
    snprintf(entry->name, PATH_MAX, "%s", dir_path);
    
    stack_init(&stack);
    if (!stack) goto err;
    char * initial_path = strdup(dir_path);
    if (!initial_path) goto err;
    push(&stack, initial_path);

    while (stack->size > 0) {
        char * curr_dir_path = pop(&stack);

        dir = opendir(curr_dir_path);
        if (!dir) {
            print_error("cannot open directory '%s': %s\n", curr_dir_path, strerror(errno));
            free(curr_dir_path);
            continue;
        }

        int dir_fd = dirfd(dir);
        if (dir_fd == -1) {
            print_error("cannot get file descriptor for '%s': %s\n", curr_dir_path, strerror(errno));
            free(curr_dir_path);
            closedir(dir);
            continue;
        }
        
        struct dirent * dirent;
        struct stat sb;
        rewinddir(dir);
        while ((dirent = readdir(dir))) {
            if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) continue;
            if (fstatat(dir_fd, dirent->d_name, &sb, AT_SYMLINK_NOFOLLOW) == -1) continue;
            if (opt_one_dev && sb.st_dev != source_dev) continue;
            entry->size += sb.st_size;
            
            if (S_ISDIR(sb.st_mode)) {
                size_t path_len = strlen(curr_dir_path) + 1 + strlen(dirent->d_name) + 1;
                char * new_path = malloc(path_len);
                if (!new_path) {
                    closedir(dir);
                    free(curr_dir_path);
                    goto err;
                }
                snprintf(new_path, path_len, "%s/%s", curr_dir_path, dirent->d_name);
                push(&stack, new_path);
            }
        }
        if (dir) closedir(dir);
        dir = NULL;
        free(curr_dir_path);
    }
    retval = 0;
err:
    if (stack) free_stack(&stack);
    if (dir) closedir(dir);
    return retval;
}

/*
    Compares entries (`entry` type) based on their attribute `size`. Used as custom sort function. Behaviour standard.
*/
int cmp_entries(const void * a , const void * b) {
    size_t size_a = ((entry *)a)->size;
    size_t size_b = ((entry *)b)->size;
    if (size_b > size_a) return 1;
    if (size_b < size_a) return -1;
    return 0;
}

/*
    Prints custom bar using [] brackets and # hash, ie. `[####  ]` showing how much **roughly** `actual_size` is compared to `max_size`.

    Args:
    - `actual_size` the size of the entry
    - `max_size` the size of the largest entry in the directory
    - `bar_width` the step (how many hashes would the full bar have)

    ---
    If the `bar_width` is less than 3, the function does nothing and ends silently.
*/
void print_bar(size_t actual_size, size_t max_size, size_t bar_width) {
    if (bar_width < 3) return;

    char * bar = malloc(bar_width + 1);
    if (!bar) return;

    memset(bar, ' ', bar_width);

    size_t hash_count = 0;
    if (max_size > 0) {
        hash_count = (size_t)(((double)actual_size / max_size) * (bar_width - 2));
    }
    
    if (hash_count > bar_width - 2) hash_count = bar_width - 2;

    memset(bar + 1, '#', hash_count);
    bar[0] = '['; 
    bar[bar_width - 1] = ']'; 
    bar[bar_width] = '\0';
    
    printf("%s", bar);
    free(bar);
}

/*
    Formats the size in bytes into more readable form.

    Args:
    - `size` of the entry
    - `buf` the buffer to write the final format into
*/
void format_size(size_t size, char *buf) {
    const double TB = 1024.0 * 1024.0 * 1024.0 * 1024.0;
    const double GB = 1024.0 * 1024.0 * 1024.0;
    const double MB = 1024.0 * 1024.0;
    const double KB = 1024.0;
    if (size >= TB) {
        sprintf(buf, "%.2f TB", (double)size / TB);
    } else if (size >= GB) {
        sprintf(buf, "%.2f GB", (double)size / GB);
    } else if (size >= MB) {
        sprintf(buf, "%.2f MB", (double)size / MB);
    } else if (size >= KB) {
        sprintf(buf, "%.2f KB", (double)size / KB);
    } else {
        sprintf(buf, "%lu B", size);
    }
}


// main
int main(int argc, char * argv[]) {
    int retval = EXIT_FAILURE;
    size_t n_entries = 10;
    int bar_width = 34; // the final bar summary default width (32 + 2 for brackets)
    char * target_dir = "."; // default dir is cwd
    DIR * dir = NULL;
    entry * entries = NULL;

    int opt;  // love getopt <3
    while ((opt = getopt(argc, argv, "n:w:sxh")) != -1) {
        switch (opt) {
            case 'n': {
                long arg_entries = atol(optarg);
                if (arg_entries < 0) {
                    print_error("invalid argument: count should be greater or equal to 0.\n");
                    goto err;
                }
                if (arg_entries == 0) {
                    n_entries = (size_t)-1;  // max amount (18 446 744 073 709 551 615 entries)
                } else {
                    n_entries = (size_t)arg_entries;
                }
                break;
            }
            case 'w': {
                long arg_bar_width = atol(optarg);
                if (arg_bar_width < 1) {
                    print_error("invalid argument: bar width should be greater or equal to 1.\n");
                    goto err;
                }
                bar_width = (int)arg_bar_width + 2; // + 2 for brackets
                break;
            }
            case 'i': {
                opt_ignore_dot = 1; // ignore (hidden) .files/directories
                break;
            }
            case 's': {
                opt_silent = 1; // supress error print
                break;
            }
            case 'x': {
                opt_one_dev = 1; // limit to current filesystem
                break;
            }
            case 'h':
            case '?':
            default: // help
                fprintf(stderr, "Usage: %s [-n count] [-w barwidth] [-s] [directory]\n", argv[0]);
                fprintf(stderr, "Options:\n");
                fprintf(stderr, "  -n count     Number of entries to show (0 for all, default: 10)\n");
                fprintf(stderr, "  -w barwidth  Width of the bar graph excluding [] brackets (default: 32)\n");
                fprintf(stderr, "  -s           Silent mode (suppress errors)\n");
                fprintf(stderr, "  -x           Stay on one file system (ignore /proc, /sys, USBs, etc.)\n");
                fprintf(stderr, "  -i           Ignore hidden files/directories\n");
                fprintf(stderr, "  -h           Show this help message\n");
                goto err;
        }
    }

    if (optind < argc) { // optional argument path
        target_dir = argv[optind];
    }

    if (chdir(target_dir) != 0) { // cwd change
        print_error("cannot access '%s': %s\n", target_dir, strerror(errno));
        goto err;
    }

    struct dirent * dirent = NULL;
    struct stat sb;

    size_t entries_size = 256;
    entries = calloc(entries_size, sizeof(entry));
    if (!entries) {
        print_error("memory allocation failed: %s\n", strerror(errno));
        goto err;
    }
    
    dir = opendir(".");
    if (!dir) {
        print_error("cannot open target directory: %s\n", strerror(errno));
        goto err;
    }
    int dir_fd = dirfd(dir);
    if (dir_fd == -1) {
        print_error("cannot get directory file descriptor: %s\n", strerror(errno));
        goto err;
    }

    struct stat target_sb;
    if (fstat(dir_fd, &target_sb) == -1) {
        print_error("cannot stat target directory: %s\n", strerror(errno));
        goto err;
    }
    dev_t source_dev = target_sb.st_dev; // filesystem id

    size_t total_entries = 0;
    size_t others_size = 0;
    size_t n_entry = 0;

    rewinddir(dir);
    while ((dirent = readdir(dir))) {
        if (opt_ignore_dot && dirent->d_name[0] == '.') continue;
        if (strcmp(dirent->d_name, ".") == 0 || strcmp(dirent->d_name, "..") == 0) continue;
        if (fstatat(dir_fd, dirent->d_name, &sb, AT_SYMLINK_NOFOLLOW) == -1) continue;
        if (opt_one_dev && sb.st_dev != source_dev) continue;
        entry curr_entry = { 0 };
        if (S_ISDIR(sb.st_mode)) {
            if (get_entry_from_dir(&curr_entry, dirent->d_name, source_dev)) {
                print_error("failed to traverse directory '%s'\n", dirent->d_name);
                continue;
            }
        } else if (S_ISREG(sb.st_mode)) {
            strcpy(curr_entry.name, dirent->d_name);
            curr_entry.size = sb.st_size;
        }
        if (curr_entry.size > 0) {
            if (n_entry + 1 == entries_size) {
                entry * new_entries = realloc(entries, entries_size * 2 * sizeof(entry));
                if (!new_entries) goto err;
                entries = new_entries;
                entries_size *= 2;
            }
            entries[n_entry++] = curr_entry;
        }
        total_entries++;
    }
    qsort(entries, n_entry, sizeof(entry), cmp_entries);

    size_t to_print = (n_entry < n_entries) ? n_entry : n_entries;
    others_size = 0;
    for (size_t i = to_print; i < n_entry; i++) {
        others_size += entries[i].size;
    }
    size_t max_val = entries[0].size; 
    if (others_size > max_val) {
        max_val = others_size;
    }
    if (max_val == 0) max_val = 1;


    for (size_t i = 0; i < to_print; i++) {
        char size_str[32];
        format_size(entries[i].size, size_str);
        printf("%12s ", size_str);
        print_bar(entries[i].size, max_val, bar_width);
        printf(" %s\n", entries[i].name);
    }

    if (others_size > 0) {
        char size_str[32];
        format_size(others_size, size_str);
        
        printf("%12s ", size_str);
        print_bar(others_size, max_val, bar_width);
        printf(" ...and %lu others.\n", n_entry - to_print);
    }

    size_t total_size = others_size;
    for(size_t i=0; i<to_print; i++) total_size += entries[i].size;
    // printf("----------------------------------------------------------------\n");
    char total_size_str[32];
    format_size(total_size, total_size_str);
    printf("%12s TOTAL\n", total_size_str);

    retval = 0;
err:
    if (dir) closedir(dir);
    if (entries) free(entries);
    return retval;
}