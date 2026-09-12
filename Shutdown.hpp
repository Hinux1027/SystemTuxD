#ifndef SYSTEMTUXD_SHUTDOWN_HPP
#define SYSTEMTUXD_SHUTDOWN_HPP

#include<signal.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<ctype.h>
#include<limits.h>
#include<errno.h>
#include<unistd.h>
#include<sys/wait.h>
#include<sys/stat.h>
#include<dirent.h>
#include<algorithm>
#include<vector>
#include<string>
#include<sys/reboot.h>
inline volatile sig_atomic_t g_should_exit = 0;
inline volatile sig_atomic_t g_is_shutting_down = 0;
inline std::vector<pid_t> g_service_pids;

inline void HandleShutdownSignal(int){
    g_should_exit = 1;
    g_is_shutting_down = 1;
}

inline bool ShouldExit(){
    return g_should_exit != 0;
}

inline bool IsShuttingDown(){
    return g_is_shutting_down != 0;
}

inline void RegisterServicePid(pid_t pid){
    if(pid > 0){
        g_service_pids.push_back(pid);
    }
}

inline void UnregisterServicePid(pid_t pid){
    for(auto it = g_service_pids.begin(); it != g_service_pids.end(); ++it){
        if(*it == pid){
            g_service_pids.erase(it);
            break;
        }
    }
}

inline bool LoadServiceList(const char *conf, std::vector<std::string> &out){
    FILE *f = fopen(conf, "r");
    if(!f) return false;

    char buf[1024];
    while(fgets(buf, sizeof(buf), f)){
        char *start = buf;
        while(*start && isspace((unsigned char)*start)) ++start;
        if(*start == '\0' || *start == '#') continue;

        char *end = start + strlen(start);
        while(end > start && isspace((unsigned char)*(end - 1))){
            --end;
        }
        *end = '\0';

        if(start[0] != '\0'){
            out.emplace_back(start);
        }
    }

    fclose(f);
    return true;
}

inline std::string BuildServicePath(const std::string &service){
    char path[PATH_MAX];
    if(snprintf(path, sizeof(path), "/etc/tuxrc/rc/%s", service.c_str()) >= (int)sizeof(path)){
        return std::string();
    }
    return std::string(path);
}

inline bool RunShutdownSequence(){
    // First, try to run stop hooks from /etc/tux.d in reverse order if present
    auto load_dir = [&](const char *dirpath, std::vector<std::string> &out){
        DIR *d = opendir(dirpath);
        if(!d) return false;
        struct dirent *ent;
        std::vector<std::string> names;
        while((ent = readdir(d))){
            std::string fn(ent->d_name);
            if(fn == "." || fn == "..") continue;
            std::string full = std::string(dirpath) + "/" + fn;
            struct stat st;
            if(stat(full.c_str(), &st) != 0) continue;
            if(!S_ISREG(st.st_mode)) continue;
            if(access(full.c_str(), X_OK) != 0) continue;
            names.push_back(fn);
        }
        closedir(d);
        if(names.empty()) return false;
        std::sort(names.begin(), names.end(), [&](const std::string &a, const std::string &b){
            auto get_prefix = [](const std::string &s)->std::pair<int, std::string>{
                size_t i = 0;
                while(i < s.size() && (s[i] >= '0' && s[i] <= '9')) ++i;
                if(i == 0) return {0, s};
                int num = std::atoi(s.substr(0, i).c_str());
                std::string rest = s.substr(i);
                if(!rest.empty() && (rest[0] == '-' || rest[0] == '_')) rest = rest.substr(1);
                return {num, rest};
            };
            auto pa = get_prefix(a);
            auto pb = get_prefix(b);
            if(pa.first != pb.first) return pa.first < pb.first;
            return pa.second < pb.second;
        });
        out = std::move(names);
        return true;
    };

    std::vector<std::string> shutdown_services;
    if(load_dir("/etc/tux.d", shutdown_services)){
        // run in reverse order
        for(auto it = shutdown_services.rbegin(); it != shutdown_services.rend(); ++it){
            std::string path = std::string("/etc/tux.d/") + *it;
            pid_t child = fork();
            if(child < 0) continue;
            if(child == 0){
                execl(path.c_str(), path.c_str(), "stop", (char*)NULL);
                _exit(127);
            }
            int status = 0;
            waitpid(child, &status, 0);
        }
    } else {
        // fallback to legacy shutdown.conf
        if(LoadServiceList("/etc/tuxrc/shutdown.conf", shutdown_services)){
            for(const auto &svc : shutdown_services){
                std::string path = BuildServicePath(svc);
                if(access(path.c_str(), X_OK) != 0) continue;
                pid_t child = fork();
                if(child < 0) continue;
                if(child == 0){
                    execl(path.c_str(), path.c_str(), "stop", (char*)NULL);
                    _exit(127);
                }
                int status = 0;
                waitpid(child, &status, 0);
            }
        }
    }

    // Send SIGTERM to running services in reverse registration (stop order)
    for(auto it = g_service_pids.rbegin(); it != g_service_pids.rend(); ++it){
        pid_t pid = *it;
        if(pid > 0){
            kill(pid, SIGTERM);
        }
    }

    // Wait up to timeout for processes to exit. Default timeout = 5 seconds.
    const int total_wait_ms = 5000;
    const int step_ms = 100;
    int waited = 0;
    while(waited < total_wait_ms){
        bool still_alive = false;
        std::vector<pid_t> remaining;
        for(pid_t pid : g_service_pids){
            if(pid <= 0) continue;
            int status = 0;
            pid_t res = waitpid(pid, &status, WNOHANG);
            if(res == 0){
                still_alive = true;
                remaining.push_back(pid);
            }
        }
        g_service_pids.swap(remaining);
        if(!still_alive) break;
        usleep(step_ms * 1000);
        waited += step_ms;
    }

    // Force kill remaining
    for(pid_t pid : g_service_pids){
        if(pid > 0){
            kill(pid, SIGKILL);
        }
    }

    // Reap all
    for(pid_t pid : g_service_pids){
        if(pid > 0){
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
    g_service_pids.clear();
    sync();
    reboot(RB_POWER_OFF);
    std::cerr << "WARNING: reboot() failed, trying sysrq..." << std::endl;
    system("echo \"o\" > /proc/sysrq-trigger");
    return true;
}

#endif
