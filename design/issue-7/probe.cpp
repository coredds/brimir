// Probe: what does UCRT fwrite(stdout) do in a GUI-subsystem process with no console?
#include <windows.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <io.h>

int main() {
    FILE *log = fopen(getenv("PROBE_LOG"), "w");
    fprintf(log, "GetStdHandle(OUT)=%p fileno(stdout)=%d isatty=%d\n", GetStdHandle(STD_OUTPUT_HANDLE),
            _fileno(stdout), _isatty(_fileno(stdout)));
    char line[80];
    memset(line, 'x', sizeof(line) - 1);
    line[sizeof(line) - 1] = '\n';
    size_t total = 0;
    for (int i = 0; i < 400; ++i) {
        errno = 0;
        size_t w = fwrite(line, 1, sizeof(line), stdout);
        total += w;
        if (w < sizeof(line)) {
            fprintf(log, "SHORT WRITE at call %d: wrote %zu of %zu, total before=%zu, errno=%d\n", i, w,
                    sizeof(line), total - w, errno);
            break;
        }
    }
    fprintf(log, "done, total=%zu\n", total);
    fclose(log);
    return 0;
}
