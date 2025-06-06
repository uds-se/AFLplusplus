/*
   american fuzzy lop++ - initialization related routines
   ------------------------------------------------------

   Originally written by Michal Zalewski

   Now maintained by by Marc Heuse <mh@mh-sec.de>,
                        Heiko Eißfeldt <heiko.eissfeldt@hexco.de> and
                        Andrea Fioraldi <andreafioraldi@gmail.com>

   Copyright 2016, 2017 Google Inc. All rights reserved.
   Copyright 2019 AFLplusplus Project. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This is the real deal: the program takes an instrumented binary and
   attempts a variety of basic fuzzing tricks, paying close attention to
   how they affect the execution path.

 */

#include "afl-fuzz.h"

#ifdef HAVE_AFFINITY

/* Build a list of processes bound to specific cores. Returns -1 if nothing
   can be found. Assumes an upper bound of 4k CPUs. */

void bind_to_free_cpu(void) {

#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
  cpu_set_t c;
#elif defined(__NetBSD__)
  cpuset_t*          c;
#endif

  u8  cpu_used[4096] = {0};
  u32 i;

  if (cpu_core_count < 2) return;

  if (getenv("AFL_NO_AFFINITY")) {

    WARNF("Not binding to a CPU core (AFL_NO_AFFINITY set).");
    return;

  }

#if defined(__linux__)
  DIR*           d;
  struct dirent* de;
  d = opendir("/proc");

  if (!d) {

    WARNF("Unable to access /proc - can't scan for free CPU cores.");
    return;

  }

  ACTF("Checking CPU core loadout...");

  /* Introduce some jitter, in case multiple AFL tasks are doing the same
     thing at the same time... */

  usleep(R(1000) * 250);

  /* Scan all /proc/<pid>/status entries, checking for Cpus_allowed_list.
     Flag all processes bound to a specific CPU using cpu_used[]. This will
     fail for some exotic binding setups, but is likely good enough in almost
     all real-world use cases. */

  while ((de = readdir(d))) {

    u8*   fn;
    FILE* f;
    u8    tmp[MAX_LINE];
    u8    has_vmsize = 0;

    if (!isdigit(de->d_name[0])) continue;

    fn = alloc_printf("/proc/%s/status", de->d_name);

    if (!(f = fopen(fn, "r"))) {

      ck_free(fn);
      continue;

    }

    while (fgets(tmp, MAX_LINE, f)) {

      u32 hval;

      /* Processes without VmSize are probably kernel tasks. */

      if (!strncmp(tmp, "VmSize:\t", 8)) has_vmsize = 1;

      if (!strncmp(tmp, "Cpus_allowed_list:\t", 19) && !strchr(tmp, '-') &&
          !strchr(tmp, ',') && sscanf(tmp + 19, "%u", &hval) == 1 &&
          hval < sizeof(cpu_used) && has_vmsize) {

        cpu_used[hval] = 1;
        break;

      }

    }

    ck_free(fn);
    fclose(f);

  }

  closedir(d);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
  struct kinfo_proc* procs;
  size_t             nprocs;
  size_t             proccount;
  int                s_name[] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL};
  size_t             s_name_l = sizeof(s_name) / sizeof(s_name[0]);

  if (sysctl(s_name, s_name_l, NULL, &nprocs, NULL, 0) != 0) return;
  proccount = nprocs / sizeof(*procs);
  nprocs = nprocs * 4 / 3;

  procs = ck_alloc(nprocs);
  if (sysctl(s_name, s_name_l, procs, &nprocs, NULL, 0) != 0) {

    ck_free(procs);
    return;

  }

  for (i = 0; i < proccount; i++) {

#if defined(__FreeBSD__)
    if (procs[i].ki_oncpu < sizeof(cpu_used) && procs[i].ki_pctcpu > 60)
      cpu_used[procs[i].ki_oncpu] = 1;
#elif defined(__DragonFly__)
    if (procs[i].kp_lwp.kl_cpuid < sizeof(cpu_used) &&
        procs[i].kp_lwp.kl_pctcpu > 10)
      cpu_used[procs[i].kp_lwp.kl_cpuid] = 1;
#endif

  }

  ck_free(procs);
#elif defined(__NetBSD__)
  struct kinfo_proc2* procs;
  size_t              nprocs;
  size_t              proccount;
  int                 s_name[] = {

      CTL_KERN, KERN_PROC2, KERN_PROC_ALL, 0, sizeof(struct kinfo_proc2), 0};
  size_t s_name_l = sizeof(s_name) / sizeof(s_name[0]);

  if (sysctl(s_name, s_name_l, NULL, &nprocs, NULL, 0) != 0) return;
  proccount = nprocs / sizeof(struct kinfo_proc2);
  procs = ck_alloc(nprocs * sizeof(struct kinfo_proc2));
  s_name[5] = proccount;

  if (sysctl(s_name, s_name_l, procs, &nprocs, NULL, 0) != 0) {

    ck_free(procs);
    return;

  }

  for (i = 0; i < proccount; i++) {

    if (procs[i].p_cpuid < sizeof(cpu_used) && procs[i].p_pctcpu > 0)
      cpu_used[procs[i].p_cpuid] = 1;

  }

  ck_free(procs);
#else
#warning \
    "For this platform we do not have free CPU binding code yet. If possible, please supply a PR to https://github.com/vanhauser-thc/AFLplusplus"
#endif

  for (i = 0; i < cpu_core_count; ++i)
    if (!cpu_used[i]) break;

  if (i == cpu_core_count) {

    SAYF("\n" cLRD "[-] " cRST
         "Uh-oh, looks like all %d CPU cores on your system are allocated to\n"
         "    other instances of afl-fuzz (or similar CPU-locked tasks). "
         "Starting\n"
         "    another fuzzer on this machine is probably a bad plan, but if "
         "you are\n"
         "    absolutely sure, you can set AFL_NO_AFFINITY and try again.\n",
         cpu_core_count);

    FATAL("No more free CPU cores");

  }

  OKF("Found a free CPU core, binding to #%u.", i);

  cpu_aff = i;

#if defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
  CPU_ZERO(&c);
  CPU_SET(i, &c);
#elif defined(__NetBSD__)
  c = cpuset_create();
  if (c == NULL) PFATAL("cpuset_create failed");

  cpuset_set(i, c);
#endif

#if defined(__linux__)
  if (sched_setaffinity(0, sizeof(c), &c)) PFATAL("sched_setaffinity failed");
#elif defined(__FreeBSD__) || defined(__DragonFly__)
  if (pthread_setaffinity_np(pthread_self(), sizeof(c), &c))
    PFATAL("pthread_setaffinity failed");
#elif defined(__NetBSD__)
  if (pthread_setaffinity_np(pthread_self(), cpuset_size(c), c))
    PFATAL("pthread_setaffinity failed");

  cpuset_destroy(c);
#else
  // this will need something for other platforms
#endif

}

#endif                                                     /* HAVE_AFFINITY */

/* Load postprocessor, if available. */

void setup_post(void) {

  void* dh;
  u8*   fn = getenv("AFL_POST_LIBRARY");
  u32   tlen = 6;

  if (!fn) return;

  ACTF("Loading postprocessor from '%s'...", fn);

  dh = dlopen(fn, RTLD_NOW);
  if (!dh) FATAL("%s", dlerror());

  post_handler = dlsym(dh, "afl_postprocess");
  if (!post_handler) FATAL("Symbol 'afl_postprocess' not found.");

  /* Do a quick test. It's better to segfault now than later =) */

  post_handler("hello", &tlen);

  OKF("Postprocessor installed successfully.");

}

