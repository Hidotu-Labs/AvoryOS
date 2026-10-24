#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == 10 || s[n - 1] == 13)) s[--n] = 0;
}

/* Display handoff (AvoryOS): a graphical Plymouth splash switches the
 * virtio scanout to its own resource, and there is no native fbcon resource
 * saved to switch back to — so after quit the text prompt keeps drawing to
 * a hidden framebuffer while QEMU shows black. Re-present right after the
 * prompt is drawn via a transient DRM open/close: with no other holders
 * this is a last-close, and the kernel hands the display back showing the
 * just-drawn prompt. With a live master (SDDM/Weston) our close is not
 * last, so nothing is stolen. Silent on purpose (card may not exist). */
static void force_console_present(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    close(fd);
}

static int authenticate(const struct passwd *pw) {
    if (!pw->pw_passwd || strcmp(pw->pw_passwd, "!") == 0 ||
        strcmp(pw->pw_passwd, "*") == 0) return 0;
    if (pw->pw_passwd[0] == 0) return 1;
    char *password = getpass("Password: ");
    if (!password) return 0;
    char *hash = crypt(password, pw->pw_passwd);
    int ok = hash && strcmp(hash, pw->pw_passwd) == 0;
    memset(password, 0, strlen(password));
    return ok;
}

static void run_session(const struct passwd *pw) {
    if (initgroups(pw->pw_name, pw->pw_gid) < 0 || setgid(pw->pw_gid) < 0) {
        perror("login: groups");
        return;
    }
    if (setuid(pw->pw_uid) < 0) {
        perror("login: setuid");
        return;
    }
    const char *homedir = (pw->pw_uid == 0) ? "/" : (pw->pw_dir && pw->pw_dir[0] ? pw->pw_dir : "/");
    if (chdir(homedir) < 0 && chdir("/") < 0) {
        perror("login: chdir");
        _exit(1);
    }

    const char *shell = pw->pw_shell && pw->pw_shell[0] ? pw->pw_shell : "/bin/sh";
    clearenv();
    setenv("HOME", homedir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("TERM_PROGRAM", "vt", 1);

    char runtime_dir[64];
    snprintf(runtime_dir, sizeof(runtime_dir), "/tmp/runtime-%u", (unsigned)pw->pw_uid);
    mkdir(runtime_dir, 0700);
    chmod(runtime_dir, 0700);
    chown(runtime_dir, pw->pw_uid, pw->pw_gid);
    setenv("XDG_RUNTIME_DIR", runtime_dir, 1);

    if (pw->pw_uid == 0) {
        setenv("XDG_CONFIG_HOME", "/.config", 1);
        setenv("XDG_CACHE_HOME", "/.cache", 1);
        setenv("XDG_DATA_HOME", "/.local/share", 1);
        setenv("XDG_STATE_HOME", "/.local/state", 1);
        mkdir("/.config", 0755);
        mkdir("/.cache", 0755);
        mkdir("/.local", 0755);
        mkdir("/.local/share", 0755);
        mkdir("/.local/state", 0755);
    }

    if (access("/run/dbus/system_bus_socket", F_OK) == 0) {
        setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/run/dbus/system_bus_socket", 1);
    } else if (access("/var/run/dbus/system_bus_socket", F_OK) == 0) {
        setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/var/run/dbus/system_bus_socket", 1);
    }

    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    char login_argv0[260];
    snprintf(login_argv0, sizeof(login_argv0), "-%s", base);
    execl(shell, login_argv0, (char *)NULL);

    if (strcmp(shell, "/bin/sh") != 0) {
        execl("/bin/sh", "-sh", (char *)NULL);
    }
    if (strcmp(shell, "/bin/busybox") != 0) {
        execl("/bin/busybox", "sh", (char *)NULL);
    }
    perror("login: exec shell");
    _exit(127);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Check if booted with avory_installer on the kernel command line */
    FILE *cmdline = fopen("/proc/cmdline", "r");
    if (cmdline) {
        char buf[512];
        if (fgets(buf, sizeof(buf), cmdline) && strstr(buf, "avory_installer")) {
            fclose(cmdline);
            pid_t child = fork();
            if (child == 0) {
                execl("/usr/sbin/avory-installer", "avory-installer", (char *)NULL);
                execl("/bin/avory-installer", "avory-installer", (char *)NULL);
                _exit(127);
            }
            if (child > 0) {
                int status;
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            }
        } else {
            fclose(cmdline);
        }
    }

    for (;;) {
        char username[64];
        puts("\nAvoryOS (type 'install' for installer, or login)");
        fputs("login: ", stdout);
        force_console_present();
        if (!fgets(username, sizeof(username), stdin)) {
            clearerr(stdin);
            sleep(1);
            continue;
        }
        trim_newline(username);
        if (!username[0]) continue;

        if (strcmp(username, "installer") == 0 || strcmp(username, "install") == 0) {
            pid_t child = fork();
            if (child == 0) {
                execl("/usr/sbin/avory-installer", "avory-installer", (char *)NULL);
                execl("/bin/avory-installer", "avory-installer", (char *)NULL);
                _exit(127);
            }
            if (child > 0) {
                int status;
                while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
            }
            continue;
        }

        struct passwd *found = getpwnam(username);
        if (!found || !authenticate(found)) {
            puts("Login incorrect");
            sleep(1);
            continue;
        }

        struct passwd pw = *found;
        char name[64], home[256], shell[256];
        snprintf(name, sizeof(name), "%s", found->pw_name);
        snprintf(home, sizeof(home), "%s", found->pw_dir);
        snprintf(shell, sizeof(shell), "%s", found->pw_shell);
        pw.pw_name = name; pw.pw_dir = home; pw.pw_shell = shell;

        pid_t child = fork();
        if (child == 0) { run_session(&pw); _exit(1); }
        if (child < 0) perror("login: fork");
        else { int status; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
    }
}
