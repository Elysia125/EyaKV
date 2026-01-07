#include <iostream>
#include "starter/starter.h"
int main(int argc, char **argv)
{
    std::cout << "EyaKV 0.1.0 starting..." << std::endl;
    EyaKVStarter::get_instance().start();
    return 0;
}