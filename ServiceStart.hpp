#include<sys/mount.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<fcntl.h>
#include<sys/errno.h>
#include<unistd.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<limits.h>
#include<ctype.h>
#include<signal.h>
#include<vector>
#include<string>
#include"Shutdown.hpp"
#include<pwd.h>
#include<unordered_map>
#include<chrono>
#include<algorithm>
#include<fstream>
#include<iostream>
#include<dirent.h>
#include<regex>

static volatile sig_atomic_t g_sigchld_flag = 0;

static void HandleChildSignal(int){
    g_sigchld_flag = 1;
}

struct ServiceEntry{
    std::string name;
    std::string path;
    enum RestartPolicy{ RP_NEVER=0, RP_ON_FAILURE=1, RP_ALWAYS=2 } restart = RP_NEVER;
    int restart_count = 0;
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
};

static ServiceEntry::RestartPolicy ParseRestartPolicy(const std::string &path){
    std::ifstream f(path);
    if(!f) return ServiceEntry::RP_NEVER;
    std::string line;
    int lines = 0;
    while(std::getline(f, line) && lines++ < 50){
        // trim
        size_t p = line.find_first_not_of(" \t\r\n");
        if(p==std::string::npos) continue;
        if(line[p] != '#') continue;
        size_t keypos = line.find("TUX-Restart:", p);
        if(keypos!=std::string::npos){
            size_t vpos = keypos + strlen("TUX-Restart:");
            std::string val = line.substr(vpos);
            // trim
            size_t s = val.find_first_not_of(" \t");
            if(s!=std::string::npos) val = val.substr(s);
            size_t e = val.find_last_not_of(" \t\r\n");
            if(e!=std::string::npos) val = val.substr(0, e+1);
            if(val == "always") return ServiceEntry::RP_ALWAYS;
            if(val == "on-failure") return ServiceEntry::RP_ON_FAILURE;
            return ServiceEntry::RP_NEVER;
        }
    }
    return ServiceEntry::RP_NEVER;
}

static void InstallSignalHandlers(){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = HandleShutdownSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);

    struct sigaction sch;
    memset(&sch,0,sizeof(sch));
    sch.sa_handler = HandleChildSignal;
    sigemptyset(&sch.sa_mask);
    sch.sa_flags = SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sch, nullptr);
}

static std::string LogDir(){
    return std::string("/var/log/systemtuxd");
}

// (Removed unused forward declaration for undefined ProcessStartPlaceholder)

