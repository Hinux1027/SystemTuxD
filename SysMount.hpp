#include <sys/mount.h>
#include <sys/stat.h>
#include <iostream>
#include <cstring>
#include <cerrno>
#include <string>
#include <fstream>
#include <vector>

void SysMount() {
    auto ensure_dir = [](const std::string &d)->bool{
        struct stat st;
        if(stat(d.c_str(), &st) == 0){
            if(S_ISDIR(st.st_mode)) return true;
            std::cerr << "\033[31m[EE] \033[0m"<<"Path exists and is not a directory: " << d << std::endl;
            return false;
        }
        if(mkdir(d.c_str(), 0755) != 0){
            if(errno == EEXIST) return true;
            std::cerr << "\033[31m[EE] \033[0m Failed to create " << d << ": " << std::strerror(errno) << std::endl;
            return false;
        }
        return true;
    };

    auto is_mounted = [](const std::string &target)->bool{
        std::ifstream f("/proc/self/mountinfo");
        if(!f) return false;
        std::string line;
        while(std::getline(f, line)){
            // mountinfo: fields ... mount_point ...
            // simple search for target is sufficient for init usage
            if(line.find(" " + target + " ") != std::string::npos ||
               line.find(" " + target + "\t") != std::string::npos) return true;
        }
        return false;
    };

    // helper to mount with checks
    auto try_mount = [&](const char *source, const char *target, const char *fstype, unsigned long flags, const char *data)->bool{
        std::string t(target);
        if(!ensure_dir(t)) return false;
        if(is_mounted(t)){
            std::cout << "\033[34m[II] \033[0m Already mounted: " << t << std::endl;
            return true;
        }
        if(mount(source, target, fstype, flags, data) != 0){
            std::cerr << "\033[31m[EE] \033[0m Failed to mount " << t << ": " << std::strerror(errno) << std::endl;
            return false;
        }
        std::cout << "\033[32m[FF] \033[0m Mounted " << t << std::endl;
        return true;
    };

    // 用 devtmpfs 提供基础设备节点
    try_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID | MS_NODEV, nullptr);

    // 挂载虚拟文件系统
    try_mount(nullptr, "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, nullptr);
    try_mount(nullptr, "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, nullptr);

    // 创建 /sys 子目录（在挂载后）
    auto ensure_sys_subdir = [](const std::string &subdir){
        std::string path = "/sys/" + subdir;
        mkdir(path.c_str(), 0755);
        // 有些子目录需要挂载
    };

    // 挂载 /sys/kernel/security（可选，某些功能需要）
    ensure_dir("/sys/kernel/security");
    try_mount("securityfs", "/sys/kernel/security", "securityfs", 0, nullptr);

    // 挂载 /sys/fs/cgroup（systemd 需要）
    ensure_dir("/sys/fs/cgroup");
    try_mount(nullptr, "/sys/fs/cgroup", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");

    // 安全 tmpfs
    try_mount(nullptr, "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    try_mount(nullptr, "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    try_mount("devpts", "/dev/pts", "devpts", 0, nullptr);

    // /run 和 /run/lock
    try_mount(nullptr, "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
    if(ensure_dir(std::string("/run/lock"))){
        try_mount(nullptr, "/run/lock", "tmpfs", MS_NOSUID | MS_NODEV, nullptr);
    }

    std::cout << "\033[32m[FF] \033[0m Mount operations completed." << std::endl;
}