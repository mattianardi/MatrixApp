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
int c_agent(int argc, char **argv, struct ctx *c);
int c_sha256(int argc, char **argv, struct ctx *c);

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
    { "agent",    "agent [list|tools|add|grant|revoke|policy|pending|approve|deny|log|journal|rollback|freeze|call|tuner]", "AI agents: identities, capabilities, approvals, audit", c_agent, 1, 0 }, \
    { "sha256",   "sha256 <file>",             "SHA-256 of a file",                          c_sha256,   0, 0 }, \
    { "pkg",      "pkg [update|list|info|install|remove] [name]", "package manager",        c_pkg,      1, 0 },
