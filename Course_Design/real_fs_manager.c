#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#define ROOT_DIR "vfs_root"
#define MAX_PATH_LEN 1024
#define MAX_LINE_LEN 4096

static void trim_newline(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[n - 1] = '\0';
        n--;
    }
}

static int read_line(const char* prompt, char* buf, size_t size) {
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }
    if (!fgets(buf, (int)size, stdin)) {
        return 0;
    }
    trim_newline(buf);
    return 1;
}

static int read_int(const char* prompt, int* out) {
    char buf[128];
    if (!read_line(prompt, buf, sizeof(buf))) return 0;
    *out = atoi(buf);
    return 1;
}

static int is_safe_relative_path(const char* rel) {
    if (!rel || rel[0] == '\0') return 0;
    if (rel[0] == '/') return 0;
    if (strstr(rel, "..") != NULL) return 0;
    return 1;
}

static void build_full_path(const char* rel, char* out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", ROOT_DIR, rel);
}

static int mkdir_p(const char* path) {
    char tmp[MAX_PATH_LEN];
    size_t len;

    if (!path || path[0] == '\0') return -1;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;

    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }

    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static int ensure_root_dir(void) {
    if (mkdir(ROOT_DIR, 0755) != 0 && errno != EEXIST) {
        perror("mkdir ROOT_DIR");
        return -1;
    }
    return 0;
}

static int ensure_parent_dir(const char* full_path) {
    char parent[MAX_PATH_LEN];
    char* slash;

    snprintf(parent, sizeof(parent), "%s", full_path);
    slash = strrchr(parent, '/');
    if (!slash) return 0;  // 没有父目录
    *slash = '\0';
    return mkdir_p(parent);
}

static int create_directory(const char* rel_path) {
    char full[MAX_PATH_LEN];

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));
    if (mkdir_p(full) != 0) {
        perror("mkdir");
        return -1;
    }

    printf("目录创建成功: %s\n", full);
    return 0;
}

static int delete_directory_recursive(const char* path) {
    DIR* dir = opendir(path);
    if (!dir) {
        return -1;
    }

    struct dirent* entry;
    char subpath[MAX_PATH_LEN];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (delete_directory_recursive(subpath) != 0) {
                closedir(dir);
                return -1;
            }
        }
        else {
            if (unlink(subpath) != 0) {
                closedir(dir);
                return -1;
            }
        }
    }

    closedir(dir);
    return rmdir(path);
}

static int delete_directory(const char* rel_path) {
    char full[MAX_PATH_LEN];

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));

    struct stat st;
    if (stat(full, &st) != 0) {
        perror("stat");
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        printf("目标不是目录。\n");
        return -1;
    }

    if (delete_directory_recursive(full) != 0) {
        perror("delete directory");
        return -1;
    }

    printf("目录删除成功: %s\n", full);
    return 0;
}

static void list_tree_recursive(const char* path, int depth) {
    DIR* dir = opendir(path);
    if (!dir) {
        return;
    }

    struct dirent* entry;
    char subpath[MAX_PATH_LEN];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        for (int i = 0; i < depth; i++) {
            printf("  ");
        }

        snprintf(subpath, sizeof(subpath), "%s/%s", path, entry->d_name);

        struct stat st;
        if (stat(subpath, &st) != 0) {
            printf("%s [无法读取属性]\n", entry->d_name);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            printf("%s/\n", entry->d_name);
            list_tree_recursive(subpath, depth + 1);
        }
        else {
            printf("%s\n", entry->d_name);
        }
    }

    closedir(dir);
}

static void list_directory_tree(void) {
    printf("\n========== 目录树 ==========\n");
    printf("%s/\n", ROOT_DIR);
    list_tree_recursive(ROOT_DIR, 1);
}

static int create_or_overwrite_file(const char* rel_path, const char* content) {
    char full[MAX_PATH_LEN];
    FILE* fp;

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));
    if (ensure_parent_dir(full) != 0) {
        perror("mkdir parent");
        return -1;
    }

    fp = fopen(full, "w");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    if (content && content[0] != '\0') {
        fputs(content, fp);
    }

    fclose(fp);
    printf("文件创建/覆盖成功: %s\n", full);
    return 0;
}

static int append_to_file(const char* rel_path, const char* content) {
    char full[MAX_PATH_LEN];
    FILE* fp;

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));
    if (ensure_parent_dir(full) != 0) {
        perror("mkdir parent");
        return -1;
    }

    fp = fopen(full, "a");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    fputs(content, fp);
    fclose(fp);

    printf("追加写入成功: %s\n", full);
    return 0;
}

static int read_file(const char* rel_path) {
    char full[MAX_PATH_LEN];
    FILE* fp;
    char buf[1024];
    size_t n;

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));
    fp = fopen(full, "r");
    if (!fp) {
        perror("fopen");
        return -1;
    }

    printf("\n========== 文件内容 ==========\n");
    printf("路径: %s\n\n", full);

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        fwrite(buf, 1, n, stdout);
    }
    printf("\n");

    fclose(fp);
    return 0;
}

