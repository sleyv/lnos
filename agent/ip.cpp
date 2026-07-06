//#include <stdio.h>
#include <string.h>
#include <string>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include "ip.h"

std::string getip()
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    ioctl(s, SIOCGIFADDR, &ifr);
    close(s);

    struct sockaddr_in *sa = (struct sockaddr_in*)&ifr.ifr_addr;
    std::string ipadress = inet_ntoa(sa->sin_addr);
    // printf("addr = %s\n", inet_ntoa(sa->sin_addr));
    return ipadress;
}