void setup_custom_mutator(void) {

  void* dh;
  u8*   fn = getenv("AFL_CUSTOM_MUTATOR_LIBRARY");

  if (!fn) return;

  ACTF("Loading custom mutator library from '%s'...", fn);

  dh = dlopen(fn, RTLD_NOW);
  if (!dh) FATAL("%s", dlerror());

  custom_mutator = dlsym(dh, "afl_custom_mutator");
  //  if (!custom_mutator) FATAL("Symbol 'afl_custom_mutator' not found.");

  if (getenv("FF_STACKING") && !getenv("AFL_FFGEN") && !getenv("BLACKBOX_FFMUT") && !getenv("BLACKBOX_FFGEN"))
    stacking_mutation_mode = 1;
  if (getenv("AFL_FFGEN")) {
    pre_save_handler = dlsym(dh, "ff_generate");
    //  if (!pre_save_handler) WARNF("Symbol 'ff_generate' not found.");

    post_load_handler = dlsym(dh, "ff_parse");
    //  if (!post_load_handler) WARNF("Symbol 'ff_parse' not found.");
  } else {
    if (!getenv("BLACKBOX_FFGEN"))
      process_file = dlsym(dh, "process_file");
    one_smart_mutation = dlsym(dh, "one_smart_mutation");
    generate_random_file = dlsym(dh, "generate_random_file");
    mutation_infop = dlsym(dh, "mutation_info");
    printf("mutation_infop %p\n", mutation_infop);
  }
  OKF("Custom mutator installed successfully.");

}

/* Shuffle an array of pointers. Might be slightly biased. */

static void shuffle_ptrs(void** ptrs, u32 cnt) {

  u32 i;

  for (i = 0; i < cnt - 2; ++i) {

    u32   j = i + UR(cnt - i);
    void* s = ptrs[i];
    ptrs[i] = ptrs[j];
    ptrs[j] = s;

  }

}

u8* get_file_name(u8* fname) {
  u8* tmp = alloc_printf("%s/dec_queue/%s", out_dir, strrchr(fname, '/') + 1);
  return tmp;
}


/* Read all testcases from the input directory, then queue them for testing.
   Called at startup. */

void read_testcases(void) {

  struct dirent** nl;
  s32             nl_cnt;
  u32             i;
  u8*             fn1;

  /* Auto-detect non-in-place resumption attempts. */

  fn1 = alloc_printf("%s/queue", in_dir);
  if (!access(fn1, F_OK))
    in_dir = fn1;
  else
    ck_free(fn1);

  ACTF("Scanning '%s'...", in_dir);

  /* We use scandir() + alphasort() rather than readdir() because otherwise,
     the ordering  of test cases would vary somewhat randomly and would be
     difficult to control. */

  nl_cnt = scandir(in_dir, &nl, NULL, alphasort);

  if (nl_cnt < 0) {

    if (errno == ENOENT || errno == ENOTDIR)

      SAYF("\n" cLRD "[-] " cRST
           "The input directory does not seem to be valid - try again. The "
           "fuzzer needs\n"
           "    one or more test case to start with - ideally, a small file "
           "under 1 kB\n"
           "    or so. The cases must be stored as regular files directly in "
           "the input\n"
           "    directory.\n");

    PFATAL("Unable to open '%s'", in_dir);

  }

  if (shuffle_queue && nl_cnt > 1) {

    ACTF("Shuffling queue...");
    shuffle_ptrs((void**)nl, nl_cnt);

  }

  for (i = 0; i < nl_cnt; ++i) {

    struct stat st;

    u8* fn2 = alloc_printf("%s/%s", in_dir, nl[i]->d_name);
    u8* dfn =
        alloc_printf("%s/.state/deterministic_done/%s", in_dir, nl[i]->d_name);

    u8 passed_det = 0;

    free(nl[i]);                                             /* not tracked */

    if (lstat(fn2, &st) || access(fn2, R_OK))
      PFATAL("Unable to access '%s'", fn2);

    /* This also takes care of . and .. */

    if (!S_ISREG(st.st_mode) || !st.st_size || strstr(fn2, "/README.txt")) {

      ck_free(fn2);
      ck_free(dfn);
      continue;

    }

    if (st.st_size > MAX_FILE)
      FATAL("Test case '%s' is too big (%s, limit is %s)", fn2, DMS(st.st_size),
            DMS(MAX_FILE));

    /* Check for metadata that indicates that deterministic fuzzing
       is complete for this entry. We don't want to repeat deterministic
       fuzzing when resuming aborted scans, because it would be pointless
       and probably very time-consuming. */

    if (!access(dfn, F_OK)) passed_det = 1;
    ck_free(dfn);

    add_to_queue(fn2, st.st_size, passed_det);

    if (process_file) {
      queue_top->validity = process_file(fn2, get_file_name(fn2));
      queue_top->index = queued_parsed++;
    }

  }

  free(nl);                                                  /* not tracked */

  if (!queued_paths) {

    SAYF("\n" cLRD "[-] " cRST
         "Looks like there are no valid test cases in the input directory! The "
         "fuzzer\n"
         "    needs one or more test case to start with - ideally, a small "
         "file under\n"
         "    1 kB or so. The cases must be stored as regular files directly "
         "in the\n"
         "    input directory.\n");

    FATAL("No usable test cases in '%s'", in_dir);

  }

  last_path_time = 0;
  queued_at_start = queued_paths;

}

/* Examine map coverage. Called once, for first test case. */

static void check_map_coverage(void) {

  u32 i;

  if (count_bytes(trace_bits) < 100) return;

  for (i = (1 << (MAP_SIZE_POW2 - 1)); i < MAP_SIZE; ++i)
    if (trace_bits[i]) return;

  WARNF("Recompile binary with newer version of afl to improve coverage!");

}

/* Perform dry run of all test cases to confirm that the app is working as
   expected. This is done only for the initial inputs, and only once. */

