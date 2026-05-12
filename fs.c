#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MAGIC 0x46494C45
#define MAX_NAME 64
#define BUF_SZ (64 * 1024)

#define C1 4
#define C2 22
#define C3 14
#define TW (C1 + 1 + C2 + 1 + C3)

#define MW 37

#define CLR_RST "\033[0m"
#define CLR_GRN "\033[32m"
#define CLR_CYN "\033[36m"
#define CLR_RED "\033[31m"
#define CLR_YEL "\033[33m"
#define CLR_DIM "\033[2m"
#define CLR_REV "\033[7m"

typedef struct {
  uint32_t magic;
  uint32_t file_count;
  uint32_t total_size;
} superblock_t;

typedef struct {
  char name[MAX_NAME];
  uint32_t size;
} file_entry_t;

static const char *get_basename(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}

static void format_size(uint32_t b, char *out, size_t len) {
  if (b < 1024)
    snprintf(out, len, "%u B", b);
  else if (b < 1024u * 1024)
    snprintf(out, len, "%.1f KB", b / 1024.0);
  else if (b < 1024u * 1024 * 1024)
    snprintf(out, len, "%.1f MB", b / (1024.0 * 1024));
  else
    snprintf(out, len, "%.1f GB", b / (1024.0 * 1024 * 1024));
}

static int read_sb(int fd, superblock_t *sb) {
  if (lseek(fd, 0, SEEK_SET) < 0)
    return -1;
  if (read(fd, sb, sizeof(*sb)) != sizeof(*sb))
    return -1;
  return 0;
}

static int write_sb(int fd, const superblock_t *sb) {
  if (lseek(fd, 0, SEEK_SET) < 0)
    return -1;
  if (write(fd, sb, sizeof(*sb)) != sizeof(*sb))
    return -1;
  return 0;
}

static int find_entry(int fd, const char *name, file_entry_t *out, off_t *off) {
  superblock_t sb;
  if (read_sb(fd, &sb) < 0)
    return -1;

  off_t pos = sizeof(superblock_t);
  for (uint32_t i = 0; i < sb.file_count; i++) {
    file_entry_t e;
    if (lseek(fd, pos, SEEK_SET) < 0)
      return -1;
    if (read(fd, &e, sizeof(e)) != sizeof(e))
      return -1;
    if (strncmp(e.name, name, MAX_NAME) == 0) {
      if (out)
        *out = e;
      if (off)
        *off = pos;
      return (int)i;
    }
    pos += sizeof(file_entry_t) + e.size;
  }
  return -1;
}

static int get_entries(int fd, file_entry_t *arr, int max) {
  superblock_t sb;
  if (read_sb(fd, &sb) < 0)
    return 0;
  off_t pos = sizeof(superblock_t);
  int n = 0;
  for (uint32_t i = 0; i < sb.file_count && n < max; i++) {
    file_entry_t e;
    if (lseek(fd, pos, SEEK_SET) < 0)
      break;
    if (read(fd, &e, sizeof(e)) != sizeof(e))
      break;
    arr[n++] = e;
    pos += sizeof(file_entry_t) + e.size;
  }
  return n;
}

static int read_key(void) {
  struct termios old, raw;
  tcgetattr(STDIN_FILENO, &old);
  raw = old;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSANOW, &raw);
  int c = getchar();
  if (c == 27) {
    int c2 = getchar();
    if (c2 == '[') {
      int c3 = getchar();
      if (c3 == 'A') c = 1000;
      else if (c3 == 'B') c = 1001;
    }
  }
  tcsetattr(STDIN_FILENO, TCSANOW, &old);
  return c;
}

static int pick_file(int fd, const char *title) {
  file_entry_t files[256];
  int count = get_entries(fd, files, 256);
  if (count == 0) {
    fprintf(stderr, "  No files in filesystem\n");
    return -1;
  }
  int sel = 0;
  for (;;) {
    printf("\033[2J\033[H");
    printf("  %s (arrows + Enter, q = cancel)\n\n", title);
    for (int i = 0; i < count; i++) {
      char sz[32];
      format_size(files[i].size, sz, sizeof(sz));
      if (i == sel)
        printf("  " CLR_REV " > %-20s  %10s " CLR_RST "\n", files[i].name, sz);
      else
        printf("    %-20s  %10s\n", files[i].name, sz);
    }
    printf("\n");
    int key = read_key();
    if (key == 1000 && sel > 0) sel--;
    else if (key == 1001 && sel < count - 1) sel++;
    else if (key == '\n' || key == '\r') break;
    else if (key == 'q' || key == 'Q') return -1;
  }
  return sel;
}

