// SPDX-License-Identifier: AGPL-3.0-or-later
//
// ap_spawn_test — host self-test for common/ap_spawn.h (aa_ap_script start).
// Starts a probe script detached and checks what it sees: its own session,
// /dev/null as stdin, no inherited descriptors, a clean environment without
// LD_PRELOAD, its output in the log file, and that the caller does not wait
// for it. Returns non-zero on any failed check.

#include "common/ap_spawn.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(cond, msg) do {                                        \
        if (!(cond)) { printf("  FAIL: %s\n", (msg)); g_fail = 1; }  \
        else         { printf("  ok:   %s\n", (msg)); }              \
    } while (0)

static std::string slurp(const char *path)
{
    std::string s; FILE *f = fopen(path, "r"); if (!f) return s;
    char buf[512]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f); return s;
}

static double now()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main()
{
    char dir[] = "/tmp/ap_spawn_test.XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    const std::string script = std::string(dir) + "/probe.sh";
    const std::string report = std::string(dir) + "/report.txt";
    const std::string log    = std::string(dir) + "/start.log";

    // The probe writes what it sees, then outlives the caller by a few seconds.
    FILE *f = fopen(script.c_str(), "w");
    fprintf(f,
        "echo started-on-stdout\n"
        "{\n"
        "  echo \"sid=$(cut -d' ' -f6 /proc/$$/stat)\"\n"
        "  echo \"fds=$(for x in /proc/$$/fd/*; do readlink $x; done | tr '\\n' ' ')\"\n"
        "  echo \"stdin=$(readlink /proc/$$/fd/0)\"\n"
        "  echo \"preload=${LD_PRELOAD-unset}\"\n"
        "  echo \"secret=${AP_SPAWN_SECRET-unset}\"\n"
        "  echo \"path=$PATH\"\n"
        "} > %s.tmp\n"
        "mv %s.tmp %s\n"
        "sleep 3\n", report.c_str(), report.c_str(), report.c_str());
    fclose(f);   // 0644 on purpose: started through /bin/sh, no x bit needed

    // Things the child must NOT inherit.
    setenv("LD_PRELOAD", "/nonexistent/libpatch-aap_service.so", 1);
    setenv("AP_SPAWN_SECRET", "leak", 1);
    const std::string marker = std::string(dir) + "/leak-marker";
    const int leaky0 = open(marker.c_str(), O_WRONLY | O_CREAT, 0644);  // no CLOEXEC
    const int leaky  = dup2(leaky0, 200);                              // and a high one

    printf("[1] missing or empty script\n");
    CHECK(ap_spawn::start_detached("", log.c_str()) == -1, "empty path is refused");
    CHECK(ap_spawn::start_detached((std::string(dir) + "/nope.sh").c_str(), log.c_str()) == -1,
          "missing script is refused");

    printf("[2] detached start\n");
    const double t0 = now();
    const pid_t first = ap_spawn::start_detached(script.c_str(), log.c_str());
    const double took = now() - t0;
    CHECK(first > 0, "script started");
    CHECK(took < 1.5, "caller does not wait for the script (it sleeps 3 s)");

    std::string r;
    for (int i = 0; i < 50 && r.empty(); ++i) { usleep(100000); r = slurp(report.c_str()); }
    printf("  report:\n%s", r.c_str());
    CHECK(!r.empty(), "probe wrote its report");

    char mysid[32]; snprintf(mysid, sizeof mysid, "sid=%d\n", (int)getsid(0));
    CHECK(r.find(mysid) == std::string::npos && r.find("sid=") != std::string::npos,
          "own session (setsid)");
    CHECK(r.find("stdin=/dev/null") != std::string::npos, "stdin is /dev/null");
    // The shell keeps its own internal descriptors (script, saved stdout);
    // what must not be there is anything the caller had open.
    CHECK(r.find("leak-marker") == std::string::npos, "caller's open files are not inherited");
    CHECK(r.find("preload=unset") != std::string::npos, "LD_PRELOAD not inherited");
    CHECK(r.find("secret=unset") != std::string::npos, "caller environment not inherited");
    CHECK(r.find("path=/usr/sbin:/usr/bin:/sbin:/bin") != std::string::npos, "fixed PATH");
    CHECK(slurp(log.c_str()).find("started-on-stdout") != std::string::npos, "stdout goes to the log file");

    int status = 0;
    CHECK(waitpid(-1, &status, WNOHANG) <= 0, "no zombie left for the caller");
    close(leaky); close(leaky0);

    printf("\n%s\n", g_fail ? "RESULT: FAIL" : "RESULT: PASS");
    return g_fail;
}
