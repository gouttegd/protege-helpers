#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>                         /* chdir */
#include <sys/stat.h>
#include <err.h>
#include <dlfcn.h>                          /* dlopen/dlsym */
#include <pthread.h>

#include <mach-o/dyld.h>                    /* _NSGetExecutablePath */
#include <CoreFoundation/CoreFoundation.h>  /* CFRunLoopSourceCreate... */

#include <jni.h>                            /* Java Native Interface */

#define JAVA_LIB_PATH "jre/lib/jli/libjli.dylib"
#define PROTEGE_MAIN_CLASS "org/protege/osgi/framework/Launcher"

typedef jint (JNICALL CreateJavaVM_t)(JavaVM **vm, void **env, void *args);


static char bundle_path[PATH_MAX];
static void *java_library = NULL;

static char *default_jvm_options[] = {
    "-Dlogback.configurationFile=conf/logback.xml",
    "-DentityExpansionLimit=100000000",
    "-Dfile.encoding=UTF-8",
    "-XX:CompileCommand=exclude,javax/swing/text/GlyphView,getBreakSpot",
    "-Dapple.laf.useScreenMenuBar=true",
    "-Dcom.apple.mrj.application.apple.menu.about.name=Protege",
    "-Xdock:name=Protege",
    "-Xdock:icon=Resources/Protege.icns",
    "-Djava.class.path"
      "=bundles/guava.jar"
      ":bundles/logback-classic.jar"
      ":bundles/logback-core.jar"
      ":bundles/slf4j-api.jar"
      ":bundles/glassfish-corba-orb.jar"
      ":bundles/org.apache.felix.main.jar"
      ":bundles/maven-artifact.jar"
      ":bundles/protege-launcher.jar",
    NULL
};


/*
 * Dummy callback for the main thread loop.
 */
static void
dummy_callback(void *info) { }


/*
 * Get the path to the "Contents" directory inside the application bundle.
 */
static char *
get_bundle_path(char *buffer, uint32_t len)
{
    char *last_slash = NULL;
    int n = 2;

    if ( ! _NSGetExecutablePath(buffer, &len) ) {
        while ( n-- > 0 ) {
            if ( (last_slash = strrchr(buffer, '/')) ) {
                *last_slash = '\0';
            }
        }
    }

    return last_slash ? buffer : NULL;
}

/*
 * Load the Java library from the bundled JRE.
 */
static void*
load_jre(const char *base_path)
{
    char path[PATH_MAX];
    void *lib;

    strncpy(path, base_path, PATH_MAX);
    strncat(path, "/" JAVA_LIB_PATH, PATH_MAX);
    lib = dlopen(path, RTLD_LAZY);

    return lib;
}

/*
 * Execute the main method of the specified Java class.
 */
static int
start_java_main(JNIEnv *env, const char *main_class_name)
{
    jclass main_class;
    jmethodID main_method;
    jobjectArray main_args;

    if ( ! (main_class = (*env)->FindClass(env, main_class_name)) )
        return -1;

    if ( ! (main_method = (*env)->GetStaticMethodID(env, main_class, "main", "([Ljava/lang/String;)V")) )
        return -1;

    main_args = (*env)->NewObjectArray(env, 0,
                                       (*env)->FindClass(env, "java/lang/String"),
                                       (*env)->NewStringUTF(env, ""));
    (*env)->CallStaticVoidMethod(env, main_class, main_method, main_args);

    return 0;
}

static void*
start_jvm(void *arg)
{
    JavaVM *jvm;
    JNIEnv *env;
    CreateJavaVM_t *create_java_vm = NULL;

    if ( ! (create_java_vm = (CreateJavaVM_t *)dlsym(java_library, "JNI_CreateJavaVM")) )
        errx(EXIT_FAILURE, "Cannot find JNI_CreateJavaVM function");

    if ( create_java_vm(&jvm, (void **)&env, (JavaVMInitArgs *)arg) == JNI_ERR )
        errx(EXIT_FAILURE, "Cannot create Java virtual machine");

    if ( start_java_main(env, PROTEGE_MAIN_CLASS) != 0 ) {
        (*jvm)->DestroyJavaVM(jvm);
        errx(EXIT_FAILURE, "Cannot start Java main method");
    }

    if ( (*env)->ExceptionCheck(env) ) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
    }

    (*jvm)->DetachCurrentThread(jvm);
    (*jvm)->DestroyJavaVM(jvm);
    free(((JavaVMInitArgs *)arg)->options);

    exit(EXIT_SUCCESS);
}

static char *
find_configuration_file(char *buffer, size_t len)
{
    char *home;
    struct stat st_buf;

    if ( (home = getenv("HOME")) ) {
        if ( snprintf(buffer, len, "%s/.protege/conf/jvm.conf", home) < len ) {
            if ( stat(buffer, &st_buf) == 0 )
                return buffer;
        }
    }

    if ( snprintf(buffer, len, "%s/conf/jvm.conf", bundle_path) < len ) {
        if ( stat(buffer, &st_buf) == 0 )
            return buffer;
    }

    return NULL;
}

