#pragma once
#include <iostream>
#include <string>


void printMenu() {
	const std::string RED_TEXT = "\033[31m";
	const std::string GREEN_TEXT = "\033[32m";
	const std::string YELLOW_TEXT = "\033[33m";
	const std::string BLUE_TEXT = "\033[34m";
	const std::string CYAN_TEXT = "\033[36m";
	const std::string RESET_TEXT = "\033[0m";

	std::cout << BLUE_TEXT << R"(
	  ___  ____   ___ _____ _____  _______   __
	 / ___/ ___| / _ \|  _ \| ____/ ____\ \ / /
	| |   \___ \| | | | |_) |  _| \___ \ \ V / 
	| |___ ___) | |_| |  __/| |___ ___) | | |  
	 \____|____/ \___/|_|   |_____|____/  |_|
		
	)" << std::endl;

	std::cout << GREEN_TEXT << "Welcome to the OS Emulator!" << std::endl;
	std::cout << YELLOW_TEXT << "Type 'exit' to quit the emulator, 'clear' to clear the screen" << std::endl;
	std::cout << YELLOW_TEXT << "\n**IMPORTANT: Type 'initialize to load config and start system**\n" << RESET_TEXT << std::endl;

}

void commandInput(std::string& command) {
	std::cout << "Enter your command: ";
	std::cin >> command;
}