static int browse_dir(char *result, size_t result_sz) {
  char cwd[1024];
  if (!getcwd(cwd, sizeof(cwd)))
    strcpy(cwd, "/");

  for (;;) {
    DIR *d = opendir(cwd);
    if (!d) {
      fprintf(stderr, "  Cannot open directory: %s\n", cwd);
      return -1;
    }

    char names[512][256];
    int is_dir[512];
    int count = 0;

    strcpy(names[count], "..");
    is_dir[count] = 1;
    count++;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 512) {
      if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
        continue;
      strncpy(names[count], ent->d_name, 255);
      names[count][255] = '\0';
      char full[1280];
      snprintf(full, sizeof(full), "%s/%s", cwd, ent->d_name);
      struct stat st;
      is_dir[count] = (stat(full, &st) == 0 && S_ISDIR(st.st_mode));
      count++;
    }
    closedir(d);

    int sel = 0;
    int done = 0;
    while (!done) {
      printf("\033[2J\033[H");
      printf("  BROWSE: %s  (arrows + Enter, q = cancel)\n\n", cwd);
      for (int i = 0; i < count; i++) {
        const char *tag = is_dir[i] ? "/" : "";
        if (i == sel)
          printf("  " CLR_REV " > %s%s " CLR_RST "\n", names[i], tag);
        else
          printf("    %s%s\n", names[i], tag);
      }
      printf("\n");

      int key = read_key();
      if (key == 1000 && sel > 0) sel--;
      else if (key == 1001 && sel < count - 1) sel++;
      else if (key == 'q' || key == 'Q') return -1;
      else if (key == '\n' || key == '\r') {
        if (is_dir[sel]) {
          if (strcmp(names[sel], "..") == 0) {
            char *slash = strrchr(cwd, '/');
            if (slash && slash != cwd)
              *slash = '\0';
            else
              strcpy(cwd, "/");
          } else {
            char tmp[1280];
            snprintf(tmp, sizeof(tmp), "%s/%s", cwd, names[sel]);
            strncpy(cwd, tmp, sizeof(cwd) - 1);
            cwd[sizeof(cwd) - 1] = '\0';
          }
          done = 1;
        } else {
          snprintf(result, result_sz, "%s/%s", cwd, names[sel]);
          return 0;
        }
      }
    }
  }
}

static void hline(const char *l, const char *j1, const char *j2,
                  const char *r) {
  printf("  %s", l);
  for (int i = 0; i < C1; i++)
    printf("─");
  printf("%s", j1);
  for (int i = 0; i < C2; i++)
    printf("─");
  printf("%s", j2);
  for (int i = 0; i < C3; i++)
    printf("─");
  printf("%s\n", r);
}

static void hline_full(const char *l, const char *r) {
  printf("  %s", l);
  for (int i = 0; i < TW; i++)
    printf("─");
  printf("%s\n", r);
}

static int disk_create(const char *path) {
  int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    fprintf(stderr, "  Error: cannot create %s: %s\n", path, strerror(errno));
    return -1;
  }
  superblock_t sb = {
      .magic = MAGIC, .file_count = 0, .total_size = sizeof(superblock_t)};
  if (write(fd, &sb, sizeof(sb)) != sizeof(sb)) {
    fprintf(stderr, "  Error: write superblock failed\n");
    close(fd);
    return -1;
  }
  return fd;
}

static int disk_open(const char *path) {
  int fd = open(path, O_RDWR);
  if (fd < 0)
    return -1;
  superblock_t sb;
  if (read(fd, &sb, sizeof(sb)) != sizeof(sb) || sb.magic != MAGIC) {
    fprintf(stderr, "  Error: %s is not a valid filesystem image\n", path);
    close(fd);
    return -1;
  }
  return fd;
}

static int disk_open_or_create(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0)
    return disk_open(path);
  printf("  Image '%s' not found, creating new filesystem.\n", path);
  return disk_create(path);
}

