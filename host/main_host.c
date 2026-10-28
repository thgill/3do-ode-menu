/* main_host.c - run the menu on the host: menu_host --sd DIR [--usb DIR] --script FILE --out DIR */
#include "h3do.h"
#include <unistd.h>

extern int menu_main(void);
void host_setup(const char *outdir, const char *script);
void fwsim_setup(const char *sd, const char *usb);
void fwsim_start_in(const char *rel);
extern int fw_boot_source;
extern long fw_readdir_steps, fw_cmds, fw_d1, fw_e6, fw_bytes_written, fw_d1_overflow, fw_dotdot;

static void report(void)
{
    fprintf(stderr, "firmware: %ld commands, %ld D1, %ld E6, %ld directory reads, %ld bytes written, %ld D1 overflows, %ld dot-dot names\n",
            fw_cmds, fw_d1, fw_e6, fw_readdir_steps, fw_bytes_written, fw_d1_overflow, fw_dotdot);
}

int main(int argc, char **argv)
{
    const char *sd = 0, *usb = 0, *script = 0, *out = ".", *assets = ".", *start = 0;
    int i;

    for (i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--sd")) sd = argv[i + 1];
        else if (!strcmp(argv[i], "--usb")) usb = argv[i + 1];
        else if (!strcmp(argv[i], "--script")) script = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--disc")) assets = argv[i + 1];
        else if (!strcmp(argv[i], "--start")) start = argv[i + 1];
        else if (!strcmp(argv[i], "--source")) fw_boot_source = atoi(argv[i + 1]);
    }
    fwsim_setup(sd, usb);
    if (start)
        fwsim_start_in(start);
    host_setup(out, script);
    atexit(report);
    if (chdir(assets) != 0)
        return 1;
    return menu_main();
}
