#include <stdlib.h>
#include <string.h>

#include <unistd.h>                         /* chdir */
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

    exit(EXIT_SUCCESS);
}

int
main(int argc, char **argv)
{
    JavaVMOption jvm_opts[9];
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

    /* Preparing Java options.
     * TODO: allow to configure memory parameters. */
    jvm_opts[0].optionString = "-Dlogback.configurationFile=conf/logback.xml";
    jvm_opts[1].optionString = "-DentityExpansionLimit=100000000";
    jvm_opts[2].optionString = "-Dfile.encoding=UTF-8";
    jvm_opts[3].optionString = "-XX:CompileCommand=exclude,javax/swing/text/GlyphView,getBreakSpot";
    jvm_opts[4].optionString = "-Dapple.laf.useScreenMenuBar=true";
    jvm_opts[5].optionString = "-Dcom.apple.mrj.application.apple.menu.about.name=Protege";
    jvm_opts[6].optionString = "-Xdock:name=Protege";
    jvm_opts[7].optionString = "-Xdock:icon=Resources/Protege.icns";
    jvm_opts[8].optionString = "-Djava.class.path"
                               "=bundles/guava.jar"
                               ":bundles/logback-classic.jar"
                               ":bundles/logback-core.jar"
                               ":bundles/slf4j-api.jar"
                               ":bundles/glassfish-corba-orb.jar"
                               ":bundles/org.apache.felix.main.jar"
                               ":bundles/maven-artifact.jar"
                               ":bundles/protege-launcher.jar";

    jvm_args.version = JNI_VERSION_1_2;
    jvm_args.nOptions = 9;
    jvm_args.options = jvm_opts;
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