/*
 * Discards characters from the specified stream up to the next end of line.
 */
static int
discard_line(FILE *f)
{
    int c;

    while ( (c = fgetc(f)) != '\n' && c != EOF ) ;

    return c == EOF ? -1 : 0;
}

/*
 * Reads a single line from the specified stream. This function differs from
 * standard fgets(3) in two aspects: the newline character is not stored, and
 * if the line to read is too long to fit into the provided buffer, the whole
 * line is discarded.
 */
static ssize_t
get_line(FILE *f, char *buffer, size_t len)
{
    int c;
    size_t n = 0;

    while ( (c = fgetc(f)) != '\n' ) {
        if ( c == EOF )
            return -1;

        if ( n >= len - 1 ) {   /* line too long */
            discard_line(f);
            return -1;
        }

        buffer[n++] = (char)c;
    }
    buffer[n] = '\0';

    return n;
}

static void
get_extra_jvm_options(JavaVMOption **jvm_opts, int *n_options)
{
    char conf_file_path[PATH_MAX];
    char line[100], *opt_value, *option_string;
    ssize_t n;
    int current_max = *n_options;
    FILE *conf_file;

    if ( ! find_configuration_file(conf_file_path, PATH_MAX) )
        return;

    if ( ! (conf_file = fopen(conf_file_path, "r")) ) {
        warn("Cannot open configuration file at %s", conf_file_path);
    }

    while ( ! feof(conf_file) ) {
        if ( (n = get_line(conf_file, line, sizeof(line))) > 0 ) {
            if ( line[0] == '#' )
                continue;

            if ( ! (opt_value = strchr(line, '=')) )
                continue;

            *opt_value++ = '\0';
            option_string = NULL;
            /*
             * TODO: Check for correctness of the option value.
             */
            if ( strcmp(line, "max_heap_size") == 0 )
                asprintf(&option_string, "-Xmx%s", opt_value);
            else if ( strcmp(line, "min_heap_size") == 0 )
                asprintf(&option_string, "-Xms%s", opt_value);
            else if ( strcmp(line, "stack_size") == 0 )
                asprintf(&option_string, "-Xss%s", opt_value);
            else if ( strcmp(line, "append") == 0 )
                option_string = strdup(opt_value);

            if ( option_string ) {
                warnx("Appending Java option: %s", option_string);
                if ( *n_options >= current_max ) {
                    current_max += 10;
                    *jvm_opts = realloc(*jvm_opts, current_max * sizeof(JavaVMOption));
                }
                (*jvm_opts)[(*n_options)++].optionString = option_string;
            }
        }
    }

    fclose(conf_file);
}

static JavaVMOption *
get_jvm_options(int *n_options)
{
    JavaVMOption *jvm_opts = NULL;
    int i;

    *n_options = (sizeof(default_jvm_options) / sizeof(char *)) - 1;
    jvm_opts = calloc(*n_options, sizeof(JavaVMOption));
    for ( i = 0; i < *n_options; i++ )
        jvm_opts[i].optionString = default_jvm_options[i];

    get_extra_jvm_options(&jvm_opts, n_options);

    return jvm_opts;
}

int
main(int argc, char **argv)
{
    JavaVMOption *jvm_opts;
    JavaVMInitArgs jvm_args;
    pthread_t jvm_thread;
    pthread_attr_t jvm_thread_attr;
    CFRunLoopSourceContext loop_context;
    CFRunLoopSourceRef loop_ref;

    (void) argc;
    (void) argv;

    if ( ! get_bundle_path(bundle_path, PATH_MAX) )
        errx(EXIT_FAILURE, "Cannot get the path to the application bundle");

    if ( chdir(bundle_path) == -1 )
        err(EXIT_FAILURE, "Cannot change current directory to the application bundle");

    if ( ! (java_library = load_jre(bundle_path)) )
        err(EXIT_FAILURE, "Cannot load the bundled JRE");

    /* Preparing Java options. */
    jvm_args.version = JNI_VERSION_1_2;
    jvm_args.options = get_jvm_options(&(jvm_args.nOptions));
    jvm_args.ignoreUnrecognized = JNI_TRUE;

    /* Start the thread where the JVM will run. */
    pthread_attr_init(&jvm_thread_attr);
    pthread_attr_setscope(&jvm_thread_attr, PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setdetachstate(&jvm_thread_attr, PTHREAD_CREATE_DETACHED);
    if ( pthread_create(&jvm_thread, &jvm_thread_attr, start_jvm, (void *)&jvm_args) != 0 )
        err(EXIT_FAILURE, "Cannot start JVM thread");
    pthread_attr_destroy(&jvm_thread_attr);

    /* Run a dummy loop in the main thread. */
    memset(&loop_context, 0, sizeof(loop_context));
    loop_context.perform = &dummy_callback;
    loop_ref = CFRunLoopSourceCreate(NULL, 0, &loop_context);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), loop_ref, kCFRunLoopCommonModes);
    CFRunLoopRun();

    return EXIT_SUCCESS;
}
