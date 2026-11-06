// HP2.app's CFBundleExecutable (built by tools/install_app.sh; RULES R-I5, the ROTK solution 2026-11-06).
// A bash script as the bundle's executable cannot read the app's files when the app is opened from
// Finder under ~/Desktop (macOS privacy, TCC: the responsible process is the interpreter, no prompt,
// no log). This small Mach-O sets up the environment and execv()s the game in the same process:
//   - player files in HP2_PLAYER_ROOT (baked at install: HP2Port/.cache/player), else
//     ~/Library/Application Support/HP2/player: HOME=<root>/home (settings.toml, launcher.toml,
//     HP2Port/<profile>/ saves and hp2.ini), HP2_LOG_DIR = HP2_CRASH_DIR = <root>/logs;
//   - the log <root>/logs/hp2.log has a size cap: stdout + stderr of the game go through a pipe to a
//     forked copy of this launcher; every start and every LOG_CAP rotates hp2.log -> hp2.log.1 .. .5
//     (a crashed run's log survives the next start; owner 2026-11-06: ~/Library/Logs/hp2.log had grown to 5.4 GB). Kernel call tracing is off
//     unless HP2_KTRACE is set (it was on by default in the runtime: the 5.4 GB);
//   - HP2_DATA = Contents/Resources (game/extracted inside), HP2_MODS = Contents/Resources/mods;
//   - language (HP2_LANG): en | uk | uk_text (Ukrainian text, English speech) from $HP2_LANG, else
//     launcher.toml [launcher] language (the launcher window's choice), else en (owner 2026-11-06:
//     English by default, the choice is remembered);
//   - the launcher window (Resources/launcher/launcher.js): language and Play; not in test runs
//     (HP2_BACKGROUND / HP2_NO_UI) unless HP2_LAUNCHER_AUTO answers it; HP2_LAUNCHER=0 skips it;
//   - a bundle without game data (the DMG build, tools/make_dmg.sh): the disc copy lives next to the
//     player folder (<root>/../data); on the first run the player picks the disc image
//     (HP2_IMPORT_ISO skips the dialog) and Resources/import/import_disc.py extracts it there; a
//     bundle without game code builds it on the player's Mac (Resources/build/build_game.sh, CLT);
//   - the game is Contents/MacOS/hp2-bin ("hp2" would be this file, HP2, on a case-insensitive disk);
//   - screen default unless HP2_TEST_PROFILE=1: HP2_WIDESCREEN=1 (16:10);
//   - the log gets "=== start pid N ===": tools/app_smoke.sh waits for that pid to end.
// Every variable already in the environment wins (open --env, tests).
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef HP2_PLAYER_ROOT
#define HP2_PLAYER_ROOT ""
#endif
#define LOG_CAP (50LL << 20)  // bytes per log file
#define LOG_KEEP 5            // hp2.log + hp2.log.1 .. .5: every run starts a new hp2.log

static void mkdirs(const char* path) {
  char p[PATH_MAX];
  snprintf(p, sizeof p, "%s", path);
  for (char* s = p + 1; *s; s++)
    if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
  mkdir(p, 0755);
}

static void setdef(const char* k, const char* v) { setenv(k, v, 0); }

static char real_home[PATH_MAX] = "/tmp";

// <file> [launcher] <key> = "value" (or bare value); 0 if absent.
static int read_setting(const char* file, const char* key, char* out, size_t n) {
  FILE* f = fopen(file, "r");
  if (!f) return 0;
  char line[512];
  int in = 0, found = 0;
  size_t kl = strlen(key);
  while (fgets(line, sizeof line, f)) {
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '[') { in = strncmp(s, "[launcher]", 10) == 0; continue; }
    if (!in || strncmp(s, key, kl) || (s[kl] != ' ' && s[kl] != '=')) continue;
    char* v = strchr(s, '=');
    if (!v) continue;
    v++;
    while (*v == ' ' || *v == '"') v++;
    size_t i = 0;
    while (v[i] && v[i] != '"' && v[i] != ' ' && v[i] != '#' && v[i] != '\n' && i + 1 < n) out[i] = v[i], i++;
    out[i] = 0;
    found = 1;
  }
  fclose(f);
  return found;
}

