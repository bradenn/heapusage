/*
 * hulog.cpp
 *
 * Copyright (C) 2017 Kristofer Berggren
 * All rights reserved.
 * 
 * heapusage is distributed under the BSD 3-Clause license, see LICENSE for details.
 *
 */

/* ----------- Includes ------------------------------------------ */
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <cinttypes>
#include <libgen.h>
#include <pthread.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include "json.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

#include "hulog.h"

using json = nlohmann::json;

/* ----------- Defines ------------------------------------------- */
#define MAX_CALL_STACK 20   /* Limits the callstack depth to store */


/* ----------- Types --------------------------------------------- */
typedef struct hu_allocinfo_s {
    void *ptr;
    ssize_t size;
    void *callstack[MAX_CALL_STACK];
    int callstack_depth;
    int count;
} hu_allocinfo_t;


/* ----------- File Global Variables ----------------------------- */
static pid_t pid = 0;
static char *hu_log_file = NULL;
static int hu_log_free = 0;
static int hu_log_nosyms = 0;
static ssize_t hu_log_minleak = 0;

static int callcount = 0;
static int logging_enabled = 0;
static pthread_mutex_t callcount_lock = PTHREAD_MUTEX_INITIALIZER;
#ifdef __APPLE__
static pthread_mutex_t recursive_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#elif __linux__
static pthread_mutex_t recursive_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
#warning "Unsupported platform"
#endif

static unsigned long long allocinfo_total_frees = 0;
static unsigned long long allocinfo_total_allocs = 0;
static unsigned long long allocinfo_total_alloc_bytes = 0;
static unsigned long long allocinfo_current_alloc_bytes = 0;
static unsigned long long allocinfo_peak_alloc_bytes = 0;

static std::map<void *, hu_allocinfo_t> *allocations = nullptr;
static std::map<void *, std::string> *symbol_cache = nullptr;
static std::map<void *, std::string> *objfile_cache = nullptr;


/* ----------- Local Functions ----------------------------------- */
struct size_compare {
    bool
    operator()(const hu_allocinfo_t &lhs, const hu_allocinfo_t &rhs) const {
        return lhs.size < rhs.size;
    }
};

static std::string addr_to_symbol(void *addr);


/* ----------- Global Functions ---------------------------------- */
void log_init() {
    /* Get runtime info */
    hu_log_file = getenv("HU_FILE");
    hu_log_free = ((getenv("HU_FREE") != NULL) &&
                   (strcmp(getenv("HU_FREE"), "1") == 0));
    hu_log_nosyms = ((getenv("HU_NOSYMS") != NULL) &&
                     (strcmp(getenv("HU_NOSYMS"), "1") == 0));
    hu_log_minleak = getenv("HU_MINLEAK") ? strtoll(getenv("HU_MINLEAK"), NULL,
                                                    10) : 0;
    pid = getpid();

    /* Initial log output */
    if (hu_log_file) {
        FILE *f = fopen(hu_log_file, "w");
        if (f) {
        } else {
            fprintf(stderr,
                    "heapusage error: unable to open output file (%s) for writing\n",
                    hu_log_file);
        }
    } else {
        fprintf(stderr, "heapusage error: no output file specified\n");
    }

    allocations = new std::map<void *, hu_allocinfo_t>();
    symbol_cache = new std::map<void *, std::string>();
    objfile_cache = new std::map<void *, std::string>();
}

void log_enable(int flag) {
    logging_enabled = flag;
}

void
log_print_callstack(FILE *f, int callstack_depth, void *const callstack[],
                    json &j) {
    if (callstack_depth > 0) {
        int i = 1;
        j["trace"] = json::array();
        while (i < callstack_depth) {
            json o;
#if UINTPTR_MAX == 0xffffffff
            o["address"] = (unsigned int) callstack[i]
#else
            o["address"] = (unsigned long) callstack[i];
#endif

            std::string symbol = addr_to_symbol(callstack[i]);
            o["location"] = symbol.c_str();
            if(callstack_depth - 5 <= i){
                j["trace"].push_back(o);
            }


            ++i;
        }
    } else {
        fprintf(f, "    error: backtrace() returned empty callstack\n");
    }
}

bool log_is_valid_callstack(int callstack_depth, void *const callstack[],
                            bool is_alloc) {
    int i = callstack_depth - 1;
    std::string objfile;
    while (i > 0) {
        void *addr = callstack[i];

        auto it = objfile_cache->find(addr);
        if (it != objfile_cache->end()) {
            objfile = it->second;
        } else {
            Dl_info dlinfo;
            if (dladdr(addr, &dlinfo) && dlinfo.dli_fname) {
                char *fname = strdup(dlinfo.dli_fname);
                if (fname) {
                    objfile = std::string(basename(fname));
                    (*objfile_cache)[addr] = objfile;
                    free(fname);
                }
            }
        }

        if (!objfile.empty()) {
            // For now only care about originating object file
            break;
        }

        --i;
    }

    if (!objfile.empty()) {
        // ignore invalid dealloc from libobjc
        if (!is_alloc && (objfile == "libobjc.A.dylib")) return false;
    }

    return true;
}

