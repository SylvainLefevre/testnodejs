// Mini-shim de fs::FS / File (lecture seule) au-dessus d'un dossier du PC, pour tester le sketch (-DHOST_TEST)
#pragma once
#include <stdio.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#define FILE_READ "r"

namespace fs {
class File {
 public:
  File() {}
  File(const std::string &full, const std::string &name) : full_(full), name_(name), valid_(true) {
    struct stat st;
    if (stat(full.c_str(), &st) != 0) { valid_ = false; return; }
    if (S_ISDIR(st.st_mode)) { dir_ = opendir(full.c_str()); isDir_ = true; }
    else { f_ = fopen(full.c_str(), "rb"); size_ = st.st_size; if (!f_) valid_ = false; }
  }
  File(const File &) = delete;
  File &operator=(File &&o) { close(); move(o); return *this; }
  File(File &&o) { move(o); }
  ~File() { close(); }
  explicit operator bool() const { return valid_; }
  bool isDirectory() const { return isDir_; }
  const char *name() const { return name_.c_str(); }
  size_t size() const { return size_; }
  size_t read(uint8_t *b, size_t n) { return f_ ? fread(b, 1, n, f_) : 0; }
  bool seek(uint32_t pos) { return f_ && fseek(f_, pos, SEEK_SET) == 0; }
  File openNextFile() {
    if (!dir_) return File();
    while (struct dirent *e = readdir(dir_)) {
      if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
      return File(full_ + "/" + e->d_name, e->d_name);
    }
    return File();
  }
  void close() { if (f_) fclose(f_); if (dir_) closedir(dir_); f_ = nullptr; dir_ = nullptr; valid_ = false; }
 private:
  void move(File &o) { full_ = o.full_; name_ = o.name_; f_ = o.f_; dir_ = o.dir_; isDir_ = o.isDir_; size_ = o.size_; valid_ = o.valid_; o.f_ = nullptr; o.dir_ = nullptr; o.valid_ = false; }
  std::string full_, name_;
  FILE *f_ = nullptr;
  DIR *dir_ = nullptr;
  bool isDir_ = false, valid_ = false;
  size_t size_ = 0;
};

class FS {
 public:
  explicit FS(const std::string &root = "") : root_(root) {}
  File open(const char *path, const char *mode = FILE_READ) {
    std::string p = path;
    size_t slash = p.find_last_of('/');
    std::string base = (slash == std::string::npos) ? p : p.substr(slash + 1);
    return File(root_ + (p == "/" ? "" : p), base);
  }
  std::string root_;
};
}  // namespace fs
using fs::File;
using fs::FS;