void perform_dry_run(char** argv) {

  struct queue_entry* q = queue;
  u32                 cal_failures = 0;
  u8*                 skip_crashes = getenv("AFL_SKIP_CRASHES");

  while (q) {

    u8* use_mem;
    u8  res;
    s32 fd;

    u8* fn = strrchr(q->fname, '/') + 1;

    ACTF("Attempting dry run with '%s'...", fn);

    fd = open(q->fname, O_RDONLY);
    if (fd < 0) PFATAL("Unable to open '%s'", q->fname);

    use_mem = ck_alloc_nozero(q->len);

    if (read(fd, use_mem, q->len) != q->len)
      FATAL("Short read from '%s'", q->fname);

    close(fd);

    if (pre_save_handler){
      u8* gen_fn = get_gen_name(q->fname, "/queue/");
      fd = open(gen_fn, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (fd < 0) PFATAL("Unable to create '%s'", gen_fn);
      u8*    new_data;
      size_t new_size = pre_save_handler(use_mem, q->len, &new_data);
      ck_write(fd, new_data, new_size, gen_fn);
      close(fd);
      ck_free(gen_fn);
    }


    res = calibrate_case(argv, q, use_mem, 0, 1);
    ck_free(use_mem);

    if (stop_soon) return;

    if (res == crash_mode || res == FAULT_NOBITS)
      SAYF(cGRA "    len = %u, map size = %u, exec speed = %llu us\n" cRST,
           q->len, q->bitmap_size, q->exec_us);

    switch (res) {

      case FAULT_NONE:

        if (q == queue) check_map_coverage();

        if (crash_mode) FATAL("Test case '%s' does *NOT* crash", fn);

        break;

      case FAULT_TMOUT:

        if (timeout_given) {

          /* The -t nn+ syntax in the command line sets timeout_given to '2' and
             instructs afl-fuzz to tolerate but skip queue entries that time
             out. */

          if (timeout_given > 1) {

            WARNF("Test case results in a timeout (skipping)");
            q->cal_failed = CAL_CHANCES;
            ++cal_failures;
            break;

          }

          SAYF("\n" cLRD "[-] " cRST
               "The program took more than %u ms to process one of the initial "
               "test cases.\n"
               "    Usually, the right thing to do is to relax the -t option - "
               "or to delete it\n"
               "    altogether and allow the fuzzer to auto-calibrate. That "
               "said, if you know\n"
               "    what you are doing and want to simply skip the unruly test "
               "cases, append\n"
               "    '+' at the end of the value passed to -t ('-t %u+').\n",
               exec_tmout, exec_tmout);

          FATAL("Test case '%s' results in a timeout", fn);

        } else {

          SAYF("\n" cLRD "[-] " cRST
               "The program took more than %u ms to process one of the initial "
               "test cases.\n"
               "    This is bad news; raising the limit with the -t option is "
               "possible, but\n"
               "    will probably make the fuzzing process extremely slow.\n\n"

               "    If this test case is just a fluke, the other option is to "
               "just avoid it\n"
               "    altogether, and find one that is less of a CPU hog.\n",
               exec_tmout);

          FATAL("Test case '%s' results in a timeout", fn);

        }

      case FAULT_CRASH:

        if (crash_mode) break;

        if (skip_crashes) {

          WARNF("Test case results in a crash (skipping)");
          q->cal_failed = CAL_CHANCES;
          ++cal_failures;
          break;

        }

        if (mem_limit) {

          SAYF("\n" cLRD "[-] " cRST
               "Oops, the program crashed with one of the test cases provided. "
               "There are\n"
               "    several possible explanations:\n\n"

               "    - The test case causes known crashes under normal working "
               "conditions. If\n"
               "      so, please remove it. The fuzzer should be seeded with "
               "interesting\n"
               "      inputs - but not ones that cause an outright crash.\n\n"

               "    - The current memory limit (%s) is too low for this "
               "program, causing\n"
               "      it to die due to OOM when parsing valid files. To fix "
               "this, try\n"
               "      bumping it up with the -m setting in the command line. "
               "If in doubt,\n"
               "      try something along the lines of:\n\n"

               MSG_ULIMIT_USAGE
               " /path/to/binary [...] <testcase )\n\n"

               "      Tip: you can use http://jwilk.net/software/recidivm to "
               "quickly\n"
               "      estimate the required amount of virtual memory for the "
               "binary. Also,\n"
               "      if you are using ASAN, see %s/notes_for_asan.txt.\n\n"

               MSG_FORK_ON_APPLE

               "    - Least likely, there is a horrible bug in the fuzzer. If "
               "other options\n"
               "      fail, poke <afl-users@googlegroups.com> for "
               "troubleshooting tips.\n",
               DMS(mem_limit << 20), mem_limit - 1, doc_path);

        } else {

          SAYF("\n" cLRD "[-] " cRST
               "Oops, the program crashed with one of the test cases provided. "
               "There are\n"
               "    several possible explanations:\n\n"

               "    - The test case causes known crashes under normal working "
               "conditions. If\n"
               "      so, please remove it. The fuzzer should be seeded with "
               "interesting\n"
               "      inputs - but not ones that cause an outright crash.\n\n"

               MSG_FORK_ON_APPLE

               "    - Least likely, there is a horrible bug in the fuzzer. If "
               "other options\n"
               "      fail, poke <afl-users@googlegroups.com> for "
               "troubleshooting tips.\n");

        }

#undef MSG_ULIMIT_USAGE
#undef MSG_FORK_ON_APPLE

        FATAL("Test case '%s' results in a crash", fn);

      case FAULT_ERROR:

        FATAL("Unable to execute target application ('%s')", argv[0]);

      case FAULT_NOINST: FATAL("No instrumentation detected");

      case FAULT_NOBITS:

        ++useless_at_start;

        if (!in_bitmap && !shuffle_queue)
          WARNF("No new instrumentation output, test case may be useless.");

        break;

    }

    if (q->var_behavior) WARNF("Instrumentation output varies across runs.");

    q = q->next;

  }

  if (cal_failures) {

    if (cal_failures == queued_paths)
      FATAL("All test cases time out%s, giving up!",
            skip_crashes ? " or crash" : "");

    WARNF("Skipped %u test cases (%0.02f%%) due to timeouts%s.", cal_failures,
          ((double)cal_failures) * 100 / queued_paths,
          skip_crashes ? " or crashes" : "");

    if (cal_failures * 5 > queued_paths)
      WARNF(cLRD "High percentage of rejected test cases, check settings!");

  }

  OKF("All test cases processed.");

}

/* Helper function: link() if possible, copy otherwise. */

static void link_or_copy(u8* old_path, u8* new_path) {

  s32 i = link(old_path, new_path);
  s32 sfd, dfd;
  u8* tmp;

  if (!i) return;

  sfd = open(old_path, O_RDONLY);
  if (sfd < 0) PFATAL("Unable to open '%s'", old_path);

  dfd = open(new_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (dfd < 0) PFATAL("Unable to create '%s'", new_path);

  tmp = ck_alloc(64 * 1024);

  while ((i = read(sfd, tmp, 64 * 1024)) > 0)
    ck_write(dfd, tmp, i, new_path);

  if (i < 0) PFATAL("read() failed");

  ck_free(tmp);
  close(sfd);
  close(dfd);

}

/* Create hard links for input test cases in the output directory, choosing
   good names and pivoting accordingly. */

void pivot_inputs(void) {

  struct queue_entry* q = queue;
  u32                 id = 0;

  ACTF("Creating hard links for all input files...");

  while (q) {

    u8 *nfn, *rsl = strrchr(q->fname, '/');
    u32 orig_id;

    if (!rsl)
      rsl = q->fname;
    else
      ++rsl;

    /* If the original file name conforms to the syntax and the recorded
       ID matches the one we'd assign, just use the original file name.
       This is valuable for resuming fuzzing runs. */

    if (!strncmp(rsl, CASE_PREFIX, 3) &&
        sscanf(rsl + 3, "%06u", &orig_id) == 1 && orig_id == id) {

      u8* src_str;
      u32 src_id;

      resuming_fuzz = 1;
      nfn = alloc_printf("%s/queue/%s", out_dir, rsl);

      /* Since we're at it, let's also try to find parent and figure out the
         appropriate depth for this entry. */

      src_str = strchr(rsl + 3, ':');

      if (src_str && sscanf(src_str + 1, "%06u", &src_id) == 1) {

        struct queue_entry* s = queue;
        while (src_id-- && s)
          s = s->next;
        if (s) q->depth = s->depth + 1;

        if (max_depth < q->depth) max_depth = q->depth;

      }

    } else {

      /* No dice - invent a new name, capturing the original one as a
         substring. */

#ifndef SIMPLE_FILES

      u8* use_name = strstr(rsl, ",orig:");

      if (use_name)
        use_name += 6;
      else
        use_name = rsl;
      nfn = alloc_printf("%s/queue/id:%06u,time:0,orig:%s", out_dir, id,
                         use_name);

#else

      nfn = alloc_printf("%s/queue/id_%06u", out_dir, id);

#endif                                                    /* ^!SIMPLE_FILES */

    }

    /* Pivot to the new queue entry. */

    link_or_copy(q->fname, nfn);
    ck_free(q->fname);
    q->fname = nfn;

    /* Make sure that the passed_det value carries over, too. */

    if (q->passed_det) mark_as_det_done(q);

    q = q->next;
    ++id;

  }

  if (in_place_resume) nuke_resume_dir();

}

/* When resuming, try to find the queue position to start from. This makes sense
   only when resuming, and when we can find the original fuzzer_stats. */

u32 find_start_position(void) {

  static u8 tmp[4096];                   /* Ought to be enough for anybody. */

  u8 *fn, *off;
  s32 fd, i;
  u32 ret;

  if (!resuming_fuzz) return 0;

  if (in_place_resume)
    fn = alloc_printf("%s/fuzzer_stats", out_dir);
  else
    fn = alloc_printf("%s/../fuzzer_stats", in_dir);

  fd = open(fn, O_RDONLY);
  ck_free(fn);

  if (fd < 0) return 0;

  i = read(fd, tmp, sizeof(tmp) - 1);
  (void)i;                                                 /* Ignore errors */
  close(fd);

  off = strstr(tmp, "cur_path          : ");
  if (!off) return 0;

  ret = atoi(off + 20);
  if (ret >= queued_paths) ret = 0;
  return ret;

}

/* The same, but for timeouts. The idea is that when resuming sessions without
   -t given, we don't want to keep auto-scaling the timeout over and over
   again to prevent it from growing due to random flukes. */

void find_timeout(void) {

  static u8 tmp[4096];                   /* Ought to be enough for anybody. */

  u8 *fn, *off;
  s32 fd, i;
  u32 ret;

  if (!resuming_fuzz) return;

  if (in_place_resume)
    fn = alloc_printf("%s/fuzzer_stats", out_dir);
  else
    fn = alloc_printf("%s/../fuzzer_stats", in_dir);

  fd = open(fn, O_RDONLY);
  ck_free(fn);

  if (fd < 0) return;

  i = read(fd, tmp, sizeof(tmp) - 1);
  (void)i;                                                 /* Ignore errors */
  close(fd);

  off = strstr(tmp, "exec_timeout      : ");
  if (!off) return;

  ret = atoi(off + 20);
  if (ret <= 4) return;

  exec_tmout = ret;
  timeout_given = 3;

}

/* A helper function for maybe_delete_out_dir(), deleting all prefixed
   files in a directory. */

static u8 delete_files(u8* path, u8* prefix) {

  DIR*           d;
  struct dirent* d_ent;

  d = opendir(path);

  if (!d) return 0;

  while ((d_ent = readdir(d))) {

    if (d_ent->d_name[0] != '.' &&
        (!prefix || !strncmp(d_ent->d_name, prefix, strlen(prefix)))) {

      u8* fname = alloc_printf("%s/%s", path, d_ent->d_name);
      if (unlink(fname)) PFATAL("Unable to delete '%s'", fname);
      ck_free(fname);

    }

  }

  closedir(d);

  return !!rmdir(path);

}

/* Get the number of runnable processes, with some simple smoothing. */

double get_runnable_processes(void) {

  static double res;

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__NetBSD__) || defined(__DragonFly__)

  /* I don't see any portable sysctl or so that would quickly give us the
     number of runnable processes; the 1-minute load average can be a
     semi-decent approximation, though. */

  if (getloadavg(&res, 1) != 1) return 0;

#else

  /* On Linux, /proc/stat is probably the best way; load averages are
     computed in funny ways and sometimes don't reflect extremely short-lived
     processes well. */

  FILE* f = fopen("/proc/stat", "r");
  u8 tmp[1024];
  u32 val = 0;

  if (!f) return 0;

  while (fgets(tmp, sizeof(tmp), f)) {

    if (!strncmp(tmp, "procs_running ", 14) ||
        !strncmp(tmp, "procs_blocked ", 14))
      val += atoi(tmp + 14);

  }

  fclose(f);

  if (!res) {

    res = val;

  } else {

    res = res * (1.0 - 1.0 / AVG_SMOOTHING) +
          ((double)val) * (1.0 / AVG_SMOOTHING);

  }

#endif          /* ^(__APPLE__ || __FreeBSD__ || __OpenBSD__ || __NetBSD__) */

  return res;

}

/* Delete the temporary directory used for in-place session resume. */

void nuke_resume_dir(void) {

  u8* fn;

  fn = alloc_printf("%s/_resume/.state/deterministic_done", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/_resume/.state/auto_extras", out_dir);
  if (delete_files(fn, "auto_")) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/_resume/.state/redundant_edges", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/_resume/.state/variable_behavior", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/_resume/.state", out_dir);
  if (rmdir(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/_resume", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  return;

dir_cleanup_failed:

  FATAL("_resume directory cleanup failed");

}

/* Delete fuzzer output directory if we recognize it as ours, if the fuzzer
   is not currently running, and if the last run time isn't too great. */

void maybe_delete_out_dir(void) {

  FILE* f;
  u8*   fn = alloc_printf("%s/fuzzer_stats", out_dir);

  /* See if the output directory is locked. If yes, bail out. If not,
     create a lock that will persist for the lifetime of the process
     (this requires leaving the descriptor open).*/

  out_dir_fd = open(out_dir, O_RDONLY);
  if (out_dir_fd < 0) PFATAL("Unable to open '%s'", out_dir);

#ifndef __sun

  if (flock(out_dir_fd, LOCK_EX | LOCK_NB) && errno == EWOULDBLOCK) {

    SAYF("\n" cLRD "[-] " cRST
         "Looks like the job output directory is being actively used by "
         "another\n"
         "    instance of afl-fuzz. You will need to choose a different %s\n"
         "    or stop the other process first.\n",
         sync_id ? "fuzzer ID" : "output location");

    FATAL("Directory '%s' is in use", out_dir);

  }

#endif                                                            /* !__sun */

  f = fopen(fn, "r");

  if (f) {

    u64 start_time2, last_update;

    if (fscanf(f,
               "start_time     : %llu\n"
               "last_update    : %llu\n",
               &start_time2, &last_update) != 2)
      FATAL("Malformed data in '%s'", fn);

    fclose(f);

    /* Let's see how much work is at stake. */

    if (!in_place_resume && last_update - start_time2 > OUTPUT_GRACE * 60) {

      SAYF("\n" cLRD "[-] " cRST
           "The job output directory already exists and contains the results "
           "of more\n"
           "    than %d minutes worth of fuzzing. To avoid data loss, afl-fuzz "
           "will *NOT*\n"
           "    automatically delete this data for you.\n\n"

           "    If you wish to start a new session, remove or rename the "
           "directory manually,\n"
           "    or specify a different output location for this job. To resume "
           "the old\n"
           "    session, put '-' as the input directory in the command line "
           "('-i -') and\n"
           "    try again.\n",
           OUTPUT_GRACE);

      FATAL("At-risk data found in '%s'", out_dir);

    }

  }

  ck_free(fn);

  /* The idea for in-place resume is pretty simple: we temporarily move the old
     queue/ to a new location that gets deleted once import to the new queue/
     is finished. If _resume/ already exists, the current queue/ may be
     incomplete due to an earlier abort, so we want to use the old _resume/
     dir instead, and we let rename() fail silently. */

  if (in_place_resume) {

    u8* orig_q = alloc_printf("%s/queue", out_dir);

    in_dir = alloc_printf("%s/_resume", out_dir);

    rename(orig_q, in_dir);                                /* Ignore errors */

    OKF("Output directory exists, will attempt session resume.");

    ck_free(orig_q);

  } else {

    OKF("Output directory exists but deemed OK to reuse.");

  }

  ACTF("Deleting old session data...");

  /* Okay, let's get the ball rolling! First, we need to get rid of the entries
     in <out_dir>/.synced/.../id:*, if any are present. */

  if (!in_place_resume) {

    fn = alloc_printf("%s/.synced", out_dir);
    if (delete_files(fn, NULL)) goto dir_cleanup_failed;
    ck_free(fn);

  }

  /* Next, we need to clean up <out_dir>/queue/.state/ subdirectories: */

  fn = alloc_printf("%s/queue/.state/deterministic_done", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/queue/.state/auto_extras", out_dir);
  if (delete_files(fn, "auto_")) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/queue/.state/redundant_edges", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/queue/.state/variable_behavior", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  /* Then, get rid of the .state subdirectory itself (should be empty by now)
     and everything matching <out_dir>/queue/id:*. */

  fn = alloc_printf("%s/queue/.state", out_dir);
  if (rmdir(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/queue", out_dir);
  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  if (pre_save_handler) {
    fn = alloc_printf("%s/gen_queue", out_dir);
    if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
    ck_free(fn);
  }

  if (one_smart_mutation) {
    fn = alloc_printf("%s/dec_queue", out_dir);
    if (delete_files(fn, NULL)) goto dir_cleanup_failed;
    ck_free(fn);
  }

  /* All right, let's do <out_dir>/crashes/id:* and <out_dir>/hangs/id:*. */

  if (!in_place_resume) {

    fn = alloc_printf("%s/crashes/README.txt", out_dir);
    unlink(fn);                                            /* Ignore errors */
    ck_free(fn);

  }

  fn = alloc_printf("%s/crashes", out_dir);

  /* Make backup of the crashes directory if it's not empty and if we're
     doing in-place resume. */

  if (in_place_resume && rmdir(fn)) {

    time_t     cur_t = time(0);
    struct tm* t = localtime(&cur_t);

#ifndef SIMPLE_FILES

    u8* nfn = alloc_printf("%s.%04d-%02d-%02d-%02d:%02d:%02d", fn,
                           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                           t->tm_hour, t->tm_min, t->tm_sec);

#else

    u8* nfn = alloc_printf("%s_%04d%02d%02d%02d%02d%02d", fn, t->tm_year + 1900,
                           t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
                           t->tm_sec);

#endif                                                    /* ^!SIMPLE_FILES */

    rename(fn, nfn);                                      /* Ignore errors. */
    ck_free(nfn);

  }

  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/gen_crashes", out_dir);

  /* Make backup of the crashes directory if it's not empty and if we're
     doing in-place resume. */

  if (in_place_resume && rmdir(fn)) {

    time_t     cur_t = time(0);
    struct tm* t = localtime(&cur_t);

#ifndef SIMPLE_FILES

    u8* nfn = alloc_printf("%s.%04d-%02d-%02d-%02d:%02d:%02d", fn,
                           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                           t->tm_hour, t->tm_min, t->tm_sec);

#else

    u8* nfn = alloc_printf("%s_%04d%02d%02d%02d%02d%02d", fn, t->tm_year + 1900,
                           t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
                           t->tm_sec);

#endif                                                    /* ^!SIMPLE_FILES */

    rename(fn, nfn);                                      /* Ignore errors. */
    ck_free(nfn);

  }

  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/hangs", out_dir);

  /* Backup hangs, too. */

  if (in_place_resume && rmdir(fn)) {

    time_t     cur_t = time(0);
    struct tm* t = localtime(&cur_t);

#ifndef SIMPLE_FILES

    u8* nfn = alloc_printf("%s.%04d-%02d-%02d-%02d:%02d:%02d", fn,
                           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                           t->tm_hour, t->tm_min, t->tm_sec);

#else

    u8* nfn = alloc_printf("%s_%04d%02d%02d%02d%02d%02d", fn, t->tm_year + 1900,
                           t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
                           t->tm_sec);

#endif                                                    /* ^!SIMPLE_FILES */

    rename(fn, nfn);                                      /* Ignore errors. */
    ck_free(nfn);

  }

  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/gen_hangs", out_dir);

  /* Backup hangs, too. */

  if (in_place_resume && rmdir(fn)) {

    time_t     cur_t = time(0);
    struct tm* t = localtime(&cur_t);

#ifndef SIMPLE_FILES

    u8* nfn = alloc_printf("%s.%04d-%02d-%02d-%02d:%02d:%02d", fn,
                           t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                           t->tm_hour, t->tm_min, t->tm_sec);

#else

    u8* nfn = alloc_printf("%s_%04d%02d%02d%02d%02d%02d", fn, t->tm_year + 1900,
                           t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min,
                           t->tm_sec);

#endif                                                    /* ^!SIMPLE_FILES */

    rename(fn, nfn);                                      /* Ignore errors. */
    ck_free(nfn);

  }

  if (delete_files(fn, CASE_PREFIX)) goto dir_cleanup_failed;
  ck_free(fn);

  /* And now, for some finishing touches. */

  if (file_extension) {

    fn = alloc_printf("%s/.cur_input.%s", out_dir, file_extension);

  } else {

    fn = alloc_printf("%s/.cur_input", out_dir);

  }

  if (unlink(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/fuzz_bitmap", out_dir);
  if (unlink(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  if (!in_place_resume) {

    fn = alloc_printf("%s/fuzzer_stats", out_dir);
    if (unlink(fn) && errno != ENOENT) goto dir_cleanup_failed;
    ck_free(fn);

  }

  fn = alloc_printf("%s/plot_data", out_dir);
  if (unlink(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  fn = alloc_printf("%s/cmdline", out_dir);
  if (unlink(fn) && errno != ENOENT) goto dir_cleanup_failed;
  ck_free(fn);

  OKF("Output dir cleanup successful.");

  /* Wow... is that all? If yes, celebrate! */

  return;

dir_cleanup_failed:

  SAYF("\n" cLRD "[-] " cRST
       "Whoops, the fuzzer tried to reuse your output directory, but bumped "
       "into\n"
       "    some files that shouldn't be there or that couldn't be removed - "
       "so it\n"
       "    decided to abort! This happened while processing this path:\n\n"

       "    %s\n\n"
       "    Please examine and manually delete the files, or specify a "
       "different\n"
       "    output location for the tool.\n",
       fn);

  FATAL("Output directory cleanup failed");

}

/* Prepare output directories and fds. */

void setup_dirs_fds(void) {

  u8* tmp;
  s32 fd;

  ACTF("Setting up output directories...");

  if (sync_id && mkdir(sync_dir, 0700) && errno != EEXIST)
    PFATAL("Unable to create '%s'", sync_dir);

  if (mkdir(out_dir, 0700)) {

    if (errno != EEXIST) PFATAL("Unable to create '%s'", out_dir);

    maybe_delete_out_dir();

  } else {

    if (in_place_resume)
      FATAL("Resume attempted but old output directory not found");

    out_dir_fd = open(out_dir, O_RDONLY);

#ifndef __sun

    if (out_dir_fd < 0 || flock(out_dir_fd, LOCK_EX | LOCK_NB))
      PFATAL("Unable to flock() output directory.");

#endif                                                            /* !__sun */

  }

  /* Queue directory for any starting & discovered paths. */

  tmp = alloc_printf("%s/queue", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  if (pre_save_handler) {
    tmp = alloc_printf("%s/gen_queue", out_dir);
    if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
    ck_free(tmp);
  }

  if (one_smart_mutation) {
    tmp = alloc_printf("%s/dec_queue", out_dir);
    if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
    ck_free(tmp);
  }

  /* Top-level directory for queue metadata used for session
     resume and related tasks. */

  tmp = alloc_printf("%s/queue/.state/", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  /* Directory for flagging queue entries that went through
     deterministic fuzzing in the past. */

  tmp = alloc_printf("%s/queue/.state/deterministic_done/", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  /* Directory with the auto-selected dictionary entries. */

  tmp = alloc_printf("%s/queue/.state/auto_extras/", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  /* The set of paths currently deemed redundant. */

  tmp = alloc_printf("%s/queue/.state/redundant_edges/", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  /* The set of paths showing variable behavior. */

  tmp = alloc_printf("%s/queue/.state/variable_behavior/", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  /* Sync directory for keeping track of cooperating fuzzers. */

  if (sync_id) {

    tmp = alloc_printf("%s/.synced/", out_dir);

    if (mkdir(tmp, 0700) && (!in_place_resume || errno != EEXIST))
      PFATAL("Unable to create '%s'", tmp);

    ck_free(tmp);

  }

  /* All recorded crashes. */

  tmp = alloc_printf("%s/crashes", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);
  
  if (pre_save_handler) {
    tmp = alloc_printf("%s/gen_crashes", out_dir);
    if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
    ck_free(tmp);
  }

  /* All recorded hangs. */

  tmp = alloc_printf("%s/hangs", out_dir);
  if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);
  if (pre_save_handler) {
    tmp = alloc_printf("%s/gen_hangs", out_dir);
    if (mkdir(tmp, 0700)) PFATAL("Unable to create '%s'", tmp);
    ck_free(tmp);
  }

  /* Generally useful file descriptors. */

  dev_null_fd = open("/dev/null", O_RDWR);
  if (dev_null_fd < 0) PFATAL("Unable to open /dev/null");

#ifndef HAVE_ARC4RANDOM
  dev_urandom_fd = open("/dev/urandom", O_RDONLY);
  if (dev_urandom_fd < 0) PFATAL("Unable to open /dev/urandom");
#endif

  /* Gnuplot output file. */

  tmp = alloc_printf("%s/plot_data", out_dir);
  fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  plot_file = fdopen(fd, "w");
  if (!plot_file) PFATAL("fdopen() failed");

  fprintf(plot_file,
          "# unix_time, cycles_done, cur_path, paths_total, "
          "pending_total, pending_favs, map_size, unique_crashes, "
          "unique_hangs, max_depth, execs_per_sec\n");
  /* ignore errors */

}

void setup_cmdline_file(char** argv) {

  u8* tmp;
  s32 fd;
  u32 i = 0;

  FILE* cmdline_file = NULL;

  /* Store the command line to reproduce our findings */
  tmp = alloc_printf("%s/cmdline", out_dir);
  fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) PFATAL("Unable to create '%s'", tmp);
  ck_free(tmp);

  cmdline_file = fdopen(fd, "w");
  if (!cmdline_file) PFATAL("fdopen() failed");

  while (argv[i]) {

    fprintf(cmdline_file, "%s\n", argv[i]);
    ++i;

  }

  fclose(cmdline_file);

}

/* Setup the output file for fuzzed data, if not using -f. */

void setup_stdio_file(void) {

  u8* fn;
  if (file_extension) {

    fn = alloc_printf("%s/.cur_input.%s", out_dir, file_extension);

  } else {

    fn = alloc_printf("%s/.cur_input", out_dir);

  }

  unlink(fn);                                              /* Ignore errors */

  out_fd = open(fn, O_RDWR | O_CREAT | O_EXCL, 0600);

  if (out_fd < 0) PFATAL("Unable to create '%s'", fn);

  ck_free(fn);

}

/* Make sure that core dumps don't go to a program. */

void check_crash_handling(void) {

#ifdef __APPLE__

  /* Yuck! There appears to be no simple C API to query for the state of
     loaded daemons on MacOS X, and I'm a bit hesitant to do something
     more sophisticated, such as disabling crash reporting via Mach ports,
     until I get a box to test the code. So, for now, we check for crash
     reporting the awful way. */

#if !TARGET_OS_IPHONE
  if (system("launchctl list 2>/dev/null | grep -q '\\.ReportCrash$'")) return;

  SAYF(
      "\n" cLRD "[-] " cRST
      "Whoops, your system is configured to forward crash notifications to an\n"
      "    external crash reporting utility. This will cause issues due to "
      "the\n"
      "    extended delay between the fuzzed binary malfunctioning and this "
      "fact\n"
      "    being relayed to the fuzzer via the standard waitpid() API.\n\n"
      "    To avoid having crashes misinterpreted as timeouts, please run the\n"
      "    following commands:\n\n"

      "    SL=/System/Library; PL=com.apple.ReportCrash\n"
      "    launchctl unload -w ${SL}/LaunchAgents/${PL}.plist\n"
      "    sudo launchctl unload -w ${SL}/LaunchDaemons/${PL}.Root.plist\n");

#endif
  if (!getenv("AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES"))
    FATAL("Crash reporter detected");

#else

  /* This is Linux specific, but I don't think there's anything equivalent on
   *BSD, so we can just let it slide for now. */

  s32 fd = open("/proc/sys/kernel/core_pattern", O_RDONLY);
  u8 fchar;

  if (fd < 0) return;

  ACTF("Checking core_pattern...");

  if (read(fd, &fchar, 1) == 1 && fchar == '|') {

    SAYF(
        "\n" cLRD "[-] " cRST
        "Hmm, your system is configured to send core dump notifications to an\n"
        "    external utility. This will cause issues: there will be an "
        "extended delay\n"
        "    between stumbling upon a crash and having this information "
        "relayed to the\n"
        "    fuzzer via the standard waitpid() API.\n\n"

        "    To avoid having crashes misinterpreted as timeouts, please log in "
        "as root\n"
        "    and temporarily modify /proc/sys/kernel/core_pattern, like so:\n\n"

        "    echo core >/proc/sys/kernel/core_pattern\n");

    if (!getenv("AFL_I_DONT_CARE_ABOUT_MISSING_CRASHES"))
      FATAL("Pipe at the beginning of 'core_pattern'");

  }

  close(fd);

#endif                                                        /* ^__APPLE__ */

}

/* Check CPU governor. */

void check_cpu_governor(void) {

#ifdef __linux__
  FILE* f;
  u8    tmp[128];
  u64   min = 0, max = 0;

  if (getenv("AFL_SKIP_CPUFREQ")) return;

  if (cpu_aff > 0)
    snprintf(tmp, sizeof(tmp), "%s%d%s", "/sys/devices/system/cpu/cpu", cpu_aff,
             "/cpufreq/scaling_governor");
  else
    snprintf(tmp, sizeof(tmp), "%s",
             "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
  f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
  if (!f) {

    if (cpu_aff > 0)
      snprintf(tmp, sizeof(tmp), "%s%d%s",
               "/sys/devices/system/cpu/cpufreq/policy", cpu_aff,
               "/scaling_governor");
    else
      snprintf(tmp, sizeof(tmp), "%s",
               "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor");
    f = fopen(tmp, "r");

  }

  if (!f) {

    WARNF("Could not check CPU scaling governor");
    return;

  }

  ACTF("Checking CPU scaling governor...");

  if (!fgets(tmp, 128, f)) PFATAL("fgets() failed");

  fclose(f);

  if (!strncmp(tmp, "perf", 4)) return;

  f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq", "r");

  if (f) {

    if (fscanf(f, "%llu", &min) != 1) min = 0;
    fclose(f);

  }

  f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", "r");

  if (f) {

    if (fscanf(f, "%llu", &max) != 1) max = 0;
    fclose(f);

  }

  if (min == max) return;

  SAYF("\n" cLRD "[-] " cRST
       "Whoops, your system uses on-demand CPU frequency scaling, adjusted\n"
       "    between %llu and %llu MHz. Unfortunately, the scaling algorithm in "
       "the\n"
       "    kernel is imperfect and can miss the short-lived processes spawned "
       "by\n"
       "    afl-fuzz. To keep things moving, run these commands as root:\n\n"

       "    cd /sys/devices/system/cpu\n"
       "    echo performance | tee cpu*/cpufreq/scaling_governor\n\n"

       "    You can later go back to the original state by replacing "
       "'performance'\n"
       "    with 'ondemand' or 'powersave'. If you don't want to change the "
       "settings,\n"
       "    set AFL_SKIP_CPUFREQ to make afl-fuzz skip this check - but expect "
       "some\n"
       "    performance drop.\n",
       min / 1024, max / 1024);
  FATAL("Suboptimal CPU scaling governor");

#elif defined __APPLE__
  u64 min = 0, max = 0;
  size_t mlen = sizeof(min);
  if (getenv("AFL_SKIP_CPUFREQ")) return;

  ACTF("Checking CPU scaling governor...");

  if (sysctlbyname("hw.cpufrequency_min", &min, &mlen, NULL, 0) == -1) {

    WARNF("Could not check CPU min frequency");
    return;

  }

  if (sysctlbyname("hw.cpufrequency_max", &max, &mlen, NULL, 0) == -1) {

    WARNF("Could not check CPU max frequency");
    return;

  }

  if (min == max) return;

  SAYF("\n" cLRD "[-] " cRST
       "Whoops, your system uses on-demand CPU frequency scaling, adjusted\n"
       "    between %llu and %llu MHz.\n"
       "    If you don't want to check those settings, set "
       "AFL_SKIP_CPUFREQ\n"
       "    to make afl-fuzz skip this check - but expect some performance "
       "drop.\n",
       min / 1024, max / 1024);
  FATAL("Suboptimal CPU scaling governor");
#endif

}

/* Count the number of logical CPU cores. */

void get_core_count(void) {

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)

  size_t s = sizeof(cpu_core_count);

  /* On *BSD systems, we can just use a sysctl to get the number of CPUs. */

#ifdef __APPLE__

  if (sysctlbyname("hw.logicalcpu", &cpu_core_count, &s, NULL, 0) < 0) return;

#else

  int s_name[2] = {CTL_HW, HW_NCPU};

  if (sysctl(s_name, 2, &cpu_core_count, &s, NULL, 0) < 0) return;

#endif                                                        /* ^__APPLE__ */

#else

#ifdef HAVE_AFFINITY

  cpu_core_count = sysconf(_SC_NPROCESSORS_ONLN);

#else

  FILE* f = fopen("/proc/stat", "r");
  u8    tmp[1024];

  if (!f) return;

  while (fgets(tmp, sizeof(tmp), f))
    if (!strncmp(tmp, "cpu", 3) && isdigit(tmp[3])) ++cpu_core_count;

  fclose(f);

#endif                                                    /* ^HAVE_AFFINITY */

#endif                        /* ^(__APPLE__ || __FreeBSD__ || __OpenBSD__) */

  if (cpu_core_count > 0) {

    u32 cur_runnable = 0;

    cur_runnable = (u32)get_runnable_processes();

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)

    /* Add ourselves, since the 1-minute average doesn't include that yet. */

    ++cur_runnable;

#endif                           /* __APPLE__ || __FreeBSD__ || __OpenBSD__ */

    OKF("You have %d CPU core%s and %u runnable tasks (utilization: %0.0f%%).",
        cpu_core_count, cpu_core_count > 1 ? "s" : "", cur_runnable,
        cur_runnable * 100.0 / cpu_core_count);

    if (cpu_core_count > 1) {

      if (cur_runnable > cpu_core_count * 1.5) {

        WARNF("System under apparent load, performance may be spotty.");

      } else if (cur_runnable + 1 <= cpu_core_count) {

        OKF("Try parallel jobs - see %s/parallel_fuzzing.txt.", doc_path);

      }

    }

  } else {

    cpu_core_count = 0;
    WARNF("Unable to figure out the number of CPU cores.");

  }

}

/* Validate and fix up out_dir and sync_dir when using -S. */

void fix_up_sync(void) {

  u8* x = sync_id;

  if (dumb_mode) FATAL("-S / -M and -n are mutually exclusive");

  if (skip_deterministic) {

    if (force_deterministic) FATAL("use -S instead of -M -d");
    // else
    //  FATAL("-S already implies -d");

  }

  while (*x) {

    if (!isalnum(*x) && *x != '_' && *x != '-')
      FATAL("Non-alphanumeric fuzzer ID specified via -S or -M");

    ++x;

  }

  if (strlen(sync_id) > 32) FATAL("Fuzzer ID too long");

  x = alloc_printf("%s/%s", out_dir, sync_id);

  sync_dir = out_dir;
  out_dir = x;

  if (!force_deterministic) {

    skip_deterministic = 1;
    use_splicing = 1;

  }

}

/* Handle screen resize (SIGWINCH). */

static void handle_resize(int sig) {

  clear_screen = 1;

}

/* Check ASAN options. */

void check_asan_opts(void) {

  u8* x = getenv("ASAN_OPTIONS");

  if (x) {

    if (!strstr(x, "abort_on_error=1"))
      FATAL("Custom ASAN_OPTIONS set without abort_on_error=1 - please fix!");

    if (!strstr(x, "symbolize=0"))
      FATAL("Custom ASAN_OPTIONS set without symbolize=0 - please fix!");

  }

  x = getenv("MSAN_OPTIONS");

  if (x) {

    if (!strstr(x, "exit_code=" STRINGIFY(MSAN_ERROR)))
      FATAL("Custom MSAN_OPTIONS set without exit_code=" STRINGIFY(
          MSAN_ERROR) " - please fix!");

    if (!strstr(x, "symbolize=0"))
      FATAL("Custom MSAN_OPTIONS set without symbolize=0 - please fix!");

  }

}

/* Handle stop signal (Ctrl-C, etc). */

static void handle_stop_sig(int sig) {

  stop_soon = 1;

  if (child_pid > 0) kill(child_pid, SIGKILL);
  if (forksrv_pid > 0) kill(forksrv_pid, SIGKILL);

}

/* Handle skip request (SIGUSR1). */

static void handle_skipreq(int sig) {

  skip_requested = 1;

}

/* Do a PATH search and find target binary to see that it exists and
   isn't a shell script - a common and painful mistake. We also check for
   a valid ELF header and for evidence of AFL instrumentation. */

void check_binary(u8* fname) {

  u8*         env_path = 0;
  struct stat st;

  s32 fd;
  u8* f_data;
  u32 f_len = 0;

  ACTF("Validating target binary...");

  if (strchr(fname, '/') || !(env_path = getenv("PATH"))) {

    target_path = ck_strdup(fname);
    if (stat(target_path, &st) || !S_ISREG(st.st_mode) ||
        !(st.st_mode & 0111) || (f_len = st.st_size) < 4)
      FATAL("Program '%s' not found or not executable", fname);

  } else {

    while (env_path) {

      u8 *cur_elem, *delim = strchr(env_path, ':');

      if (delim) {

        cur_elem = ck_alloc(delim - env_path + 1);
        memcpy(cur_elem, env_path, delim - env_path);
        ++delim;

      } else

        cur_elem = ck_strdup(env_path);

      env_path = delim;

      if (cur_elem[0])
        target_path = alloc_printf("%s/%s", cur_elem, fname);
      else
        target_path = ck_strdup(fname);

      ck_free(cur_elem);

      if (!stat(target_path, &st) && S_ISREG(st.st_mode) &&
          (st.st_mode & 0111) && (f_len = st.st_size) >= 4)
        break;

      ck_free(target_path);
      target_path = 0;

    }

    if (!target_path) FATAL("Program '%s' not found or not executable", fname);

  }

  if (getenv("AFL_SKIP_BIN_CHECK") || use_wine) return;

  /* Check for blatant user errors. */

  if ((!strncmp(target_path, "/tmp/", 5) && !strchr(target_path + 5, '/')) ||
      (!strncmp(target_path, "/var/tmp/", 9) && !strchr(target_path + 9, '/')))
    FATAL("Please don't keep binaries in /tmp or /var/tmp");

  fd = open(target_path, O_RDONLY);

  if (fd < 0) PFATAL("Unable to open '%s'", target_path);

  f_data = mmap(0, f_len, PROT_READ, MAP_PRIVATE, fd, 0);

  if (f_data == MAP_FAILED) PFATAL("Unable to mmap file '%s'", target_path);

  close(fd);

  if (f_data[0] == '#' && f_data[1] == '!') {

    SAYF("\n" cLRD "[-] " cRST
         "Oops, the target binary looks like a shell script. Some build "
         "systems will\n"
         "    sometimes generate shell stubs for dynamically linked programs; "
         "try static\n"
         "    library mode (./configure --disable-shared) if that's the "
         "case.\n\n"

         "    Another possible cause is that you are actually trying to use a "
         "shell\n"
         "    wrapper around the fuzzed component. Invoking shell can slow "
         "down the\n"
         "    fuzzing process by a factor of 20x or more; it's best to write "
         "the wrapper\n"
         "    in a compiled language instead.\n");

    FATAL("Program '%s' is a shell script", target_path);

  }

#ifndef __APPLE__

  if (f_data[0] != 0x7f || memcmp(f_data + 1, "ELF", 3))
    FATAL("Program '%s' is not an ELF binary", target_path);

#else

#if !defined(__arm__) && !defined(__arm64__)
  if ((f_data[0] != 0xCF || f_data[1] != 0xFA || f_data[2] != 0xED) &&
      (f_data[0] != 0xCA || f_data[1] != 0xFE || f_data[2] != 0xBA))
    FATAL("Program '%s' is not a 64-bit or universal Mach-O binary",
          target_path);
#endif

#endif                                                       /* ^!__APPLE__ */

  if (!qemu_mode && !unicorn_mode && !dumb_mode &&
      !memmem(f_data, f_len, SHM_ENV_VAR, strlen(SHM_ENV_VAR) + 1)) {

    SAYF(
        "\n" cLRD "[-] " cRST
        "Looks like the target binary is not instrumented! The fuzzer depends "
        "on\n"
        "    compile-time instrumentation to isolate interesting test cases "
        "while\n"
        "    mutating the input data. For more information, and for tips on "
        "how to\n"
        "    instrument binaries, please see %s/README.\n\n"

        "    When source code is not available, you may be able to leverage "
        "QEMU\n"
        "    mode support. Consult the README for tips on how to enable this.\n"

        "    (It is also possible to use afl-fuzz as a traditional, \"dumb\" "
        "fuzzer.\n"
        "    For that, you can use the -n option - but expect much worse "
        "results.)\n",
        doc_path);

    FATAL("No instrumentation detected");

  }

  if ((qemu_mode || unicorn_mode) &&
      memmem(f_data, f_len, SHM_ENV_VAR, strlen(SHM_ENV_VAR) + 1)) {

    SAYF("\n" cLRD "[-] " cRST
         "This program appears to be instrumented with afl-gcc, but is being "
         "run in\n"
         "    QEMU or Unicorn mode (-Q or -U). This is probably not what you "
         "want -\n"
         "    this setup will be slow and offer no practical benefits.\n");

    FATAL("Instrumentation found in -Q or -U mode");

  }

  if (memmem(f_data, f_len, "libasan.so", 10) ||
      memmem(f_data, f_len, "__msan_init", 11))
    uses_asan = 1;

  /* Detect persistent & deferred init signatures in the binary. */

  if (memmem(f_data, f_len, PERSIST_SIG, strlen(PERSIST_SIG) + 1)) {

    OKF(cPIN "Persistent mode binary detected.");
    setenv(PERSIST_ENV_VAR, "1", 1);
    persistent_mode = 1;

  } else if (getenv("AFL_PERSISTENT")) {

    WARNF("AFL_PERSISTENT is no longer supported and may misbehave!");

  }

  if (memmem(f_data, f_len, DEFER_SIG, strlen(DEFER_SIG) + 1)) {

    OKF(cPIN "Deferred forkserver binary detected.");
    setenv(DEFER_ENV_VAR, "1", 1);
    deferred_mode = 1;

  } else if (getenv("AFL_DEFER_FORKSRV")) {

    WARNF("AFL_DEFER_FORKSRV is no longer supported and may misbehave!");

  }

  if (munmap(f_data, f_len)) PFATAL("unmap() failed");

}

/* Trim and possibly create a banner for the run. */

void fix_up_banner(u8* name) {

  if (!use_banner) {

    if (sync_id) {

      use_banner = sync_id;

    } else {

      u8* trim = strrchr(name, '/');
      if (!trim)
        use_banner = name;
      else
        use_banner = trim + 1;

    }

  }

  if (strlen(use_banner) > 32) {

    u8* tmp = ck_alloc(36);
    sprintf(tmp, "%.32s...", use_banner);
    use_banner = tmp;

  }

}

/* Check if we're on TTY. */

void check_if_tty(void) {

  struct winsize ws;

  if (getenv("AFL_NO_UI")) {

    OKF("Disabling the UI because AFL_NO_UI is set.");
    not_on_tty = 1;
    return;

  }

  if (ioctl(1, TIOCGWINSZ, &ws)) {

    if (errno == ENOTTY) {

      OKF("Looks like we're not running on a tty, so I'll be a bit less "
          "verbose.");
      not_on_tty = 1;

    }

    return;

  }

}

/* Set up signal handlers. More complicated that needs to be, because libc on
   Solaris doesn't resume interrupted reads(), sets SA_RESETHAND when you call
   siginterrupt(), and does other stupid things. */

void setup_signal_handlers(void) {

  struct sigaction sa;

  sa.sa_handler = NULL;
  sa.sa_flags = SA_RESTART;
  sa.sa_sigaction = NULL;

  sigemptyset(&sa.sa_mask);

  /* Various ways of saying "stop". */

  sa.sa_handler = handle_stop_sig;
  sigaction(SIGHUP, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  /* Exec timeout notifications. */

  sa.sa_handler = handle_timeout;
  sigaction(SIGALRM, &sa, NULL);

  /* Window resize */

  sa.sa_handler = handle_resize;
  sigaction(SIGWINCH, &sa, NULL);

  /* SIGUSR1: skip entry */

  sa.sa_handler = handle_skipreq;
  sigaction(SIGUSR1, &sa, NULL);

  /* Things we don't care about. */

  sa.sa_handler = SIG_IGN;
  sigaction(SIGTSTP, &sa, NULL);
  sigaction(SIGPIPE, &sa, NULL);

}

/* Make a copy of the current command line. */

void save_cmdline(u32 argc, char** argv) {

  u32 len = 1, i;
  u8* buf;

  for (i = 0; i < argc; ++i)
    len += strlen(argv[i]) + 1;

  buf = orig_cmdline = ck_alloc(len);

  for (i = 0; i < argc; ++i) {

    u32 l = strlen(argv[i]);

    memcpy(buf, argv[i], l);
    buf += l;

    if (i != argc - 1) *(buf++) = ' ';

  }

  *buf = 0;

}

