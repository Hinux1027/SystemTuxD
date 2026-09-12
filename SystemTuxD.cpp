#include<sys/mount.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<fcntl.h>
#include<sys/errno.h>
#include<unistd.h>
#include "headers/SysMount.hpp"
#include "headers/ServiceStart.hpp"
#include "headers/Shutdown.hpp"
int main(){
    SysMount();
    ServiceStart();
    return 0;
}