static int fs_upload(int fd, const char *src_path) {
  int src = open(src_path, O_RDONLY);
  if (src < 0) {
    fprintf(stderr, "  Error: cannot open '%s': %s\n", src_path,
            strerror(errno));
    return -1;
  }

  struct stat st;
  if (fstat(src, &st) < 0) {
    fprintf(stderr, "  Error: fstat failed: %s\n", strerror(errno));
    close(src);
    return -1;
  }
  uint32_t fsize = (uint32_t)st.st_size;

  const char *bname = get_basename(src_path);
  if (strlen(bname) >= MAX_NAME) {
    fprintf(stderr, "  Error: filename too long (max %d chars)\n",
            MAX_NAME - 1);
    close(src);
    return -1;
  }

  if (find_entry(fd, bname, NULL, NULL) >= 0) {
    fprintf(stderr, "  Error: file '%s' already exists. Remove it first.\n",
            bname);
    close(src);
    return -1;
  }

  superblock_t sb;
  if (read_sb(fd, &sb) < 0) {
    close(src);
    return -1;
  }

  file_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  strncpy(entry.name, bname, MAX_NAME - 1);
  entry.size = fsize;

  if (lseek(fd, sb.total_size, SEEK_SET) < 0) {
    close(src);
    return -1;
  }
  if (write(fd, &entry, sizeof(entry)) != sizeof(entry)) {
    fprintf(stderr, "  Error: write entry failed\n");
    close(src);
    return -1;
  }

  char buf[BUF_SZ];
  uint32_t remaining = fsize;
  while (remaining > 0) {
    size_t chunk = remaining < BUF_SZ ? remaining : BUF_SZ;
    ssize_t r = read(src, buf, chunk);
    if (r <= 0) {
      fprintf(stderr, "  Error: read source failed\n");
      close(src);
      return -1;
    }
    if (write(fd, buf, (size_t)r) != r) {
      fprintf(stderr, "  Error: write data failed\n");
      close(src);
      return -1;
    }
    remaining -= (uint32_t)r;
  }
  close(src);

  sb.file_count++;
  sb.total_size += sizeof(file_entry_t) + fsize;
  if (write_sb(fd, &sb) < 0)
    return -1;

  char sz[32];
  format_size(fsize, sz, sizeof(sz));
  printf("  Uploaded '%s' (%s)\n", bname, sz);
  return 0;
}

static int fs_list(int fd, const char *disk_name) {
  superblock_t sb;
  if (read_sb(fd, &sb) < 0)
    return -1;

  hline_full("┌", "┐");
  printf("  │ FILES IN: %-*s│\n", TW - 11, disk_name);

  if (sb.file_count == 0) {
    hline_full("├", "┤");
    printf("  │ %-*s│\n", TW - 1, "(empty filesystem)");
    hline_full("└", "┘");
    return 0;
  }

  hline("├", "┬", "┬", "┤");
  printf("  │ %-*s│ %-*s│ %*s │\n", C1 - 1, "#", C2 - 1, "Name", C3 - 2,
         "Size");
  hline("├", "┼", "┼", "┤");

  off_t pos = sizeof(superblock_t);
  uint64_t total_data = 0;

  for (uint32_t i = 0; i < sb.file_count; i++) {
    file_entry_t e;
    lseek(fd, pos, SEEK_SET);
    if (read(fd, &e, sizeof(e)) != sizeof(e))
      break;

    char sz[32];
    format_size(e.size, sz, sizeof(sz));
    printf("  │ %2d │ %-*.*s│ %*s │\n", i + 1, C2 - 1, C2 - 2, e.name,
           C3 - 2, sz);

    total_data += e.size;
    pos += sizeof(file_entry_t) + e.size;
  }

  hline("├", "┴", "┴", "┤");

  char ts[32];
  format_size((uint32_t)total_data, ts, sizeof(ts));
  char footer[80];
  snprintf(footer, sizeof(footer), "Total: %u file%s, %s", sb.file_count,
           sb.file_count == 1 ? "" : "s", ts);
  printf("  │ %-*s│\n", TW - 1, footer);

  hline_full("└", "┘");
  return 0;
}

