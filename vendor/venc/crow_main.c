#include <stdio.h>
#include <unistd.h>
#include "sample_venc_lib.h"

#define CROW_FPS 25

int main(void)
{
    printf("Crow Camera v0.3 starting...\n");
    printf("Recording continuously at %d FPS.\n", CROW_FPS);
    printf("Press Ctrl+C to stop recording cleanly.\n");

    // Sipeed sample writes test-0.h265 into the current directory.
    chdir("/root");

    remove("/root/test-0.h265");
    remove("/root/crow_test.h265");

    char *argv[] = {
        "crow_camera",

        "-c", "265",
        "-w", "2560",
        "-h", "1440",

        "--testMode=2",
        "--sensorEn=1",
        "--bindmode=1",
        "--numChn=1",

        "--viWidth=2560",
        "--viHeight=1440",
        "--vpssWidth=2560",
        "--vpssHeight=1440",

        "--srcFramerate=25",
        "--framerate=25",

        "-n", "1000000000",

        NULL
    };

    int argc = 0;
    while (argv[argc] != NULL)
        argc++;

    int result = venc_main(argc, argv);

    if (result != 0) {
        printf("Crow Camera failed: %d\n", result);
        return result;
    }

    if (rename("/root/test-0.h265",
               "/root/crow_test.h265") != 0) {
        perror("Could not rename recording");
        return 1;
    }

    printf("Recording saved to /root/crow_test.h265\n");
    return 0;
}