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
    char command[PATH_MAX];
    uint32_t path_size = PATH_MAX;
    int ret;

    /* Change current directory to the main "Contents" directory of the application. */
    if ( ! _NSGetExecutablePath(app_path, &path_size) ) {
        if ( ! remove_last_components(app_path, 2) )
            chdir(app_path);
    }

    /* Append our argument to the command, if any. */
    if ( argc > 1 && strlen(argv[2]) < (PATH_MAX - 11) )
        snprintf(command, sizeof(command), "./run.sh %s", argv[2]);
    else
        strncpy(command, "./run.sh", sizeof(command));

    /* Run the real launch script. */
    ret = system(command);

    return ret;
}
