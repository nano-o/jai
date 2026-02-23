#include "jai.h"

#include <cassert>
#include <cstring>
#include <print>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sched.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>


path prog;

constexpr const char *kRunRoot = "/run/jai";

struct Config {
  std::string user_;
  uid_t uid_ = -1;
  gid_t gid_ = -1;
  path homepath_;

  Fd homefd_;
  Fd home_jai_fd_;
  Fd run_jai_fd_;

  void init();
  Fd makemount();
  Fd makens();

  Fd make_overlay();

  [[nodiscard]] Defer asuser();
  int homejai();
  int runjai();
};

// Conservatively fails if file is not a regular file or cannot be
// statted for any reason.
bool
is_fd_at_path(int targetfd, int dfd, path file, bool follow = false,
              struct stat *sbout = nullptr)
{
  struct stat sbtmp, sbpath;
  if (!sbout)
    sbout = &sbtmp;
  if (fstat(targetfd, sbout))
    syserr("fstat");
  if (fstatat(dfd, file.c_str(), &sbpath, follow ? 0 : AT_SYMLINK_NOFOLLOW))
    return false;
  return sbout->st_dev == sbpath.st_dev && sbout->st_ino == sbpath.st_ino;
}

// Open an exclusive lockfile to guard one-time setup.  Might fail, in
// which case re-check the need for setup and try again.
Fd
openlock(int dfd, path file)
{
  assert(!file.empty());

  Fd fd = openat(dfd, file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (fd) {
    if (!flock(*fd, LOCK_EX | LOCK_NB)) {
      struct stat sb;
      if (!is_fd_at_path(*fd, dfd, file, false, &sb))
        // Someone may have unlinked after completing setup; fail and
        // expect the invoker to call again if setup isn't complete.
        fd.reset();
      if (!S_ISREG(sb.st_mode))
        err("{}: expected regular file", file.string());
      return fd;
    }
    if (errno != EWOULDBLOCK && errno != EINTR)
      syserr(R"(flock("{}", LOCK_EX|LOCK_NB))", file.string());
    // We failed, but delay returning until lock is released, at which
    // point setup will likely be complete.
    if (flock(*fd, LOCK_SH) && errno != EINTR)
      syserr(R"(flock("{}", LOCK_SH))", file.string());
    fd.reset();
    return fd;
  }
  if (errno != ENOENT)
    syserr(R"(open("{}"))", file.string());

  path parent = file.parent_path();
  const char *pp = parent.empty() ? "." : parent.c_str();
  fd.reset(openat(dfd, pp, O_RDWR | O_TMPFILE | O_CLOEXEC, 0600));
  if (!fd)
    syserr(R"(openat("{}", O_RDWR|O_TMPFILE))", pp);
  if (flock(*fd, LOCK_EX | LOCK_NB))
    // It's a temp file so should be impossible for anyone else to lock it
    syserr("flock(O_TMPFILE)");
  if (linkat(*fd, "", dfd, file.c_str(), AT_EMPTY_PATH)) {
    if (errno != EEXIST)
      syserr(R"(linkat("{}"))", file.string());
    fd.reset();
  }
  return fd;
}

enum class FollowLinks {
  kNoFollow = 0,
  kFollow = 1,
};
using enum FollowLinks;

Fd
ensure_dir(int dfd, path p, mode_t perm, FollowLinks follow)
{
  assert(!p.empty());

  Fd fd;
  int flag = follow == kFollow ? 0 : O_NOFOLLOW;
  for (auto component = p.begin(); component != p.end();) {
    if (Fd nfd = openat(dfd, component->c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | flag)) {
      dfd = *(fd = std::move(nfd));
      ++component;
    }
    else if (errno != ENOENT)
      syserr(R"(ensure_dir("{}"): open("{}"))", p.string(),
             component->string());
    else if (mkdirat(dfd, component->c_str(), perm) && errno != EEXIST)
      syserr(R"(ensure_dir("{}"): mkdir("{}"))", p.string(),
             component->string());
    // Don't advance iterator; want to open directory we just created
  }

  struct stat sb;
  if (fstat(*fd, &sb))
    syserr(R"(fstat("{}"))", p.string());
  if (auto euid = geteuid(); sb.st_uid != euid)
    err("{}: has uid {} should have {}", p.string(), sb.st_uid, euid);
  if (auto m = sb.st_mode & perm; m != (sb.st_mode & 07777) && fchmod(*fd, m))
    syserr(R"(fchmod("{}", {:o}))", p.string(), m);
  return fd;
}

bool
dir_empty(int dirfd)
{
  int fd = dup(dirfd);
  if (fd < 0)
    syserr("dup");
  auto dir = fdopendir(fd);
  if (!dir) {
    close(fd);
    syserr("fdopendir");
  }
  Defer cleanup([dir] { closedir(dir); });

  while (auto de = readdir(dir))
    if (de->d_name[0] != '.' ||
        (de->d_name[1] != '\0' &&
         (de->d_name[1] != '.' || de->d_name[2] != '\0')))
      return false;
  return true;
}

void
Config::init()
{
  char buf[512];
  struct passwd pwbuf, *pw{};

  auto realuid = getuid();

  const char *envuser = getenv("SUDO_USER");
  if (realuid == 0 && envuser) {
    if (getpwnam_r(envuser, &pwbuf, buf, sizeof(buf), &pw))
      err("cannot find password entry for user {}", envuser);
  }
  else if (getpwuid_r(realuid, &pwbuf, buf, sizeof(buf), &pw))
    err("cannot find password entry for uid {}", uid_);

  user_ = pw->pw_name;
  uid_ = pw->pw_uid;
  gid_ = pw->pw_gid;
  homepath_ = pw->pw_dir;

  // Paranoia about ptrace, because we will drop privileges to access
  // the file system as the user.
  prctl(PR_SET_DUMPABLE, 0);

  // Set all user permissions except user ID so we can easily drop
  // privileges in asuser.
  if (realuid == 0 && uid_ != 0) {
    if (initgroups(user_.c_str(), gid_))
      syserr("initgroups");
    if (setgid(gid_))
      syserr("setgid");
  }

  auto cleanup = asuser();
  if (!(homefd_ = open(homepath_.c_str(), O_PATH | O_CLOEXEC)))
    syserr("{}", homepath_.string());
}

Fd
Config::makemount()
{
  Fd root =
      open_tree(-1, "/", OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | AT_RECURSIVE);
  if (!root)
    syserr("open_tree(\"/\")");
  struct mount_attr ro_private = {
      .attr_set = MOUNT_ATTR_RDONLY,
      .propagation = MS_PRIVATE,
  };
  if (mount_setattr(*root, "", AT_EMPTY_PATH | AT_RECURSIVE, &ro_private,
                    sizeof(ro_private)) < 0)
    syserr("mount_setattr(root)");

  Fd tmp = fsopen("tmpfs", FSOPEN_CLOEXEC);
  if (!tmp)
    syserr("fsopen(tmpfs)");
  if (fsconfig(*tmp, FSCONFIG_SET_STRING, "size", "10%", 0))
    syserr("fsconfig(size)");
  if (fsconfig(*tmp, FSCONFIG_SET_STRING, "mode", "01777", 0))
    syserr("fsconfig(mode)");
  if (fsconfig(*tmp, FSCONFIG_CMD_CREATE, NULL, NULL, 0))
    syserr("fsconfig(CREATE)");
  Fd mnt = fsmount(*tmp, FSMOUNT_CLOEXEC, 0);
  if (!mnt)
    syserr("fsmount(tmp)");
  if (move_mount(*mnt, "", *root, "tmp", MOVE_MOUNT_F_EMPTY_PATH))
    syserr(R"(move_mount("/tmp"))");
  mnt.reset(open_tree(*root, "tmp", OPEN_TREE_CLONE));
  if (!mnt)
    syserr(R"(open_tree("/mnt/tmp"))");
  if (move_mount(*mnt, "", *root, "var/tmp", MOVE_MOUNT_F_EMPTY_PATH))
    syserr(R"(move_mount("/var/tmp"))");

  return root;
}

Fd
Config::makens()
{
  // Allocate a stack for clone, make sure we reap before freeing it.
  constexpr size_t stack_size = 0x10'0000;
  int pid = -1;
  char *const stack = reinterpret_cast<char *>(malloc(stack_size));
  Defer reap([&pid, stack] {
    if (pid > 0)
      while (waitpid(pid, nullptr, 0) == -1 && errno == EINTR)
        ;
    free(stack);
  });

  int pipefds[2];
  if (pipe(pipefds))
    syserr("pipe");

  pid = clone(
      +[](void *pipefds) -> int {
        auto *fds = reinterpret_cast<int *>(pipefds);
        close(fds[1]);
        char c;
        while (read(fds[0], &c, 1) > 0)
          ;
        return 0;
      },
      stack + stack_size, CLONE_NEWNS | SIGCHLD, pipefds);
  int saved_errno = errno;

  close(pipefds[0]);
  //Fd child_block = pipefds[1];

  if (pid == -1) {
    errno = saved_errno;
    syserr("clone");
  }

  path child_mnt(std::format("/proc/{}/ns/mnt", pid));
  Fd nsmount =
      open_tree(-1, child_mnt.c_str(), OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
  if (!nsmount)
    syserr(R"(open_tree("{}"))", child_mnt.string());

  auto restore = asuser();
  Fd target = openat(homejai(), ".", O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
  if (!target)
    syserr("openat TMPFILE");
  if (flock(*target, LOCK_EX | LOCK_NB))
    syserr("flock TMPFILE");
  if (linkat(*target, "", homejai(), "mnt", AT_EMPTY_PATH)) {
    if (errno == EEXIST)
      return {};
    syserr(R"(linkat(TMPFILE, "{}/.jai/mnt"))", homepath_.string());
  }
  restore.reset();

  // This won't work unless jaifd()/mnt is on an MS_PRIVATE mount
  if (move_mount(AT_FDCWD, child_mnt.c_str(), homejai(), "mnt", 0)) {
    saved_errno = errno;
    auto x = std::format("id; ls -al {}", child_mnt.string());
    system(x.c_str());
    errno = saved_errno;
    syserr(R"(move_mount("{}", "{}/.jai/mnt"))", child_mnt.string(),
           homepath_.string());
  }

  return target;
}

Defer
Config::asuser()
{
  if (!uid_ || geteuid())
    // If target is root or already dropped privileges, do nothing
    return {};
  if (seteuid(uid_))
    syserr("seteuid");
  return Defer{[] { seteuid(0); }};
}

int
Config::homejai()
{
  if (!home_jai_fd_) {
    auto restore = asuser();
    home_jai_fd_ = ensure_dir(*homefd_, ".jai", 0700, kFollow);
  }
  return *home_jai_fd_;
}

Fd
make_mount(int conffd, int attr = MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV,
           decltype(mount_attr::propagation) propagation = MS_PRIVATE)
{
  Fd mnt =
      fsmount(conffd, FSMOUNT_CLOEXEC, MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV);
  if (!mnt)
    syserr("fsmount");
  mount_attr a{.propagation = propagation};
  if (mount_setattr(*mnt, "", AT_EMPTY_PATH | AT_RECURSIVE, &a, sizeof(a)))
    syserr("mount_setattr");
  return mnt;
}

int
Config::runjai()
{
  using namespace std::string_literals;
  if (run_jai_fd_)
    return *run_jai_fd_;

  const auto lockfile = kRunRoot + ".lock"s;
  Fd lock;
  for (;;) {
    struct stat sb;
    if (Fd fd = open(kRunRoot, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        fd && !fstatat(*fd, ".initialized", &sb, AT_SYMLINK_NOFOLLOW))
      return *(run_jai_fd_ = std::move(fd));
    if (lock)
      break;
    lock = openlock(-1, lockfile);
  }

  // Get rid of any partially set up directories
  while (!umount(kRunRoot))
    ;

  Fd jaiconf = fsopen("tmpfs", FSOPEN_CLOEXEC);
  if (!jaiconf)
    syserr(R"(fsopen("tmpfs"))");
  if (fsconfig(*jaiconf, FSCONFIG_SET_STRING, "size", "64M", 0) ||
      fsconfig(*jaiconf, FSCONFIG_SET_STRING, "mode", "0755", 0) ||
      fsconfig(*jaiconf, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0))
    syserr(R"(fsconfig(tmpfs))");

  Fd mfd = make_mount(*jaiconf);
  Fd mp = ensure_dir(-1, kRunRoot, 0755, kFollow);
  if (move_mount(*mfd, "", *mp, "",
                 MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH))
    syserr("move_mount -> \"{}\"", kRunRoot);
  run_jai_fd_ = ensure_dir(-1, kRunRoot, 0755, kFollow);
  mount_attr a{.propagation = MS_PRIVATE};
  if (mount_setattr(*run_jai_fd_, "", AT_EMPTY_PATH | AT_RECURSIVE, &a,
                    sizeof(a)))
    syserr("mount_setattr");
  if (Fd fd = openat(*run_jai_fd_, ".initialized", O_CREAT | O_WRONLY, 0444);
      !fd)
    syserr("creat(\".initialized\")");
  unlink(lockfile.c_str());
  return *run_jai_fd_;
}

std::vector default_blacklist = {
    ".jai",
    ".ssh",
    ".gnupg",
    ".local/share/keyrings",
    ".netrc",
    ".git-credentials",
    ".aws",
    ".azure",
    ".config/gcloud",
    ".config/gh",
    ".config/Keybase",
    ".config/kube",
    ".docker",
    ".password-store",
    ".mozilla",
    ".config/chromium",
    ".config/google-chrome",
    ".config/BraveSoftware",
    ".bash_history",
    ".zsh_history",
};

Fd
make_blacklist(int dfd, path name)
{
  Fd blacklistfd = ensure_dir(dfd, name.c_str(), 0700, kFollow);
  if (!dir_empty(*blacklistfd))
    return blacklistfd;

  for (path p : default_blacklist) {
    try {
      auto d = p.relative_path().parent_path();
      if (!d.empty())
        ensure_dir(*blacklistfd, d, 0700, kNoFollow);
      if (Fd fd = openat(*blacklistfd, p.c_str(),
                         O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
          !fd)
        syserr("{}/{}", name.string(), p.string());
    } catch (const std::exception &e) {
      std::println(stderr, "{}", e.what());
    }
  }

  return blacklistfd;
}

Fd
overlay_mount(int lowerfd, int upperfd, int workfd)
{
  Fd fsfd = fsopen("overlay", FSOPEN_CLOEXEC);
  if (!fsfd)
    syserr(R"(fsopen("overlay"))");

  if (fsconfig(*fsfd, FSCONFIG_SET_FD, "lowerdir+", nullptr, lowerfd) ||
      fsconfig(*fsfd, FSCONFIG_SET_FD, "upperdir", nullptr, upperfd) ||
      fsconfig(*fsfd, FSCONFIG_SET_FD, "workdir", nullptr, workfd))
    syserr("fsconfig(FSCONFIG_SET_FD)");
  if (fsconfig(*fsfd, FSCONFIG_CMD_CREATE, nullptr, nullptr, 0))
    syserr("fsconfig(FSCONFIG_CMD_CREATE)");

  return make_mount(*fsfd);
}

Fd
Config::make_overlay()
{
  auto restore = asuser();
  Fd changes = make_blacklist(homejai(), "changes");
  Fd work = ensure_dir(homejai(), "work", 0700, kFollow);
  restore.reset();

  Fd mnt = overlay_mount(*homefd_, *changes, *work);
  Fd olhome = ensure_dir(runjai(), "home", 0755, kFollow);
  if (move_mount(*mnt, "", *olhome, "",
                 MOVE_MOUNT_F_EMPTY_PATH | MOVE_MOUNT_T_EMPTY_PATH))
    syserr("move_mount");

  olhome = openat(runjai(), "home", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (!olhome)
    syserr("{}/{}/home", kRunRoot, user_);
  return olhome;
}

int
main(int argc, char **argv)
{
  umask(022);
  if (argc > 0)
    prog = argv[0];

  Config conf;
  conf.init();
  conf.make_overlay();
}