void log_event(int event, void *ptr, size_t size) {
    if (logging_enabled) {
        int in_recursion = 0;
        pthread_mutex_lock(&recursive_lock);
        pthread_mutex_lock(&callcount_lock);
        if (callcount == 0) {
            ++callcount;
        } else {
            in_recursion = 1;
        }
        pthread_mutex_unlock(&callcount_lock);

        if (!in_recursion) {
            if (event == EVENT_MALLOC) {
                hu_allocinfo_t allocinfo;
                allocinfo.size = size;
                allocinfo.ptr = ptr;
                allocinfo.callstack_depth = backtrace(allocinfo.callstack,
                                                      MAX_CALL_STACK);
                allocinfo.count = 1;
                (*allocations)[ptr] = allocinfo;

                allocinfo_total_allocs += 1;
                allocinfo_total_alloc_bytes += size;
                allocinfo_current_alloc_bytes += size;

                if (allocinfo_current_alloc_bytes >
                    allocinfo_peak_alloc_bytes) {
                    allocinfo_peak_alloc_bytes = allocinfo_current_alloc_bytes;
                }
            } else if (event == EVENT_FREE) {
                std::map<void *, hu_allocinfo_t>::iterator allocation = allocations->find(
                        ptr);
                if (allocation != allocations->end()) {
                    allocinfo_current_alloc_bytes -= allocation->second.size;

                    allocations->erase(ptr);
                } else if (hu_log_free) {
                    void *callstack[MAX_CALL_STACK];
                    int callstack_depth = backtrace(callstack, MAX_CALL_STACK);
                    if (log_is_valid_callstack(callstack_depth, callstack,
                                               false)) {
                        FILE *f = fopen(hu_log_file, "a");
                        if (f) {
                            fprintf(f, " Invalid deallocation at:\n"
                            );

//                            log_print_callstack(f, callstack_depth,
//                                                callstack);

                            fprintf(f, "\n");

                            fclose(f);
                        }
                    }
                }

                allocinfo_total_frees += 1;
            }

            pthread_mutex_lock(&callcount_lock);
            --callcount;
            pthread_mutex_unlock(&callcount_lock);
        }

        pthread_mutex_unlock(&recursive_lock);
    }
}

void log_summary() {
    FILE *f = NULL;
    json j;
    if (hu_log_file) {
        f = fopen(hu_log_file, "a");
    }

    if (!f) {
        return;
    }

    unsigned long long leak_total_bytes = 0;
    unsigned long long leak_total_blocks = 0;

    /* Group results by callstack */
    static std::map<std::vector<void *>, hu_allocinfo_t> allocations_by_callstack;
    for (auto it = allocations->begin(); it != allocations->end(); ++it) {
        std::vector<void *> callstack;
        callstack.assign(it->second.callstack,
                         it->second.callstack + it->second.callstack_depth);

        auto callstack_it = allocations_by_callstack.find(callstack);
        if (callstack_it != allocations_by_callstack.end()) {
            callstack_it->second.count += 1;
            callstack_it->second.size += it->second.size;
        } else {
            allocations_by_callstack[callstack] = it->second;
        }

        leak_total_bytes += it->second.size;
        leak_total_blocks += 1;
    }

    /* Sort results by total allocation size */
    std::multiset<hu_allocinfo_t, size_compare> allocations_by_size;
    for (auto it = allocations_by_callstack.begin();
         it != allocations_by_callstack.end(); ++it) {
        allocations_by_size.insert(it->second);
    }

    /* Output heap summary */
    j["lost"] = {
            {"bytes",  leak_total_bytes},
            {"blocks", leak_total_blocks}
    };

    j["runtime"] = {
            {"allocs", allocinfo_total_allocs},
            {"frees",  allocinfo_total_frees},
            {"bytes",  allocinfo_total_alloc_bytes},
    };

    j["pid"] = pid;

    // c_str might destory j, but it shouldn't matter

    j["leaks"] = json::array();
    /* Output leak details */
    for (auto it = allocations_by_size.rbegin();
         (it != allocations_by_size.rend()) &&
         (it->size >= hu_log_minleak); ++it) {
        if (log_is_valid_callstack(it->callstack_depth, it->callstack, true)) {
            json obj = {{"bytes",  it->size},
                        {"blocks", it->count}};

            log_print_callstack(f, it->callstack_depth, it->callstack, obj);
            j["leaks"].push_back(obj);
        }
    }
    fprintf(f, "%s", j.dump(4).c_str());
    fclose(f);
}


/* ----------- Local Functions ----------------------------------- */
static std::string addr_to_symbol(void *addr) {
    std::string symbol;
    auto it = symbol_cache->find(addr);
    if (it != symbol_cache->end()) {
        symbol = it->second;
    } else {
        Dl_info dlinfo;
        if (dladdr(addr, &dlinfo) && dlinfo.dli_sname) {
            if (dlinfo.dli_sname[0] == '_') {
                int status = -1;
                char *demangled = NULL;
                demangled = abi::__cxa_demangle(dlinfo.dli_sname, NULL, 0,
                                                &status);
                if (demangled) {
                    if (status == 0) {
                        symbol = std::string(demangled);
                    }
                    free(demangled);
                }
            }

            if (symbol.empty()) {
                symbol = std::string(dlinfo.dli_sname);
            }


            if (!symbol.empty()) {
                symbol += std::string(":");
                symbol += std::string(std::to_string(
                        (char *) addr - (char *) dlinfo.dli_saddr));
            }
        }

        (*symbol_cache)[addr] = symbol;
    }

    return symbol;
}

