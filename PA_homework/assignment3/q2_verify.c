#include <stdio.h>
#include <string.h>

void outputs(const char *input) {
    char buf[16];

    strcpy(buf, input);
    printf("%s\n", buf);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        outputs(argv[1]);
    }

    return 0;
}
