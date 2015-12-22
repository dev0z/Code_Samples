#include <iostream>
#include <string.h>
#include <sstream>
#include <iomanip>

using namespace std;


void printHexVal(char *mystr)
{
	std::string myStr(mystr);

    std::stringstream ss;
    int n;
    for(int i = 0; i<myStr.length(); ) {
        std::istringstream(myStr.substr(i,2))>>std::hex>>n;
        ss<<(char)n;
        i += 2;
        }

    std::string result = ss.str();

    std::cout<<result;
}

int main()
{
	char str[] ="W68a74 69s20y6fu72 66a76o75r69t65 63o6dp75t69n67 62o6fk20a6ed2fo72 61u74h6fr2c 61n64 77h79?20 41l73o2c 70l65a73e20i6ec6cu64e20t68e20o6ee2dl69n65r20a6ed2fo72 73o75r63e20y6fu20u73e64 74o20d65c69p68e72 74h69s20i6e 79o75r20a70p6ci63a74i6fn"
;
	int i = 0;

	while (str[i])
	{
		if (isalpha(str[i]))
		{
			std::cout<<str[i];

		}
		else if (isspace(str[i]))
		{
			std::cout<<" ";
			
		}
		else {
			char hexVal[2]="";
			strncat(hexVal, &str[i++],1);
			strncat(hexVal, &str[i],1);
			printHexVal(hexVal);
		}
		i++;

	}
	

	return 0;
}