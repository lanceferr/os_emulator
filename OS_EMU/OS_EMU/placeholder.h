#pragma once
#include <iostream>
#include <string>

void initializeCommand(std::string command) {
    std::cout << "\ninitialize command recognized. Doing something.\n" << std::endl;
}

void screenCommand(std::string command) {
    std::cout << "\nscreen command recognized. Doing something.\n" << std::endl;
}

void schedulerStartCommand(std::string command) {
    std::cout << "\nscheduler-start command recognized. Doing something.\n" << std::endl;
}

void schedulerStopCommand(std::string command) {
    std::cout << "\nscheduler-stop command recognized. Doing something.\n" << std::endl;
}

void reportUtilCommand(std::string command) {
    std::cout << "\nreport-util command recognized. Doing something.\n" << std::endl;
}
