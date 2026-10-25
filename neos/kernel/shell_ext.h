#pragma once
/* commands implemented in shell_ext.c (network, files, packages) */
int c_net(int argc, char **argv, struct ctx *c);
int c_ping(int argc, char **argv, struct ctx *c);
int c_nslookup(int argc, char **argv, struct ctx *c);
int c_pci(int argc, char **argv, struct ctx *c);
int c_http(int argc, char **argv, struct ctx *c);
int c_ls(int argc, char **argv, struct ctx *c);
int c_cat(int argc, char **argv, struct ctx *c);
int c_rm(int argc, char **argv, struct ctx *c);
int c_write(int argc, char **argv, struct ctx *c);
int c_cd(int argc, char **argv, struct ctx *c);
int c_pwd(int argc, char **argv, struct ctx *c);
int c_pkg(int argc, char **argv, struct ctx *c);
int c_run(int argc, char **argv, struct ctx *c);
int c_proc(int argc, char **argv, struct ctx *c);
int c_pkill(int argc, char **argv, struct ctx *c);
int c_binfmt(int argc, char **argv, struct ctx *c);
int c_lx(int argc, char **argv, struct ctx *c);
int shell_try_external(int argc, char **argv, struct ctx *c);
int c_aih(int argc, char **argv, struct ctx *c);
int c_agent(int argc, char **argv, struct ctx *c);
int c_sha256(int argc, char **argv, struct ctx *c);
int c_disk(int argc, char **argv, struct ctx *c);
int c_browse(int argc, char **argv, struct ctx *c);
int c_fontdump(int argc, char **argv, struct ctx *c);
int c_usb(int argc, char **argv, struct ctx *c);
int c_tcp(int argc, char **argv, struct ctx *c);
int c_pip(int argc, char **argv, struct ctx *c);
int c_apt(int argc, char **argv, struct ctx *c);
int c_dpkg(int argc, char **argv, struct ctx *c);

#define SHELL_EXT_CMDS \
    { "net",      "net [dhcp | set <ip> <mask> <gw> [dns]]", "network status / configure",   c_net,      1, 0 }, \
    { "ping",     "ping <host> [count]",       "ICMP echo",                                  c_ping,     0, 0 }, \
    { "nslookup", "nslookup <host>",           "DNS A lookup",                               c_nslookup, 0, 0 }, \
    { "pci",      "pci",                       "PCI devices",                                c_pci,      1, 0 }, \
    { "http",     "http <url> [file]",         "HTTP GET (to screen or to a file)",          c_http,     0, 0 }, \
    { "ls",       "ls [path]",                 "list files (Linux or Windows paths)",        c_ls,       1, 0 }, \
    { "cat",      "cat <file>",                "show a file",                                c_cat,      0, 0 }, \
    { "rm",       "rm <file>",                 "delete a file",                              c_rm,       0, 0 }, \
    { "write",    "write <file> <text>",       "create a file with text",                    c_write,    0, 0 }, \
    { "cd",       "cd <dir>",                  "change directory ($PWD is shared with both worlds)", c_cd, 0, 0 }, \
    { "pwd",      "pwd",                       "current directory",                          c_pwd,      0, 0 }, \
    { "run",      "run [-b] [-g cap[:scope]].. [-w lin|win] <prog> [args]", "start a program in ring 3 (its capabilities come from -g)", c_run, 0, 0 }, \
    { "proc",     "proc",                      "user processes",                             c_proc,     1, 0 }, \
    { "pkill",    "pkill <pid>",               "kill a user process",                        c_pkill,    0, 0 }, \
    { "aih",      "aih <what you want to do>", "AI Help: say it in plain language, review the plan, confirm", c_aih, 0, 0 }, \
    { "lx",       "lx <cmd> [args]",           "run the Linux program /bin/<cmd> (even if NEOS has a built-in)", c_lx, 0, 0 }, \
    { "binfmt",   "binfmt [add .ext /interp [arg] | remove .ext]", "how files are started: extension -> interpreter", c_binfmt, 1, 0 }, \
    { "agent",    "agent [list|tools|add|grant|revoke|policy|pending|approve|deny|log|journal|rollback|freeze|call|tuner]", "AI agents: identities, capabilities, approvals, audit", c_agent, 1, 0 }, \
    { "tcp",      "tcp", "TCP connections and listeners (state, peer, queued bytes)", c_tcp, 1, 0 }, \
    { "usb",      "usb [list | mount | sync | umount]", "USB devices; mount a FAT stick at /mnt/usb (RAM copy, written back by sync/umount)", c_usb, 1, 0 }, \
    { "pip",      "pip install|uninstall|list|show <pkg>", "Python packages (same as `pkg install pip:<name>`)", c_pip, 0, 0 }, \
    { "pip3",     "pip3 ...",                  "alias of pip",                               c_pip, 0, 0 }, \
    { "apt",      "apt update|install|remove|search|show <pkg>", "Debian/Ubuntu packages with dependencies (same as `pkg install apt:<name>`)", c_apt, 0, 0 }, \
    { "apt-get",  "apt-get ...",               "alias of apt",                               c_apt, 0, 0 }, \
    { "dpkg",     "dpkg -i <file.deb> | -r <name>", "install/remove one .deb (same as `pkg install deb:<file>`)", c_dpkg, 0, 0 }, \
    { "fontdump", "fontdump",                  "print the 8x16 console font as hex (build tooling)", c_fontdump, 0, 0 }, \
    { "browse",   "browse <url>",              "open the browser window: http://, file://, neos://home|system|settings|agents|files", c_browse, 0, 0 }, \
    { "disk",     "disk [status|sync|format]",  "durable storage (NeoFS): status, snapshot now, wipe", c_disk, 0, 0 }, \
    { "sha256",   "sha256 <file>",             "SHA-256 of a file",                          c_sha256,   0, 0 }, \
    { "pkg",      "pkg [update|list|info|install|remove] [name]", "package manager",        c_pkg,      1, 0 },
