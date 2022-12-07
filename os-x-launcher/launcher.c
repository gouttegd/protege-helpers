#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>

#include <mach-o/dyld.h>

/* Remove the last n components of a pathname. The pathname
 * is modified in place. Returns 0 if the requested number
 * of components have been removed, -1 otherwise. */
int
remove_last_components(char *buffer, unsigned n)
{
    char *last_slash = NULL;

    while ( n-- > 0 ) {
        if ( (last_slash = strrchr(buffer, '/')) ) {
             *last_slash = '\0';
        }
    }

    return last_slash ? 0 : -1;
}

int
main(int argc, char **argv)
{
    char app_path[PATH_MAX];
    uint32_t path_size = PATH_MAX;
    char *args[] = { "run.sh", NULL };

    if ( ! _NSGetExecutablePath(app_path, &path_size) ) {
        if ( ! remove_last_components(app_path, 2) ) {
            strncat(app_path, "/run.sh", PATH_MAX);
            execv(app_path, args);
        }
    }

    /* If we reach this point, we couldn't execute the run.sh script. */
    return EXIT_FAILURE;
}
