#include <string.h>
#include "shared.h"

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp("-s", argv[1]) == 0) {
		server_main();
	} else {
		client_main(argc, argv);
	}
	return 0;
}