static int fs_remove(int fd, const char *name) {
  file_entry_t entry;
  off_t entry_off;
  if (find_entry(fd, name, &entry, &entry_off) < 0) {
    fprintf(stderr, "  Error: file '%s' not found in filesystem\n", name);
    return -1;
  }

  superblock_t sb;
  if (read_sb(fd, &sb) < 0)
    return -1;

  uint32_t remove_size = sizeof(file_entry_t) + entry.size;
  off_t tail_off = entry_off + remove_size;
  off_t tail_size = sb.total_size - tail_off;

  char buf[BUF_SZ];
  off_t src = tail_off, dst = entry_off;
  off_t rem = tail_size;
  while (rem > 0) {
    size_t chunk = (size_t)(rem < BUF_SZ ? rem : BUF_SZ);
    lseek(fd, src, SEEK_SET);
    ssize_t r = read(fd, buf, chunk);
    if (r <= 0)
      break;
    lseek(fd, dst, SEEK_SET);
    write(fd, buf, (size_t)r);
    src += r;
    dst += r;
    rem -= r;
  }

  sb.file_count--;
  sb.total_size -= remove_size;
  if (ftruncate(fd, sb.total_size) < 0) {
    fprintf(stderr, "  Error: ftruncate: %s\n", strerror(errno));
    return -1;
  }
  if (write_sb(fd, &sb) < 0)
    return -1;

  char sz[32];
  format_size(entry.size, sz, sizeof(sz));
  printf("  Removed '%s' (%s freed)\n", name, sz);
  return 0;
}

static int fs_download(int fd, const char *name, const char *dst_path) {
  file_entry_t entry;
  off_t entry_off;
  if (find_entry(fd, name, &entry, &entry_off) < 0) {
    fprintf(stderr, "  Error: file '%s' not found in filesystem\n", name);
    return -1;
  }

  const char *out = dst_path ? dst_path : name;
  int dst = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (dst < 0) {
    fprintf(stderr, "  Error: cannot create '%s': %s\n", out, strerror(errno));
    return -1;
  }

  lseek(fd, entry_off + sizeof(file_entry_t), SEEK_SET);

  char buf[BUF_SZ];
  uint32_t rem = entry.size;
  while (rem > 0) {
    size_t chunk = rem < BUF_SZ ? rem : BUF_SZ;
    ssize_t r = read(fd, buf, chunk);
    if (r <= 0) {
      fprintf(stderr, "  Error: read failed\n");
      close(dst);
      return -1;
    }
    if (write(dst, buf, (size_t)r) != r) {
      fprintf(stderr, "  Error: write failed\n");
      close(dst);
      return -1;
    }
    rem -= (uint32_t)r;
  }
  close(dst);

  char sz[32];
  format_size(entry.size, sz, sizeof(sz));
  printf("  Downloaded '%s' -> '%s' (%s)\n", name, out, sz);
  return 0;
}

static void trim(char *s) {
  size_t l = strlen(s);
  if (l && s[l - 1] == '\n')
    s[l - 1] = 0;
}

static void mline(const char *text) { printf("  ║ %-*s║\n", MW - 1, text); }

static void show_menu(const char *disk_name, int fd) {
  superblock_t sb = {0};
  read_sb(fd, &sb);
  char sz[32];
  format_size(sb.total_size, sz, sizeof(sz));

  char tmp[64];

  printf("\n  ╔");
  for (int i = 0; i < MW; i++)
    printf("═");
  printf("╗\n");

  snprintf(tmp, sizeof(tmp), "SIMPLE FILESYSTEM v1.0");
  mline(tmp);
  snprintf(tmp, sizeof(tmp), "Disk: %s", disk_name);
  mline(tmp);
  snprintf(tmp, sizeof(tmp), "Files: %u   Size: %s", sb.file_count, sz);
  mline(tmp);

  printf("  ╠");
  for (int i = 0; i < MW; i++)
    printf("═");
  printf("╣\n");

  mline("");
  printf("  ║ " CLR_GRN "[1]" CLR_RST " %-*s║\n", MW - 6, "Upload file");
  printf("  ║ " CLR_CYN "[2]" CLR_RST " %-*s║\n", MW - 6, "List files");
  printf("  ║ " CLR_RED "[3]" CLR_RST " %-*s║\n", MW - 6, "Delete file");
  printf("  ║ " CLR_YEL "[4]" CLR_RST " %-*s║\n", MW - 6, "Download file");
  printf("  ║ " CLR_DIM "[0]" CLR_RST " %-*s║\n", MW - 6, "Exit");
  mline("");

  printf("  ╚");
  for (int i = 0; i < MW; i++)
    printf("═");
  printf("╝\n");
}

