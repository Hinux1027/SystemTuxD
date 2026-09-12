#include<sys/mount.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<sys/wait.h>
#include<fcntl.h>
#include<sys/errno.h>
#include<unistd.h>
#include "SysMount.hpp"
#include "ServiceStart.hpp"
#include "Shutdown.hpp"
int main(){
    SysMount();
    ServiceStart();
    return 0;
}
