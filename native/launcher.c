/*
 * Copyright (C) 2026 NaiveFox Android Plugin contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include "NaiveFoxAPI.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#include "runtime_manifest.h"

#ifndef NAIVEFOX_RUNTIME_RELATIVE_PATH
#  error "NAIVEFOX_RUNTIME_RELATIVE_PATH must be supplied by CMake"
#endif

#define CONFIG_MAXIMUM_BYTES (1024U * 1024U)
#define REEXEC_MARKER "NAIVEFOX_LAUNCHER_REEXEC_PATH"
#define RUNTIME_ROOT_ENV "NAIVEFOX_LAUNCHER_RUNTIME_ROOT"
#define RUNTIME_ASSET_PREFIX "assets/plugin/runtime/"
#define ZIP_EOCD_SIGNATURE UINT32_C(0x06054b50)
#define ZIP_CENTRAL_SIGNATURE UINT32_C(0x02014b50)
#define ZIP_LOCAL_SIGNATURE UINT32_C(0x04034b50)

typedef int (*RunEmbeddedFunction)(const char*, const char*, const char*);
typedef void (*RequestStopFunction)(void);
typedef const char* (*VersionFunction)(void);

typedef struct StopThreadContext {
  RequestStopFunction request_stop;
  atomic_bool run_entered;
  atomic_bool finished;
  sigset_t signal_set;
} StopThreadContext;

static bool JoinPath(char* destination, size_t capacity, const char* base,
                     const char* relative) {
  int count = snprintf(destination, capacity, "%s/%s", base, relative);
  return count >= 0 && (size_t)count < capacity;
}

static bool ParentDirectory(char* path) {
  char* separator = strrchr(path, '/');
  if (!separator || separator == path) {
    return false;
  }
  *separator = '\0';
  return true;
}

static uint16_t ReadLittle16(const unsigned char* value) {
  return (uint16_t)((uint16_t)value[0] | ((uint16_t)value[1] << 8U));
}

static uint32_t ReadLittle32(const unsigned char* value) {
  return (uint32_t)value[0] | ((uint32_t)value[1] << 8U) |
         ((uint32_t)value[2] << 16U) | ((uint32_t)value[3] << 24U);
}

static bool ReadAt(int descriptor, void* destination, size_t length,
                   uint64_t offset) {
  unsigned char* bytes = destination;
  size_t completed = 0U;
  while (completed < length) {
    if (offset > (uint64_t)INT64_MAX - (uint64_t)completed) {
      return false;
    }
    ssize_t count = pread(descriptor, bytes + completed, length - completed,
                          (off_t)(offset + (uint64_t)completed));
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      return false;
    }
    completed += (size_t)count;
  }
  return true;
}

static bool SafeAssetPath(const char* path) {
  if (!path || !*path || *path == '/' || strchr(path, '\\')) {
    return false;
  }
  const char* part = path;
  while (*part) {
    const char* separator = strchr(part, '/');
    size_t length = separator ? (size_t)(separator - part) : strlen(part);
    if (length == 0U || (length == 1U && part[0] == '.') ||
        (length == 2U && part[0] == '.' && part[1] == '.')) {
      return false;
    }
    if (!separator) {
      break;
    }
    part = separator + 1;
  }
  return true;
}

static bool EnsureAssetParents(const char* root, const char* relative) {
  char path[PATH_MAX + 1];
  if (!JoinPath(path, sizeof(path), root, relative)) {
    return false;
  }
  size_t root_length = strlen(root);
  for (char* cursor = path + root_length + 1U; *cursor; ++cursor) {
    if (*cursor != '/') {
      continue;
    }
    *cursor = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
      *cursor = '/';
      return false;
    }
    *cursor = '/';
  }
  return true;
}

static bool CopyStoredEntry(int archive, uint64_t archive_offset,
                            uint32_t length, const char* destination) {
  int output = open(destination,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0700);
  if (output < 0) {
    return false;
  }
  unsigned char buffer[64U * 1024U];
  uint32_t remaining = length;
  uint64_t offset = archive_offset;
  bool success = true;
  while (remaining > 0U) {
    size_t amount = remaining < (uint32_t)sizeof(buffer)
                        ? (size_t)remaining
                        : sizeof(buffer);
    if (!ReadAt(archive, buffer, amount, offset)) {
      success = false;
      break;
    }
    size_t written = 0U;
    while (written < amount) {
      ssize_t count = write(output, buffer + written, amount - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        success = false;
        break;
      }
      written += (size_t)count;
    }
    if (!success) {
      break;
    }
    remaining -= (uint32_t)amount;
    offset += (uint64_t)amount;
  }
  if (close(output) != 0) {
    success = false;
  }
  if (!success) {
    (void)unlink(destination);
  }
  return success;
}

static bool CopyDeflatedEntry(int archive, uint64_t archive_offset,
                              uint32_t compressed_length,
                              uint32_t uncompressed_length,
                              const char* destination) {
  int output = open(destination,
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                    0700);
  if (output < 0) {
    return false;
  }
  unsigned char input[64U * 1024U];
  unsigned char result[64U * 1024U];
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  bool initialized = inflateInit2(&stream, -MAX_WBITS) == Z_OK;
  bool success = initialized;
  uint32_t remaining = compressed_length;
  uint64_t offset = archive_offset;
  uint64_t total_output = 0U;
  int inflate_status = Z_OK;
  while (success && inflate_status != Z_STREAM_END) {
    if (stream.avail_in == 0U && remaining > 0U) {
      size_t amount = remaining < (uint32_t)sizeof(input)
                          ? (size_t)remaining
                          : sizeof(input);
      if (!ReadAt(archive, input, amount, offset)) {
        success = false;
        break;
      }
      stream.next_in = input;
      stream.avail_in = (uInt)amount;
      remaining -= (uint32_t)amount;
      offset += (uint64_t)amount;
    }
    stream.next_out = result;
    stream.avail_out = (uInt)sizeof(result);
    inflate_status = inflate(&stream, Z_NO_FLUSH);
    if (inflate_status != Z_OK && inflate_status != Z_STREAM_END) {
      success = false;
      break;
    }
    size_t produced = sizeof(result) - (size_t)stream.avail_out;
    size_t written = 0U;
    while (written < produced) {
      ssize_t count = write(output, result + written, produced - written);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        success = false;
        break;
      }
      written += (size_t)count;
    }
    total_output += (uint64_t)produced;
    if (produced == 0U && stream.avail_in == 0U && remaining == 0U &&
        inflate_status != Z_STREAM_END) {
      success = false;
    }
  }
  bool complete = remaining == 0U && stream.avail_in == 0U &&
                  total_output == (uint64_t)uncompressed_length;
  if (initialized && inflateEnd(&stream) != Z_OK) {
    success = false;
  }
  if (!complete) {
    success = false;
  }
  if (close(output) != 0) {
    success = false;
  }
  if (!success) {
    (void)unlink(destination);
  }
  return success;
}

static bool ResolveBaseApk(const char* native_library_directory,
                           char* apk_path, size_t capacity) {
  char install_root[PATH_MAX + 1];
  int count = snprintf(install_root, sizeof(install_root), "%s",
                       native_library_directory);
  if (count < 0 || (size_t)count >= sizeof(install_root) ||
      !ParentDirectory(install_root) || !ParentDirectory(install_root) ||
      !JoinPath(apk_path, capacity, install_root, "base.apk")) {
    return false;
  }
  struct stat status;
  return stat(apk_path, &status) == 0 && S_ISREG(status.st_mode);
}

static bool ExtractRuntimeAssets(const char* apk_path,
                                 const char* destination_root) {
  int archive = open(apk_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (archive < 0) {
    fprintf(stderr, "naive-plugin: cannot open own APK: %s\n", strerror(errno));
    return false;
  }
  struct stat status;
  if (fstat(archive, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 22) {
    close(archive);
    return false;
  }
  uint64_t archive_size = (uint64_t)status.st_size;
  size_t tail_size = archive_size < UINT64_C(65557)
                         ? (size_t)archive_size
                         : (size_t)65557U;
  unsigned char* tail = malloc(tail_size);
  if (!tail || !ReadAt(archive, tail, tail_size, archive_size - tail_size)) {
    free(tail);
    close(archive);
    return false;
  }

  size_t eocd_index = SIZE_MAX;
  for (size_t cursor = tail_size - 22U;; --cursor) {
    if (ReadLittle32(tail + cursor) == ZIP_EOCD_SIGNATURE &&
        cursor + 22U + (size_t)ReadLittle16(tail + cursor + 20U) == tail_size) {
      eocd_index = cursor;
      break;
    }
    if (cursor == 0U) {
      break;
    }
  }
  if (eocd_index == SIZE_MAX) {
    fprintf(stderr, "naive-plugin: own APK has no supported ZIP directory\n");
    free(tail);
    close(archive);
    return false;
  }
  const unsigned char* eocd = tail + eocd_index;
  uint16_t disk = ReadLittle16(eocd + 4U);
  uint16_t central_disk = ReadLittle16(eocd + 6U);
  uint16_t disk_entries = ReadLittle16(eocd + 8U);
  uint16_t total_entries = ReadLittle16(eocd + 10U);
  uint32_t central_size = ReadLittle32(eocd + 12U);
  uint32_t central_offset = ReadLittle32(eocd + 16U);
  free(tail);
  if (disk != 0U || central_disk != 0U || disk_entries != total_entries ||
      total_entries == UINT16_MAX || central_size == UINT32_MAX ||
      central_offset == UINT32_MAX ||
      (uint64_t)central_offset + (uint64_t)central_size > archive_size) {
    fprintf(stderr, "naive-plugin: unsupported APK ZIP layout\n");
    close(archive);
    return false;
  }

  uint64_t cursor = central_offset;
  uint64_t central_end = cursor + central_size;
  size_t extracted = 0U;
  const size_t prefix_length = sizeof(RUNTIME_ASSET_PREFIX) - 1U;
  for (uint16_t index = 0U; index < total_entries; ++index) {
    unsigned char header[46];
    if (cursor > central_end || central_end - cursor < sizeof(header) ||
        !ReadAt(archive, header, sizeof(header), cursor) ||
        ReadLittle32(header) != ZIP_CENTRAL_SIGNATURE) {
      close(archive);
      return false;
    }
    uint16_t flags = ReadLittle16(header + 8U);
    uint16_t method = ReadLittle16(header + 10U);
    uint32_t compressed_size = ReadLittle32(header + 20U);
    uint32_t uncompressed_size = ReadLittle32(header + 24U);
    uint16_t name_length = ReadLittle16(header + 28U);
    uint16_t extra_length = ReadLittle16(header + 30U);
    uint16_t comment_length = ReadLittle16(header + 32U);
    uint32_t local_offset = ReadLittle32(header + 42U);
    uint64_t record_size = UINT64_C(46) + name_length + extra_length + comment_length;
    if (name_length == 0U || name_length > PATH_MAX ||
        cursor + record_size > central_end) {
      close(archive);
      return false;
    }
    char name[PATH_MAX + 1];
    if (!ReadAt(archive, name, name_length, cursor + UINT64_C(46))) {
      close(archive);
      return false;
    }
    name[name_length] = '\0';
    cursor += record_size;

    if (strncmp(name, RUNTIME_ASSET_PREFIX, prefix_length) != 0 ||
        name[name_length - 1U] == '/') {
      continue;
    }
    const char* relative = name + prefix_length;
    if (!SafeAssetPath(relative) || (flags & 1U) != 0U ||
        (method != 0U && method != 8U) ||
        (method == 0U && compressed_size != uncompressed_size) ||
        local_offset == UINT32_MAX) {
      fprintf(stderr, "naive-plugin: invalid runtime APK entry: %s\n", name);
      close(archive);
      return false;
    }
    unsigned char local[30];
    if (!ReadAt(archive, local, sizeof(local), local_offset) ||
        ReadLittle32(local) != ZIP_LOCAL_SIGNATURE) {
      close(archive);
      return false;
    }
    uint16_t local_name_length = ReadLittle16(local + 26U);
    uint16_t local_extra_length = ReadLittle16(local + 28U);
    uint64_t data_offset = (uint64_t)local_offset + UINT64_C(30) +
                           local_name_length + local_extra_length;
    if (data_offset + compressed_size > archive_size ||
        !EnsureAssetParents(destination_root, relative)) {
      close(archive);
      return false;
    }
    char destination[PATH_MAX + 1];
    if (!JoinPath(destination, sizeof(destination), destination_root, relative)) {
      close(archive);
      return false;
    }
    bool copied = method == 0U
                      ? CopyStoredEntry(archive, data_offset, uncompressed_size,
                                        destination)
                      : CopyDeflatedEntry(archive, data_offset, compressed_size,
                                          uncompressed_size, destination);
    if (!copied) {
      fprintf(stderr, "naive-plugin: cannot extract runtime asset: %s\n", name);
      close(archive);
      return false;
    }
    ++extracted;
  }
  close(archive);
  return extracted > 0U;
}

static bool ResolveSelf(char* executable, size_t executable_capacity,
                        char* directory, size_t directory_capacity) {
  ssize_t length = readlink("/proc/self/exe", executable,
                            executable_capacity - 1U);
  if (length <= 0 || (size_t)length >= executable_capacity) {
    return false;
  }
  executable[length] = '\0';
  const char* separator = strrchr(executable, '/');
  if (!separator || separator == executable) {
    return false;
  }
  size_t directory_length = (size_t)(separator - executable);
  if (directory_length >= directory_capacity) {
    return false;
  }
  memcpy(directory, executable, directory_length);
  directory[directory_length] = '\0';
  return true;
}

static bool PrependLibraryPath(const char* runtime_path) {
  const char* existing = getenv("LD_LIBRARY_PATH");
  size_t runtime_length = strlen(runtime_path);
  size_t existing_length = existing ? strlen(existing) : 0U;
  if (runtime_length > SIZE_MAX - existing_length - 2U) {
    return false;
  }
  size_t length = runtime_length + (existing_length ? 1U + existing_length : 0U);
  char* value = malloc(length + 1U);
  if (!value) {
    return false;
  }
  memcpy(value, runtime_path, runtime_length);
  if (existing_length) {
    value[runtime_length] = ':';
    memcpy(value + runtime_length + 1U, existing, existing_length);
  }
  value[length] = '\0';
  bool success = setenv("LD_LIBRARY_PATH", value, 1) == 0;
  free(value);
  return success;
}

static int EnsureRuntimeLinkerEnvironment(int argc, char* argv[],
                                          const char* executable,
                                          const char* runtime_path) {
  (void)argc;
  const char* marker = getenv(REEXEC_MARKER);
  if (marker && strcmp(marker, executable) == 0) {
    if (unsetenv(REEXEC_MARKER) != 0) {
      fprintf(stderr, "naive-plugin: cannot clear launcher marker: %s\n",
              strerror(errno));
      return -1;
    }
    return 0;
  }
  if (!PrependLibraryPath(runtime_path) ||
      setenv(REEXEC_MARKER, executable, 1) != 0) {
    fprintf(stderr, "naive-plugin: cannot prepare Android linker environment\n");
    return -1;
  }
  execv(executable, argv);
  fprintf(stderr, "naive-plugin: cannot re-execute launcher: %s\n",
          strerror(errno));
  return -1;
}

static bool ReadConfig(const char* path, char** contents) {
  *contents = NULL;
  int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    fprintf(stderr, "naive-plugin: cannot open config: %s\n", strerror(errno));
    return false;
  }
  struct stat status;
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 || (uint64_t)status.st_size > CONFIG_MAXIMUM_BYTES) {
    fprintf(stderr,
            "naive-plugin: config must be a non-empty regular file at most 1 MiB\n");
    close(descriptor);
    return false;
  }
  size_t length = (size_t)status.st_size;
  char* buffer = malloc(length + 1U);
  if (!buffer) {
    fprintf(stderr, "naive-plugin: cannot allocate config buffer\n");
    close(descriptor);
    return false;
  }
  size_t offset = 0U;
  while (offset < length) {
    ssize_t count = read(descriptor, buffer + offset, length - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      fprintf(stderr, "naive-plugin: cannot read complete config\n");
      free(buffer);
      close(descriptor);
      return false;
    }
    offset += (size_t)count;
  }
  close(descriptor);
  if (memchr(buffer, '\0', length)) {
    fprintf(stderr, "naive-plugin: config contains an embedded NUL byte\n");
    free(buffer);
    return false;
  }
  buffer[length] = '\0';
  *contents = buffer;
  return true;
}

static int RemoveTreeEntry(const char* path, const struct stat* status,
                           int type, struct FTW* state) {
  (void)status;
  (void)type;
  (void)state;
  return remove(path);
}

static bool RemoveProfile(const char* profile_path) {
  if (!profile_path || !*profile_path) {
    return true;
  }
  return nftw(profile_path, RemoveTreeEntry, 32, FTW_DEPTH | FTW_PHYS) == 0;
}

static void* ResolveSymbol(void* library, const char* name) {
  dlerror();
  void* symbol = dlsym(library, name);
  const char* error = dlerror();
  if (error) {
    fprintf(stderr, "naive-plugin: cannot resolve %s: %s\n", name, error);
    return NULL;
  }
  return symbol;
}

static bool PreloadAndroidSystemLibraries(void) {
  for (size_t index = 0U; index < NAIVEFOX_ANDROID_SYSTEM_LIBRARY_COUNT;
       ++index) {
    const char* name = kNaiveFoxAndroidSystemLibraries[index];
    if (!dlopen(name, RTLD_NOW | RTLD_GLOBAL)) {
      fprintf(stderr, "naive-plugin: cannot preload Android library %s: %s\n",
              name, dlerror());
      return false;
    }
  }
  return true;
}

static bool ResolveFunctions(void* library, RunEmbeddedFunction* run,
                             RequestStopFunction* stop,
                             VersionFunction* version) {
  _Static_assert(sizeof(*run) == sizeof(void*),
                 "function and object pointers must have the same size");
  void* run_symbol = ResolveSymbol(library, "NaiveFoxRunEmbedded");
  void* stop_symbol = ResolveSymbol(library, "NaiveFoxRequestStop");
  void* version_symbol = ResolveSymbol(library, "NaiveFoxVersion");
  if (!run_symbol || !stop_symbol || !version_symbol) {
    return false;
  }
  memcpy(run, &run_symbol, sizeof(*run));
  memcpy(stop, &stop_symbol, sizeof(*stop));
  memcpy(version, &version_symbol, sizeof(*version));
  return true;
}

static void* StopThreadMain(void* opaque) {
  StopThreadContext* context = opaque;
  int received = 0;
  if (sigwait(&context->signal_set, &received) != 0 || received == SIGUSR1) {
    return NULL;
  }

  const struct timespec retry_delay = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
  while (!atomic_load_explicit(&context->finished, memory_order_acquire)) {
    if (atomic_load_explicit(&context->run_entered, memory_order_acquire)) {
      context->request_stop();
    }
    (void)nanosleep(&retry_delay, NULL);
  }
  return NULL;
}

static int MapStatusToExitCode(int status) {
  if (status == NAIVEFOX_STATUS_OK) {
    return 0;
  }
  if (status == NAIVEFOX_STATUS_INVALID_ARGUMENT) {
    return 2;
  }
  return 1;
}

int main(int argc, char* argv[]) {
  if (argc != 2 || !argv[1] || !*argv[1]) {
    fprintf(stderr, "usage: naive-plugin CONFIG_PATH\n");
    return 2;
  }

  char executable[PATH_MAX + 1];
  char plugin_directory[PATH_MAX + 1];
  char working_directory[PATH_MAX + 1];
  char runtime_root[PATH_MAX + 1];
  char runtime_path[PATH_MAX + 1];
  char libxul_path[PATH_MAX + 1];
  if (!ResolveSelf(executable, sizeof(executable), plugin_directory,
                   sizeof(plugin_directory)) ||
      !getcwd(working_directory, sizeof(working_directory))) {
    fprintf(stderr, "naive-plugin: cannot resolve launcher paths\n");
    return 1;
  }

  const char* marker = getenv(REEXEC_MARKER);
  const char* inherited_runtime = getenv(RUNTIME_ROOT_ENV);
  bool reexecuted = marker && strcmp(marker, executable) == 0;
  if (reexecuted) {
    size_t working_length = strlen(working_directory);
    if (!inherited_runtime ||
        strncmp(inherited_runtime, working_directory, working_length) != 0 ||
        inherited_runtime[working_length] != '/' ||
        snprintf(runtime_root, sizeof(runtime_root), "%s", inherited_runtime) < 0 ||
        strlen(inherited_runtime) >= sizeof(runtime_root)) {
      fprintf(stderr, "naive-plugin: invalid inherited runtime directory\n");
      return 1;
    }
  } else {
    int root_count = snprintf(runtime_root, sizeof(runtime_root),
                              "%s/.naivefox-runtime-%ld-XXXXXX",
                              working_directory, (long)getpid());
    char apk_path[PATH_MAX + 1];
    if (root_count < 0 || (size_t)root_count >= sizeof(runtime_root) ||
        !mkdtemp(runtime_root) || chmod(runtime_root, 0700) != 0 ||
        !ResolveBaseApk(plugin_directory, apk_path, sizeof(apk_path)) ||
        !ExtractRuntimeAssets(apk_path, runtime_root)) {
      fprintf(stderr, "naive-plugin: cannot prepare embedded runtime: %s\n",
              strerror(errno));
      (void)RemoveProfile(runtime_root);
      return 1;
    }
  }

  if (!JoinPath(runtime_path, sizeof(runtime_path), runtime_root,
                NAIVEFOX_RUNTIME_RELATIVE_PATH) ||
      !JoinPath(libxul_path, sizeof(libxul_path), runtime_path, "libxul.so")) {
    fprintf(stderr, "naive-plugin: cannot resolve extracted runtime paths\n");
    (void)RemoveProfile(runtime_root);
    return 1;
  }

  struct stat runtime_status;
  struct stat libxul_status;
  if (stat(runtime_path, &runtime_status) != 0 ||
      !S_ISDIR(runtime_status.st_mode) ||
      stat(libxul_path, &libxul_status) != 0 ||
      !S_ISREG(libxul_status.st_mode)) {
    fprintf(stderr, "naive-plugin: extracted NaiveFox runtime is incomplete\n");
    (void)RemoveProfile(runtime_root);
    return 1;
  }

  if (!reexecuted && setenv(RUNTIME_ROOT_ENV, runtime_root, 1) != 0) {
    fprintf(stderr, "naive-plugin: cannot preserve extracted runtime path\n");
    (void)RemoveProfile(runtime_root);
    return 1;
  }
  if (EnsureRuntimeLinkerEnvironment(argc, argv, executable, runtime_path) != 0) {
    (void)RemoveProfile(runtime_root);
    return 1;
  }
  (void)unsetenv(RUNTIME_ROOT_ENV);

  sigset_t signal_set;
  if (sigemptyset(&signal_set) != 0 || sigaddset(&signal_set, SIGTERM) != 0 ||
      sigaddset(&signal_set, SIGINT) != 0 || sigaddset(&signal_set, SIGHUP) != 0 ||
      sigaddset(&signal_set, SIGQUIT) != 0 || sigaddset(&signal_set, SIGUSR1) != 0 ||
      pthread_sigmask(SIG_BLOCK, &signal_set, NULL) != 0) {
    fprintf(stderr, "naive-plugin: cannot initialize signal handling\n");
    (void)RemoveProfile(runtime_root);
    return 1;
  }

  char* config = NULL;
  if (!ReadConfig(argv[1], &config)) {
    (void)RemoveProfile(runtime_root);
    return 2;
  }

  char profile_path[PATH_MAX + 1];
  int profile_count = snprintf(profile_path, sizeof(profile_path),
                               "%s/.naivefox-profile-%ld-XXXXXX",
                               working_directory, (long)getpid());
  if (profile_count < 0 || (size_t)profile_count >= sizeof(profile_path) ||
      !mkdtemp(profile_path) || chmod(profile_path, 0700) != 0) {
    fprintf(stderr, "naive-plugin: cannot create writable Gecko profile: %s\n",
            strerror(errno));
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }

  if (!PreloadAndroidSystemLibraries()) {
    (void)RemoveProfile(profile_path);
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }

  void* library = dlopen(libxul_path, RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    fprintf(stderr, "naive-plugin: cannot load libxul.so: %s\n", dlerror());
    (void)RemoveProfile(profile_path);
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }

  RunEmbeddedFunction run = NULL;
  RequestStopFunction request_stop = NULL;
  VersionFunction version = NULL;
  if (!ResolveFunctions(library, &run, &request_stop, &version)) {
    dlclose(library);
    (void)RemoveProfile(profile_path);
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }
  const char* version_text = version();
  if (!version_text || !*version_text) {
    fprintf(stderr, "naive-plugin: NaiveFoxVersion returned an empty value\n");
    dlclose(library);
    (void)RemoveProfile(profile_path);
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }
  fprintf(stderr, "naive-plugin: starting NaiveFox %s\n", version_text);

  StopThreadContext stop_context = {
      .request_stop = request_stop,
      .run_entered = ATOMIC_VAR_INIT(false),
      .finished = ATOMIC_VAR_INIT(false),
      .signal_set = signal_set,
  };
  pthread_t stop_thread;
  int thread_status = pthread_create(&stop_thread, NULL, StopThreadMain,
                                     &stop_context);
  if (thread_status != 0) {
    fprintf(stderr, "naive-plugin: cannot create stop thread: %s\n",
            strerror(thread_status));
    dlclose(library);
    (void)RemoveProfile(profile_path);
    (void)RemoveProfile(runtime_root);
    free(config);
    return 1;
  }

  atomic_store_explicit(&stop_context.run_entered, true, memory_order_release);
  int run_status = run(config, profile_path, runtime_path);
  atomic_store_explicit(&stop_context.finished, true, memory_order_release);
  (void)pthread_kill(stop_thread, SIGUSR1);
  (void)pthread_join(stop_thread, NULL);

  if (!RemoveProfile(profile_path)) {
    fprintf(stderr, "naive-plugin: warning: cannot remove temporary profile: %s\n",
            strerror(errno));
  }
  if (!RemoveProfile(runtime_root)) {
    fprintf(stderr, "naive-plugin: warning: cannot remove extracted runtime: %s\n",
            strerror(errno));
  }
  free(config);
  return MapStatusToExitCode(run_status);
}
