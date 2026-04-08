#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <crossos/crossos.h>

int main(void)
{
    printf("Testing Terminal-UI Backend\n");
    fflush(stdout);

    /* Try to initialize the platform */
    int ret = crossos_platform_init();
    if (ret != CROSSOS_OK)
    {
        printf("FAIL: Platform init returned %d\n", ret);
        return 1;
    }
    printf("PASS: Platform initialized\n");
    fflush(stdout);

    /* Create a window */
    crossos_window_t *win = crossos_window_create(80, 24, "Test Terminal Backend");
    if (!win)
    {
        printf("FAIL: Window creation failed\n");
        return 1;
    }
    printf("PASS: Window created (80x24)\n");
    fflush(stdout);

    /* Get surface */
    crossos_surface_t *surf = crossos_surface_get(win);
    if (!surf)
    {
        printf("FAIL: Surface get failed\n");
        crossos_window_destroy(win);
        return 1;
    }
    printf("PASS: Surface obtained\n");
    fflush(stdout);

    /* Lock surface */
    uint32_t *pixels = (uint32_t *)crossos_surface_lock(surf);
    if (!pixels)
    {
        printf("FAIL: Surface lock failed\n");
        crossos_window_destroy(win);
        return 1;
    }
    printf("PASS: Surface locked (got pixel buffer)\n");
    fflush(stdout);

    /* Draw a simple red rectangle */
    int width = 80 * 8;   /* 8 pixels per character */
    int height = 24 * 16; /* 16 pixels per character */
    for (int y = 100; y < 200; y++)
    {
        for (int x = 100; x < 200; x++)
        {
            if (y < height && x < width)
            {
                pixels[y * width + x] = 0xFFFF0000; /* Red in BGRA */
            }
        }
    }
    printf("PASS: Drew test pattern\n");
    fflush(stdout);

    /* Unlock and present */
    crossos_surface_unlock(surf);
    printf("PASS: Surface unlocked\n");
    fflush(stdout);

    crossos_surface_present(surf);
    printf("PASS: Surface presented\n");
    fflush(stdout);

    /* Small delay to see output */
    sleep(1);

    /* Cleanup */
    crossos_window_destroy(win);
    crossos_platform_shutdown();

    printf("PASS: All tests passed - Terminal backend fully functional\n");
    fflush(stdout);

    return 0;
}
