// Host self-test for mazda/dbapi/conf_write.h, the libpatch.conf rewriter.
//
// This edits a file the user maintains by hand, so what matters is not that the new
// value is there but that nothing else moved — the shipped file is mostly the
// documentation for each key. Hence the fixture is the real
// resources/libpatch.conf and most checks are about bytes that must NOT change.
//
// Host build, from the repo root:
//     make -C test confwrite_test && ./test/build/confwrite_test
// Optional args: <fixture> <scratch path>. Non-zero exit on failure.

// LOG_TAG only; conf_write.h pulls in dbapi/log.h, which sets LIBPATCH_NAME.
#define LOG_TAG "TEST"
#include "conf_write.h"

#include <stdio.h>
#include <string>
#include <unistd.h>

namespace {

int g_fails = 0;

void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++g_fails;
    }
}

std::string slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return std::string();
    }
    std::string s;
    char        b[4096];
    size_t      n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) {
        s.append(b, n);
    }
    fclose(f);
    return s;
}

void spew(const char *path, const std::string &s)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        printf("FAIL  cannot write %s\n", path);
        ++g_fails;
        return;
    }
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

size_t count_of(const std::string &hay, const std::string &needle)
{
    size_t n = 0, at = 0;
    while ((at = hay.find(needle, at)) != std::string::npos) {
        ++n;
        ++at;
    }
    return n;
}

} // namespace

int main(int argc, char **argv)
{
    const char *fixture = (argc > 1) ? argv[1] : "resources/libpatch.conf";
    const char *scratch = (argc > 2) ? argv[2] : "test/build/confwrite_test.conf";

    const std::string orig = slurp(fixture);
    if (orig.empty()) {
        printf("FAIL  cannot read fixture %s (run from the repo root)\n", fixture);
        return 1;
    }

    /* --- rewriting a key the file already sets ---------------------------- */
    spew(scratch, orig);
    check(conf_write::set_key(scratch, "touch", "false"), "set touch=false succeeds");
    const std::string a = slurp(scratch);
    check(a.size() == orig.size() + 1, "file grew by exactly one byte (true -> false)");
    check(a.find("\ntouch = false\n") != std::string::npos,
          "the value changed and the spacing around '=' is kept");
    check(a.find("\ntouch = true\n") == std::string::npos, "no stale line left behind");
    check(a.find("# Enable the Android Auto touch-input shim.") != std::string::npos,
          "the key's documentation comment survives");
    check(a.find("hud_transport = svcnavi") != std::string::npos, "other keys untouched");

    check(conf_write::set_key(scratch, "touch", "false"), "writing the same value again succeeds");
    check(slurp(scratch) == a, "...and is byte-identical (idempotent)");

    check(conf_write::set_key(scratch, "touch", "true"), "flipping back succeeds");
    check(slurp(scratch) == orig, "round-trip restores the original file byte-for-byte");

    /* --- a key present only as a comment --------------------------------- */
    // A comment is not an assignment: it stays, and the key is appended live.
    // Synthesised rather than taken from the shipped file, so the case does not
    // depend on which keys a branch documents that way.
    spew(scratch,
         "# some_key sets a thing.\n"
         "# Default: empty\n"
         "# some_key =\n"
         "\n"
         "other_key = 1\n");
    check(conf_write::set_key(scratch, "some_key", "first"),
          "setting a commented-only key succeeds");
    const std::string b = slurp(scratch);
    check(b.find("# some_key =") != std::string::npos, "the comment is still a comment");
    check(b.find("\nsome_key = first\n") != std::string::npos,
          "the key was appended as a live assignment");
    check(b.find("other_key = 1") != std::string::npos, "the real key either side is untouched");

    check(conf_write::set_key(scratch, "some_key", "second"),
          "editing the appended key succeeds");
    const std::string c = slurp(scratch);
    check(c.find("some_key = second") != std::string::npos, "it was rewritten in place");
    check(c.find("some_key = first") == std::string::npos, "the old value is gone");
    check(count_of(c, "\nsome_key = ") == 1, "exactly one live some_key line (no duplicate append)");
    check(count_of(c, "# some_key =") == 1, "and the comment was not duplicated either");

    /* --- a key set more than once ---------------------------------------- */
    // config.h applies lines in order, so the last one wins. Every assignment must
    // be rewritten, or a saved change can be overridden by a stale later line.
    spew(scratch,
         "touch = true\n"
         "hud = true\n"
         "touch=true\n");
    check(conf_write::set_key(scratch, "touch", "false"), "setting a duplicated key succeeds");
    check(slurp(scratch) == "touch = false\nhud = true\ntouch=false\n",
          "...every assignment is rewritten, each keeping its own spacing");

    /* --- creating the file from nothing ---------------------------------- */
    unlink(scratch);
    check(conf_write::set_key(scratch, "wireless", "true"), "a missing file is created");
    check(slurp(scratch).find("wireless = true") != std::string::npos, "with the key in it");

    /* --- shapes a hand-edit might leave behind ---------------------------- */
    spew(scratch, "touch = true");
    check(conf_write::set_key(scratch, "touch", "false"), "unterminated last line handled");
    check(slurp(scratch) == "touch = false", "...and rewritten without inventing a newline");

    spew(scratch, "\thud\t=\ttrue\r\n# c\r\nhud_fold_latin=false\r\n");
    check(conf_write::set_key(scratch, "HUD", "false"), "keys match case-insensitively");
    check(slurp(scratch) == "\thud\t=\tfalse\r\n# c\r\nhud_fold_latin=false\r\n",
          "CRLF, tab indent and tab padding around '=' all preserved");
    check(conf_write::set_key(scratch, "hud_fold_latin", "true"), "an unpadded 'key=value' works");
    check(slurp(scratch) == "\thud\t=\tfalse\r\n# c\r\nhud_fold_latin=true\r\n",
          "...and stays unpadded");

    unlink(scratch);
    printf("\n%s (%d failures)\n", g_fails ? "FAILURES" : "all good", g_fails);
    return g_fails != 0;
}