static void menu_interactive(const char *disk_path) {
  int fd = disk_open_or_create(disk_path);
  if (fd < 0)
    return;

  const char *dname = get_basename(disk_path);
  char line[512];

  for (;;) {
    show_menu(dname, fd);
    printf("  Select option: ");
    fflush(stdout);

    if (!fgets(line, sizeof(line), stdin))
      break;
    trim(line);

    if (strcmp(line, "0") == 0) {
      printf("\n  Bye!\n\n");
      break;
    }

    if (strcmp(line, "1") == 0) {
      char path[1024];
      if (browse_dir(path, sizeof(path)) == 0)
        fs_upload(fd, path);
      else
        printf("  Upload cancelled.\n");

    } else if (strcmp(line, "2") == 0) {
      printf("\n");
      fs_list(fd, dname);

    } else if (strcmp(line, "3") == 0) {
      int idx = pick_file(fd, "SELECT FILE TO DELETE");
      if (idx >= 0) {
        file_entry_t files[256];
        get_entries(fd, files, 256);
        fs_remove(fd, files[idx].name);
      }

    } else if (strcmp(line, "4") == 0) {
      int idx = pick_file(fd, "SELECT FILE TO DOWNLOAD");
      if (idx >= 0) {
        file_entry_t files[256];
        get_entries(fd, files, 256);
        printf("  Destination (Enter = same name): ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
          break;
        trim(line);
        fs_download(fd, files[idx].name, strlen(line) > 0 ? line : NULL);
      }

    } else {
      fprintf(stderr, "  Unknown option '%s'\n", line);
    }
  }

  close(fd);
}

static void print_usage(const char *prog) {
  printf("\n  Simple Filesystem v1.0\n\n");
  printf("  Usage:\n");
  printf("    %s <disk>                       Interactive menu\n", prog);
  printf("    %s <disk> upload   <src>        Upload file\n", prog);
  printf("    %s <disk> ls                    List files\n", prog);
  printf("    %s <disk> rm       <name>       Remove file\n", prog);
  printf("    %s <disk> download <name> [dst] Download file\n\n", prog);
}

int main(int argc, char *argv[]) {
  if (argc < 2 || strcmp(argv[1], "-h") == 0 ||
      strcmp(argv[1], "--help") == 0) {
    print_usage(argv[0]);
    return argc < 2 ? 1 : 0;
  }

  const char *disk = argv[1];

  if (argc == 2 || strcmp(argv[2], "--interactive") == 0) {
    menu_interactive(disk);
    return 0;
  }

  const char *cmd = argv[2];
  int fd = disk_open_or_create(disk);
  if (fd < 0)
    return 1;

  int rc = 0;

  if (strcmp(cmd, "upload") == 0) {
    if (argc < 4) {
      fprintf(stderr, "  Usage: %s <disk> upload <src>\n", argv[0]);
      rc = 1;
    } else
      rc = fs_upload(fd, argv[3]) < 0 ? 1 : 0;

  } else if (strcmp(cmd, "ls") == 0) {
    rc = fs_list(fd, get_basename(disk)) < 0 ? 1 : 0;

  } else if (strcmp(cmd, "rm") == 0) {
    if (argc < 4) {
      fprintf(stderr, "  Usage: %s <disk> rm <name>\n", argv[0]);
      rc = 1;
    } else
      rc = fs_remove(fd, argv[3]) < 0 ? 1 : 0;

  } else if (strcmp(cmd, "download") == 0) {
    if (argc < 4) {
      fprintf(stderr, "  Usage: %s <disk> download <name> [dst]\n", argv[0]);
      rc = 1;
    } else
      rc = fs_download(fd, argv[3], argc >= 5 ? argv[4] : NULL) < 0 ? 1 : 0;

  } else {
    fprintf(stderr, "  Unknown command '%s'\n", cmd);
    print_usage(argv[0]);
    rc = 1;
  }

  close(fd);
  return rc;
}