static int ui_uk = 0;  // HP2_UI_LANG=uk: installer and alerts in Ukrainian (default English)
#define TR(en, uk) (ui_uk ? (uk) : (en))

static void alert(const char* msg) {
  printf("launcher: alert: %s\n", msg);
  fflush(stdout);
  if (getenv("HP2_NO_UI")) return;  // tests
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "/usr/bin/osascript -e 'display alert \"HP2\" message \"%s\"' >/dev/null 2>&1", msg);
  system(cmd);
}

// hp2.log -> hp2.log.1 -> … -> hp2.log.LOG_KEEP (the oldest is dropped). Each run gets a fresh
// hp2.log, so the log of a crashed run survives the next start (owner 2026-11-06: the Whomping
// Willow log was lost to a later run).
static void rotate(const char* log) {
  char a[PATH_MAX], b[PATH_MAX];
  for (int i = LOG_KEEP - 1; i >= 1; i--) {
    snprintf(a, sizeof a, "%s.%d", log, i);
    snprintf(b, sizeof b, "%s.%d", log, i + 1);
    rename(a, b);
  }
  snprintf(a, sizeof a, "%s.1", log);
  rename(log, a);
}

// The capped log: reads the game's stdout+stderr from fd `in` until EOF, appends to `log`, and
// moves it to <log>.1 when it reaches LOG_CAP. Runs in a forked child; never returns.
static void log_pump(int in, const char* log) {
  signal(SIGINT, SIG_IGN);  // the game handles Ctrl-C; the pump ends at EOF
  int fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
  struct stat st;
  long long size = (fd >= 0 && fstat(fd, &st) == 0) ? st.st_size : 0;
  char buf[65536];
  for (;;) {
    ssize_t n = read(in, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    if (fd >= 0 && size + n > LOG_CAP) {
      close(fd);
      rotate(log);
      fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      size = 0;
      if (fd >= 0) size = dprintf(fd, "launcher: log rotated at %lld MB (previous part: hp2.log.1)\n", LOG_CAP >> 20);
    }
    if (fd >= 0 && write(fd, buf, (size_t)n) == n) size += n;
  }
  _exit(0);
}

// First run of the DMG installer: the game code is built on this Mac. Copies this bundle to the
// target app (HP2_INSTALL_APP, default ~/Desktop/HP2.app), then Resources/build/build_game.sh lifts
// the player's default.xbe, compiles it with Apple Command Line Tools' clang and links
// <target>/Contents/MacOS/hp2-bin; a progress window (progress.js, JXA) shows its progress file.
static int build_game(const char* res, const char* data, const char* target) {
  char cmd[PATH_MAX * 4], prog[PATH_MAX], self[PATH_MAX];
  snprintf(self, sizeof self, "%s/..", res);  // .../Contents
  snprintf(cmd, sizeof cmd, "/bin/rm -rf '%s' && /usr/bin/ditto '%s/..' '%s' && /bin/rm -rf '%s/Contents/Resources/build' "
           "'%s/Contents/Resources/import'", target, self, target, target, target);
  if (system(cmd) != 0) { alert(TR("Could not create HP2.app.", "Не вдалося створити HP2.app.")); return 1; }
  snprintf(prog, sizeof prog, "%s/build-progress.txt", data);
  unlink(prog);
  char work[PATH_MAX];  // lifted sources + objects: next to the data, never inside the app
  snprintf(work, sizeof work, "%s/build-work", data);
  setdef("HP2_BUILD_WORK", work);
  if (!getenv("HP2_NO_UI")) {
    snprintf(cmd, sizeof cmd, "/usr/bin/osascript -l JavaScript '%s/build/progress.js' '%s' >/dev/null 2>&1 &", res, prog);
    system(cmd);
  }
  printf("launcher: building the game code -> %s\n", target);
  fflush(stdout);
  snprintf(cmd, sizeof cmd, "/bin/bash '%s/build/build_game.sh' '%s/build' '%s/game/extracted' '%s/Contents/MacOS/hp2-bin' '%s'",
           res, res, data, target, prog);
  if (system(cmd) != 0) {
    char msg[512] = "", line[512];
    FILE* f = fopen(prog, "r");
    while (f && fgets(line, sizeof line, f)) snprintf(msg, sizeof msg, "%s", line);
    if (f) fclose(f);
    msg[strcspn(msg, "\n\"'")] = 0;
    char a[700];
    snprintf(a, sizeof a, TR("Build failed: %s", "Гру не зібрано: %s"), *msg ? msg : "?");
    alert(a);
    return 1;
  }
  snprintf(cmd, sizeof cmd, "/usr/bin/codesign --force --deep -s - '%s' >/dev/null 2>&1", target);
  system(cmd);
  return 0;
}

// First run of a bundle without game data: the player's own disc image -> <data>.
static int import_disc(const char* res, const char* data) {
  char iso[PATH_MAX] = "", cmd[PATH_MAX * 3];
  if (getenv("HP2_IMPORT_ISO")) {
    snprintf(iso, sizeof iso, "%s", getenv("HP2_IMPORT_ISO"));
  } else {
    snprintf(cmd, sizeof cmd, "/usr/bin/osascript -e 'POSIX path of (choose file with prompt \"%s\")'",
             TR("Choose your Harry Potter and the Chamber of Secrets disc image (Xbox, .iso)",
                "Оберіть образ диска Harry Potter and the Chamber of Secrets (Xbox, .iso)"));
    FILE* p = popen(cmd, "r");
    if (p) { if (!fgets(iso, sizeof iso, p)) iso[0] = 0; pclose(p); }
    iso[strcspn(iso, "\n")] = 0;
  }
  if (!*iso) return 1;
  mkdirs(data);
  printf("launcher: importing the disc %s -> %s\n", iso, data);
  fflush(stdout);
  snprintf(cmd, sizeof cmd, "/usr/bin/python3 -I '%s/import/import_disc.py' '%s' '%s'", res, iso, data);
  if (system(cmd) != 0) {
    alert(TR("Disc import failed (python3 is needed: xcode-select --install).",
             "Не вдалося імпортувати диск (потрібен python3: xcode-select --install)."));
    return 1;
  }
  char reg[64] = "", rf[PATH_MAX];
  snprintf(rf, sizeof rf, "%s/region.txt", data);
  FILE* f = fopen(rf, "r");
  if (f) { if (!fgets(reg, sizeof reg, f)) reg[0] = 0; fclose(f); }
  if (*reg && !strstr(reg, "US")) alert(TR("EU disc (PAL): untested.", "Європейський диск (PAL): не перевірено."));
  return 0;
}

int main(int argc, char** argv) {
  (void)argc;
  char exe[PATH_MAX], real[PATH_MAX];
  uint32_t sz = sizeof exe;
  if (_NSGetExecutablePath(exe, &sz) != 0 || !realpath(exe, real)) return 70;
  char here[PATH_MAX];
  snprintf(here, sizeof here, "%s", dirname(real));  // .../HP2.app/Contents/MacOS

  char root[PATH_MAX] = HP2_PLAYER_ROOT;
  if (getenv("HP2_PLAYER_ROOT") && *getenv("HP2_PLAYER_ROOT")) snprintf(root, sizeof root, "%s", getenv("HP2_PLAYER_ROOT"));
  struct stat st;
  char parent[PATH_MAX];
  snprintf(parent, sizeof parent, "%s", root);
  if (getenv("HOME")) snprintf(real_home, sizeof real_home, "%s", getenv("HOME"));
  if (!*root || stat(dirname(parent), &st) != 0)  // baked folder gone (another Mac): user library
    snprintf(root, sizeof root, "%s/Library/Application Support/HP2/player", real_home);
  // Under ~/Desktop macOS privacy (TCC) may refuse the baked folder: then the user library.
  char home[PATH_MAX], logs[PATH_MAX], log[PATH_MAX], sup[PATH_MAX];
  int fd = -1, denied = 0;
  for (int attempt = 0; attempt < 2 && fd < 0; attempt++) {
    if (attempt) {
      denied = errno;
      snprintf(root, sizeof root, "%s/Library/Application Support/HP2/player", real_home);
    }
    snprintf(home, sizeof home, "%s/home", root);
    snprintf(logs, sizeof logs, "%s", getenv("HP2_LOG_DIR") && *getenv("HP2_LOG_DIR") ? getenv("HP2_LOG_DIR") : "");
    if (!*logs) snprintf(logs, sizeof logs, "%s/logs", root);
    snprintf(sup, sizeof sup, "%s/Library/Application Support/HP2", home);
    mkdirs(sup);
    mkdirs(logs);
    snprintf(log, sizeof log, "%s/hp2.log", logs);
    fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
  }
  setenv("HOME", home, 1);
  setdef("HP2_LOG_DIR", logs);
  setdef("HP2_CRASH_DIR", logs);
  setdef("HP2_KTRACE", "0");
  if (fd >= 0) {
    if (fstat(fd, &st) == 0 && st.st_size > 0) {  // the previous run's log -> hp2.log.1
      close(fd);
      rotate(log);
      fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
  }
  if (fd >= 0) {
    time_t t = time(NULL);
    dprintf(fd, "=== start pid %d %s", (int)getpid(), ctime(&t));
    dprintf(fd, "launcher: %s, player files %s\n", real, root);
    if (denied) dprintf(fd, "launcher: baked player folder refused (%s): user library used\n", strerror(denied));
    close(fd);
    int p[2];
    if (pipe(p) == 0) {
      pid_t c = fork();
      if (c == 0) { close(p[1]); log_pump(p[0], log); }
      close(p[0]);
      if (c > 0) { dup2(p[1], 1); dup2(p[1], 2); }
      close(p[1]);
    }
  }
  setvbuf(stdout, NULL, _IOLBF, 0);

  char settings[PATH_MAX], lang[16] = "en";
  snprintf(settings, sizeof settings, "%s/launcher.toml", sup);
  {  // interface language of the installer and the alerts: the launcher's saved choice, else English
    char ul[16] = "en";
    if (getenv("HP2_UI_LANG") && *getenv("HP2_UI_LANG")) snprintf(ul, sizeof ul, "%s", getenv("HP2_UI_LANG"));
    else if (getenv("HP2_LANG") && *getenv("HP2_LANG")) snprintf(ul, sizeof ul, "%s", getenv("HP2_LANG"));
    else read_setting(settings, "language", ul, sizeof ul);
    ui_uk = strncmp(ul, "uk", 2) == 0;
    setenv("HP2_UI_LANG", ui_uk ? "uk" : "en", 1);
  }
  char res[PATH_MAX], buf[PATH_MAX];
  snprintf(res, sizeof res, "%s/../Resources", here);
  snprintf(buf, sizeof buf, "%s/game/extracted/default.xbe", res);
  const char* env_data = getenv("HP2_DATA");
  if (env_data && !*env_data) unsetenv("HP2_DATA"), env_data = NULL;  // empty = not set
  if (!env_data && stat(buf, &st) != 0) {
    char data[PATH_MAX];
    snprintf(parent, sizeof parent, "%s", root);
    snprintf(data, sizeof data, "%s/data", dirname(parent));
    snprintf(buf, sizeof buf, "%s/game/extracted/default.xbe", data);
    if (stat(buf, &st) != 0) {
      char clt[PATH_MAX * 2];  // DMG installer: python3 (import) and clang (build) come with CLT
      snprintf(clt, sizeof clt, "%s/build/clt.sh", res);
      if (stat(clt, &st) == 0) {
        snprintf(clt, sizeof clt, "/bin/bash '%s/build/clt.sh'", res);
        if (system(clt) != 0) {
          alert(TR("Command Line Tools are not installed (xcode-select --install).",
                   "Command Line Tools не встановлено (xcode-select --install)."));
          return 74;
        }
      }
      if (import_disc(res, data) != 0) return 72;
    }
    snprintf(res, sizeof res, "%s", data);
  }
  setdef("HP2_DATA", res);

  // The launcher window (tools/launcher/launcher.js): language, Play.
  const char* lw = getenv("HP2_LAUNCHER");
  int ui = !getenv("HP2_NO_UI") && !getenv("HP2_BACKGROUND");
  if ((!lw || strcmp(lw, "0") != 0) && (ui || getenv("HP2_LAUNCHER_AUTO"))) {
    char js[PATH_MAX], art[PATH_MAX], cmd[PATH_MAX * 4], ans[64] = "";
    snprintf(js, sizeof js, "%s/../Resources/launcher/launcher.js", here);
    snprintf(art, sizeof art, "%s/../Resources/launcher/art.png", here);
    if (stat(art, &st) != 0) snprintf(art, sizeof art, "%s/launcher-art.png", res);
    char me[16];
    snprintf(me, sizeof me, "%d", (int)getpid());  // the game after execv: launcher.js yields activation to it
    setenv("HP2_LAUNCHER_OWNER", me, 1);
    if (stat(js, &st) == 0) {
      snprintf(cmd, sizeof cmd, "/usr/bin/osascript -l JavaScript '%s' '%s' '%s'", js, settings,
               stat(art, &st) == 0 ? art : "");
      FILE* p = popen(cmd, "r");
      if (p) { if (!fgets(ans, sizeof ans, p)) ans[0] = 0; pclose(p); }
      ans[strcspn(ans, "\n")] = 0;
      printf("launcher: window -> %s\n", *ans ? ans : "(no answer: play)");
      if (strcmp(ans, "quit") == 0) return 0;
    }
  }
  if (getenv("HP2_LANG") && *getenv("HP2_LANG")) snprintf(lang, sizeof lang, "%s", getenv("HP2_LANG"));
  else read_setting(settings, "language", lang, sizeof lang);
  if (strcmp(lang, "uk") != 0 && strcmp(lang, "uk_text") != 0) snprintf(lang, sizeof lang, "en");
  setenv("HP2_LANG", lang, 1);
  setenv("HP2_UI_LANG", strncmp(lang, "uk", 2) == 0 ? "uk" : "en", 1);
  // Overlays (_UK, _UKT, fonts) live next to the game data: Resources/mods in the owner's app, the
  // player's data/mods in the DMG build (import_disc.py builds them from the disc). The HD packs
  // always ship in the app (owner 2026-11-06), so they are named explicitly; background test runs
  // (HP2_BACKGROUND) keep the original look unless a test names a pack itself.
  snprintf(buf, sizeof buf, "%s/mods", getenv("HP2_DATA"));
  setdef("HP2_MODS", buf);
  if (!getenv("HP2_BACKGROUND")) {
    const char* hd[][2] = {{"HP2_HDUI", "hd_ui"}, {"HP2_HDWORLD", "hd_world"}};
    for (int i = 0; i < 2; i++) {
      snprintf(buf, sizeof buf, "%s/../Resources/mods/%s", here, hd[i][1]);
      if (stat(buf, &st) == 0) setdef(hd[i][0], buf);
    }
  }
  const char* tp = getenv("HP2_TEST_PROFILE");
  if (!tp || strcmp(tp, "1") != 0) setdef("HP2_WIDESCREEN", "1");
  printf("launcher: language %s, data %s, mods %s\n", lang, getenv("HP2_DATA"), getenv("HP2_MODS"));

  char game[PATH_MAX];
  snprintf(game, sizeof game, "%s/hp2-bin", here);
  char kit[PATH_MAX];
  snprintf(kit, sizeof kit, "%s/../Resources/build/build_game.sh", here);
  if (stat(game, &st) != 0 && stat(kit, &st) == 0) {  // the DMG installer: no game code inside
    char target[PATH_MAX], r2[PATH_MAX];
    if (getenv("HP2_INSTALL_APP")) snprintf(target, sizeof target, "%s", getenv("HP2_INSTALL_APP"));
    else snprintf(target, sizeof target, "%s/Desktop/HP2.app", real_home);
    snprintf(game, sizeof game, "%s/Contents/MacOS/hp2-bin", target);
    snprintf(r2, sizeof r2, "%s/../Resources", here);
    if (stat(game, &st) != 0 && build_game(r2, getenv("HP2_DATA"), target) != 0) return 73;
  }
  fflush(stdout);
  argv[0] = game;
  execv(game, argv);
  fprintf(stderr, "launcher: cannot start %s: %s\n", game, strerror(errno));
  return 71;
}
