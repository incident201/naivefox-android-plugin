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

#ifndef NAIVEFOX_RUNTIME_RELATIVE_PATH
#  error "NAIVEFOX_RUNTIME_RELATIVE_PATH must be supplied by CMake"
#endif

#define CONFIG_MAXIMUM_BYTES (1024U * 1024U)
#define REEXEC_MARKER "NAIVEFOX_LAUNCHER_REEXEC_PATH"

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
  char runtime_path[PATH_MAX + 1];
  char libxul_path[PATH_MAX + 1];
  if (!ResolveSelf(executable, sizeof(executable), plugin_directory,
                   sizeof(plugin_directory)) ||
      !JoinPath(runtime_path, sizeof(runtime_path), plugin_directory,
                NAIVEFOX_RUNTIME_RELATIVE_PATH) ||
      !JoinPath(libxul_path, sizeof(libxul_path), runtime_path, "libxul.so")) {
    fprintf(stderr, "naive-plugin: cannot resolve relocated runtime paths\n");
    return 1;
  }

  struct stat runtime_status;
  struct stat libxul_status;
  if (stat(runtime_path, &runtime_status) != 0 ||
      !S_ISDIR(runtime_status.st_mode) ||
      stat(libxul_path, &libxul_status) != 0 ||
      !S_ISREG(libxul_status.st_mode)) {
    fprintf(stderr, "naive-plugin: relocated NaiveFox runtime is incomplete\n");
    return 1;
  }

  if (EnsureRuntimeLinkerEnvironment(argc, argv, executable, runtime_path) != 0) {
    return 1;
  }

  sigset_t signal_set;
  if (sigemptyset(&signal_set) != 0 || sigaddset(&signal_set, SIGTERM) != 0 ||
      sigaddset(&signal_set, SIGINT) != 0 || sigaddset(&signal_set, SIGHUP) != 0 ||
      sigaddset(&signal_set, SIGQUIT) != 0 || sigaddset(&signal_set, SIGUSR1) != 0 ||
      pthread_sigmask(SIG_BLOCK, &signal_set, NULL) != 0) {
    fprintf(stderr, "naive-plugin: cannot initialize signal handling\n");
    return 1;
  }

  char* config = NULL;
  if (!ReadConfig(argv[1], &config)) {
    return 2;
  }

  char profile_path[PATH_MAX + 1];
  int profile_count = snprintf(profile_path, sizeof(profile_path),
                               "%s/.naivefox-profile-%ld-XXXXXX",
                               plugin_directory, (long)getpid());
  if (profile_count < 0 || (size_t)profile_count >= sizeof(profile_path) ||
      !mkdtemp(profile_path) || chmod(profile_path, 0700) != 0) {
    fprintf(stderr, "naive-plugin: cannot create writable Gecko profile: %s\n",
            strerror(errno));
    free(config);
    return 1;
  }

  void* library = dlopen(libxul_path, RTLD_NOW | RTLD_GLOBAL);
  if (!library) {
    fprintf(stderr, "naive-plugin: cannot load libxul.so: %s\n", dlerror());
    (void)RemoveProfile(profile_path);
    free(config);
    return 1;
  }

  RunEmbeddedFunction run = NULL;
  RequestStopFunction request_stop = NULL;
  VersionFunction version = NULL;
  if (!ResolveFunctions(library, &run, &request_stop, &version)) {
    dlclose(library);
    (void)RemoveProfile(profile_path);
    free(config);
    return 1;
  }
  const char* version_text = version();
  if (!version_text || !*version_text) {
    fprintf(stderr, "naive-plugin: NaiveFoxVersion returned an empty value\n");
    dlclose(library);
    (void)RemoveProfile(profile_path);
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
  free(config);
  return MapStatusToExitCode(run_status);
}