// Start services with basic supervision: restart policy, log redirection, anti-flapping
void ServiceStart(){
    InstallSignalHandlers();

    std::vector<std::string> services;
    // Prefer /etc/tux.d/ script directory; fallback to old startup.conf
    auto load_from_dir = [&](const char *dirpath)->bool{
        DIR *d = opendir(dirpath);
        if(!d) return false;
        std::vector<std::string> names;
        struct dirent *ent;
        while((ent = readdir(d))){
            std::string fn(ent->d_name);
            if(fn == "." || fn == "..") continue;
            std::string full = std::string(dirpath) + "/" + fn;
            struct stat st;
            if(stat(full.c_str(), &st) != 0) continue;
            if(!S_ISREG(st.st_mode)) continue;
            if(access(full.c_str(), X_OK) != 0) continue; // only executable
            names.push_back(fn);
        }
        closedir(d);
        if(names.empty()) return false;
        // sort by optional numeric prefix then name
        std::regex r("^(\\d+)[-_](.*)$");
        std::sort(names.begin(), names.end(), [&](const std::string &a, const std::string &b){
            std::smatch ma, mb;
            bool ra = std::regex_match(a, ma, r);
            bool rb = std::regex_match(b, mb, r);
            if(ra && rb){
                long na = std::stol(ma[1]);
                long nb = std::stol(mb[1]);
                if(na != nb) return na < nb;
                return ma[2].str() < mb[2].str();
            }
            if(ra) return true; // numeric prefixes come first
            if(rb) return false;
            return a < b;
        });
        services = std::move(names);
        return true;
    };

    if(!load_from_dir("/etc/tux.d")){
        if(!LoadServiceList("/etc/tuxrc/startup.conf", services)) return;
    }

    // Prepare log dir
    std::string logdir = LogDir();
    mkdir(logdir.c_str(), 0755);

    std::vector<ServiceEntry> entries;
    for(const auto &svc : services){
        ServiceEntry e;
        e.name = svc;
        // if svc looks like a path (contains '/'), use as-is; else prefer /etc/tux.d/<svc> then fallback to old rc path
        if(svc.find('/') != std::string::npos){
            e.path = svc;
        } else {
            std::string candidate = std::string("/etc/tux.d/") + svc;
            if(access(candidate.c_str(), X_OK) == 0){
                e.path = candidate;
            } else {
                e.path = BuildServicePath(svc);
            }
        }
        if(e.path.empty()) continue;
        if(access(e.path.c_str(), X_OK) != 0) continue;
        e.restart = ParseRestartPolicy(e.path);
        entries.push_back(std::move(e));
    }

    std::unordered_map<pid_t, size_t> pid_to_index;

    auto start_one = [&](size_t idx)->pid_t{
        const ServiceEntry &se = entries[idx];
        pid_t pid = fork();
        if(pid < 0) return -1;
        if(pid == 0){
            // child
            // redirect stdout/stderr
            std::string logfile = logdir + "/" + se.name + ".log";
            int fd = open(logfile.c_str(), O_WRONLY|O_CREAT|O_APPEND, 0644);
            if(fd >= 0){
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                if(fd > 2) close(fd);
            }
            std::cout << "\033[32m[FF] \033[0m started " << se.name << '\n';
            execl(se.path.c_str(), se.path.c_str(), (char*)NULL);
            _exit(127);
        }
        // parent
        RegisterServicePid(pid);
        pid_to_index[pid] = idx;
        return pid;
    };

    // Start all services
    for(size_t i=0;i<entries.size();++i){
        start_one(i);
        sleep(1);
    }

    // supervision loop
    while(!ShouldExit()){
        if(g_sigchld_flag){
            g_sigchld_flag = 0;
            int status = 0;
            pid_t pid;
            while((pid = waitpid(-1, &status, WNOHANG)) > 0){
                auto it = pid_to_index.find(pid);
                if(it != pid_to_index.end()){
                    size_t idx = it->second;
                    ServiceEntry &se = entries[idx];
                    UnregisterServicePid(pid);
                    pid_to_index.erase(it);

                    bool failed = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
                    if (failed) {
                        std::cout << "\033[31m[EE] \033[0m" << se.name << " failed" << std::endl;
                    } else {
                        std::cout << "\033[32m[FF] \033[0m" << se.name << " started" << std::endl;
                    }
                    bool should_restart = false;
                    if(!IsShuttingDown()){
                        if(se.restart == ServiceEntry::RP_ALWAYS) should_restart = true;
                        if(se.restart == ServiceEntry::RP_ON_FAILURE && failed) should_restart = true;
                    }

                    if(should_restart){
                        // simple anti-flapping: allow 5 restarts per 60 seconds
                        auto now = std::chrono::steady_clock::now();
                        if(std::chrono::duration_cast<std::chrono::seconds>(now - se.window_start).count() > 60){
                            se.restart_count = 0;
                            se.window_start = now;
                        }
                        se.restart_count++;
                        if(se.restart_count <= 5){
                            pid_t newpid = start_one(idx);
                            if(newpid > 0){
                                // restarted
                            }
                        } else {
                            // give up for now
                        }
                    }
                } else {
                    // unknown child; just reap
                }
            }
        }
        usleep(100000);
    }

    RunShutdownSequence();
}
