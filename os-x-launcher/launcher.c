/*
 * launcher.c - Protégé macOS launcher
 * Copyright © Damien Goutte-Gattat
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistribution of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistribution in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior written
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

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

static int
check_memory_option(const char *option)
{
    char *suffix = NULL;

    errno = 0;
    (void) strtoul(option, &suffix, 10);
    if ( errno != 0 )
        return -1;

    if ( *suffix != '\0' ) {
        /* If present, the suffix must be in [kKmMgGtT]. */
        if ( (*suffix != 'k' && *suffix != 'K'
                && *suffix != 'm' && *suffix != 'M'
                && *suffix != 'g' && *suffix != 'G'
                && *suffix != 't' && *suffix != 'T')
            || *(suffix + 1) != '\0' ) {
            errno = EINVAL;
            return -1;
        }
    }

    return 0;
}

static void
append_jvm_option(JavaVMOption **jvm_opts, int *n_options, int *max_options, char *new_option)
{
    if ( strncmp(new_option, "-Xmx", 4) == 0
        || strncmp(new_option, "-Xms", 4) == 0
        || strncmp(new_option, "-Xss", 4) == 0 ) {
        if ( check_memory_option(&new_option[4]) == -1 ) {
            warn("Ignoring ill-formatted option '%s'", new_option);
            return;
        }
    }

    warnx("Appending Java option: %s", new_option);
    if ( *n_options >= *max_options ) {
        (*max_options) += 10;
        *jvm_opts = realloc(*jvm_opts, (*max_options) * sizeof(JavaVMOption));
    }
    (*jvm_opts)[(*n_options)++].optionString = new_option;
}

static int
get_extra_jvm_options_from_conf_file(JavaVMOption **jvm_opts, int *n_options, int *max_options)
{
    char conf_file_path[PATH_MAX];
    char line[100], *opt_value, *option_string;
    ssize_t n;
    FILE *conf_file;

    if ( ! find_configuration_file(conf_file_path, PATH_MAX) )
        return -1;

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
            if ( strcmp(line, "max_heap_size") == 0 )
                asprintf(&option_string, "-Xmx%s", opt_value);
            else if ( strcmp(line, "min_heap_size") == 0 )
                asprintf(&option_string, "-Xms%s", opt_value);
            else if ( strcmp(line, "stack_size") == 0 )
                asprintf(&option_string, "-Xss%s", opt_value);
            else if ( strcmp(line, "append") == 0 )
                option_string = strdup(opt_value);

            if ( option_string )
                append_jvm_option(jvm_opts, n_options, max_options, option_string);
        }
    }

    fclose(conf_file);

    return 0;
}

static void
get_extra_jvm_options_from_bundle(JavaVMOption **jvm_opts, int *n_options, int *max_options)
{
    CFBundleRef main_bundle;
    CFDictionaryRef info_dict;
    CFArrayRef jvmopts_array;
    CFIndex length, i;

    if ( ! (main_bundle = CFBundleGetMainBundle()) )
        return;

    if ( ! (info_dict = CFBundleGetInfoDictionary(main_bundle)) )
        return;

    if ( ! (jvmopts_array = (CFArrayRef)CFDictionaryGetValue(info_dict, CFSTR("JVMOptions"))) )
        return;

    length = CFArrayGetCount(jvmopts_array);
    for ( i = 0; i < length; i++ ) {
        CFStringRef option = CFArrayGetValueAtIndex(jvmopts_array, i);
        if ( CFStringHasPrefix(option, CFSTR("-Xmx")) 
            || CFStringHasPrefix(option, CFSTR("-Xms"))
            || CFStringHasPrefix(option, CFSTR("-Xss")) ) {
            CFIndex option_length;
            char *option_string;

            option_length = CFStringGetLength(option);
            option_string = malloc(option_length + 1);
            CFStringGetCString(option, option_string, option_length + 1, kCFStringEncodingMacRoman);

            append_jvm_option(jvm_opts, n_options, max_options, option_string);
        }
    }
}

static JavaVMOption *
get_jvm_options(int *n_options)
{
    JavaVMOption *jvm_opts = NULL;
    int i, max_options;

    *n_options = (sizeof(default_jvm_options) / sizeof(char *)) - 1;
    max_options = *n_options;
    jvm_opts = calloc(*n_options, sizeof(JavaVMOption));
    for ( i = 0; i < *n_options; i++ )
        jvm_opts[i].optionString = default_jvm_options[i];

    if ( get_extra_jvm_options_from_conf_file(&jvm_opts, n_options, &max_options) == -1 )
        get_extra_jvm_options_from_bundle(&jvm_opts, n_options, &max_options);

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