static int delete_file(const char* rel_path) {
    char full[MAX_PATH_LEN];

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(rel_path, full, sizeof(full));
    if (unlink(full) != 0) {
        perror("unlink");
        return -1;
    }

    printf("文件删除成功: %s\n", full);
    return 0;
}

static int rename_entry(const char* old_rel, const char* new_rel) {
    char old_full[MAX_PATH_LEN];
    char new_full[MAX_PATH_LEN];

    if (!is_safe_relative_path(old_rel) || !is_safe_relative_path(new_rel)) {
        printf("路径非法。\n");
        return -1;
    }

    build_full_path(old_rel, old_full, sizeof(old_full));
    build_full_path(new_rel, new_full, sizeof(new_full));

    if (ensure_parent_dir(new_full) != 0) {
        perror("mkdir parent");
        return -1;
    }

    if (rename(old_full, new_full) != 0) {
        perror("rename");
        return -1;
    }

    printf("重命名/移动成功:\n%s -> %s\n", old_full, new_full);
    return 0;
}

static void show_file_info(const char* rel_path) {
    char full[MAX_PATH_LEN];
    struct stat st;

    if (!is_safe_relative_path(rel_path)) {
        printf("路径非法。\n");
        return;
    }

    build_full_path(rel_path, full, sizeof(full));
    if (stat(full, &st) != 0) {
        perror("stat");
        return;
    }

    printf("\n========== 文件/目录属性 ==========\n");
    printf("路径: %s\n", full);
    printf("类型: %s\n", S_ISDIR(st.st_mode) ? "目录" : "文件");
    printf("大小: %lld 字节\n", (long long)st.st_size);
    printf("权限: %o\n", st.st_mode & 0777);
    printf("最后修改时间: %lld\n", (long long)st.st_mtime);
}

static void show_disk_usage(void) {
    struct statvfs vfs;

    if (statvfs(ROOT_DIR, &vfs) != 0) {
        perror("statvfs");
        return;
    }

    unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize;
    unsigned long long freeb = (unsigned long long)vfs.f_bavail * vfs.f_frsize;
    unsigned long long used = total - freeb;

    printf("\n========== 磁盘空间状态 ==========\n");
    printf("挂载目录: %s\n", ROOT_DIR);
    printf("总空间: %llu 字节\n", total);
    printf("已用空间: %llu 字节\n", used);
    printf("可用空间: %llu 字节\n", freeb);
}

static void menu(void) {
    char path1[MAX_PATH_LEN];
    char path2[MAX_PATH_LEN];
    char content[MAX_LINE_LEN];
    int choice;

    while (1) {
        printf("\n====================================\n");
        printf("   真实目录管理 + 文件管理系统\n");
        printf("====================================\n");
        printf("1. 创建目录\n");
        printf("2. 删除目录（递归）\n");
        printf("3. 列出目录树\n");
        printf("4. 创建/覆盖文件\n");
        printf("5. 追加写文件\n");
        printf("6. 读文件\n");
        printf("7. 删除文件\n");
        printf("8. 重命名/移动文件或目录\n");
        printf("9. 查看文件/目录属性\n");
        printf("10. 查看磁盘空间\n");
        printf("0. 退出\n");

        if (!read_int("请选择：", &choice)) {
            printf("输入错误。\n");
            return;
        }

        switch (choice) {
        case 0:
            return;

        case 1:
            if (read_line("请输入目录相对路径，例如 docs/a/b：", path1, sizeof(path1))) {
                create_directory(path1);
            }
            break;

        case 2:
            if (read_line("请输入要删除的目录相对路径：", path1, sizeof(path1))) {
                delete_directory(path1);
            }
            break;

        case 3:
            list_directory_tree();
            break;

        case 4:
            if (read_line("请输入文件相对路径，例如 docs/a.txt：", path1, sizeof(path1)) &&
                read_line("请输入文件内容（支持空格）：", content, sizeof(content))) {
                create_or_overwrite_file(path1, content);
            }
            break;

        case 5:
            if (read_line("请输入文件相对路径：", path1, sizeof(path1)) &&
                read_line("请输入追加内容（支持空格）：", content, sizeof(content))) {
                append_to_file(path1, content);
            }
            break;

        case 6:
            if (read_line("请输入文件相对路径：", path1, sizeof(path1))) {
                read_file(path1);
            }
            break;

        case 7:
            if (read_line("请输入文件相对路径：", path1, sizeof(path1))) {
                delete_file(path1);
            }
            break;

        case 8:
            if (read_line("请输入旧路径（文件或目录）：", path1, sizeof(path1)) &&
                read_line("请输入新路径（文件或目录）：", path2, sizeof(path2))) {
                rename_entry(path1, path2);
            }
            break;

        case 9:
            if (read_line("请输入文件/目录相对路径：", path1, sizeof(path1))) {
                show_file_info(path1);
            }
            break;

        case 10:
            show_disk_usage();
            break;

        default:
            printf("无效选择。\n");
            break;
        }
    }
}

int main(void) {
    if (ensure_root_dir() != 0) {
        return 1;
    }

    printf("当前工作根目录：%s\n", ROOT_DIR);
    menu();
    return 0;
}