#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
	char buf[10];
	strlcpy(buf, "foo", sizeof buf);
	return 1;
}
