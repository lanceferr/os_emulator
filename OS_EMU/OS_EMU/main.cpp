// libraries
#include <iostream>
#include <cstdlib>

// header files
#include "frontend.h"
#include "placeholder.h"

int main() {
	std::string command = "";
	printMenu();


	while (command != "exit") {
		commandInput(command);


		if (command == "clear"){
			std::system("cls");
			printMenu();
		}


		else if (command == "initialize") {initializeCommand(command);}
		else if (command == "screen") {screenCommand(command);}
		else if (command == "scheduler-start") { schedulerStartCommand(command); }
		else if (command == "scheduler-stop") { schedulerStopCommand(command); }
		else if (command == "report-util") { reportUtilCommand(command); }
		else if (command == "exit") {continue;}

		else {
			std::cout << "\nInvalid command, please try again." << std::endl;
		}


	}
	return 0;
}