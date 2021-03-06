/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4 c-style: "K&R" -*- */
/* 
   jep - Java Embedded Python

   Copyright (c) 2015 JEP AUTHORS.

   This file is licenced under the the zlib/libpng License.

   This software is provided 'as-is', without any express or implied
   warranty. In no event will the authors be held liable for any
   damages arising from the use of this software.
   
   Permission is granted to anyone to use this software for any
   purpose, including commercial applications, and to alter it and
   redistribute it freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you
   must not claim that you wrote the original software. If you use
   this software in a product, an acknowledgment in the product
   documentation would be appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and
   must not be misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.   


   *****************************************************************************
   This file handles two main things:
   - startup, shutdown of interpreters.
      (those are the pyembed_* functions)
   - setting of parameters
      (the pyembed_set*)

   The really interesting stuff is not here. :-) This file simply makes calls
   to the type definitions for pyjobject, etc.
   *****************************************************************************
*/

#ifdef WIN32
# include "winconfig.h"
#endif

#if HAVE_CONFIG_H
# include <config.h>
#endif

#if HAVE_UNISTD_H
# include <sys/types.h>
# include <unistd.h>
#endif

#if STDC_HEADERS
# include <stdio.h>
#endif


// The following includes were added to support compilation on RHEL 4, which
// ships with python2.3.  With python2.4 (and possibly beyond), the includes
// are not necessary but do not affect operation.
#if 0
#include <pyport.h>
#include <object.h>
#include <pystate.h>
#include <pythonrun.h>
#include <compile.h>
#endif
// End additional includes for python2.3

#include "pyembed.h"
#include "pyjobject.h"
#include "pyjclass.h"
#include "pyjarray.h"
#include "util.h"


#ifdef __APPLE__
#ifndef WITH_NEXT_FRAMEWORK
#include <crt_externs.h>
// workaround for
// http://bugs.python.org/issue1602133
char **environ = NULL;
#endif
#endif

static PyThreadState *mainThreadState = NULL;

static PyObject* pyembed_findclass(PyObject*, PyObject*);
static PyObject* pyembed_forname(PyObject*, PyObject*);
static PyObject* pyembed_set_print_stack(PyObject*, PyObject*);
static PyObject* pyembed_jproxy(PyObject*, PyObject*);

static int maybe_pyc_file(FILE*, const char*, const char*, int);
static void pyembed_run_pyc(JepThread *jepThread, FILE *);


// ClassLoader.loadClass
static jmethodID loadClassMethod = 0;

// jep.Proxy.newProxyInstance
static jmethodID newProxyMethod = 0;

#if PY_MAJOR_VERSION < 3
// Integer(int)
static jmethodID integerIConstructor = 0;
#endif

// Long(long)
static jmethodID longJConstructor = 0;

// Double(double)
static jmethodID doubleDConstructor = 0;

// Boolean(boolean)
static jmethodID booleanBConstructor = 0;

// ArrayList(int)
static jmethodID arraylistIConstructor = 0;
static jmethodID arraylistAdd = 0;

// HashMap(int)
static jmethodID hashmapIConstructor = 0;
static jmethodID hashmapPut = 0;

static struct PyMethodDef jep_methods[] = {
    { "findClass",
      pyembed_findclass,
      METH_VARARGS,
      "Find and instantiate a system class, somewhat faster than forName." },
    
    { "forName",
      pyembed_forname,
      METH_VARARGS,
      "Find and return a jclass object using the supplied ClassLoader." },

    { "jarray",
      pyjarray_new_v,
      METH_VARARGS,
      "Create a new primitive array in Java.\n"
      "Accepts:\n"
      "(size, type _ID, [0]) || "
      "(size, JCHAR_ID, [string value] || "
      "(size, jobject) || "
      "(size, str) || "
      "(size, jarray)" },

    { "printStack",
      pyembed_set_print_stack,
      METH_VARARGS,
      "Turn on printing of stack traces (True|False)" },

    { "jproxy",
      pyembed_jproxy,
      METH_VARARGS,
      "Create a Proxy class for a Python object.\n"
      "Accepts two arguments: ([a class object], [list of java interfaces "
      "to implement, string names])" },

    { NULL, NULL }
};

#if PY_MAJOR_VERSION >= 3
 static struct PyModuleDef jep_module_def = {
    PyModuleDef_HEAD_INIT,
    "_jep",              /* m_name */
    "_jep",              /* m_doc */
    -1,                  /* m_size */
    jep_methods,         /* m_methods */
    NULL,                /* m_reload */
    NULL,                /* m_traverse */
    NULL,                /* m_clear */
    NULL,                /* m_free */
  };
#endif


static PyObject* initjep(void) {
    PyObject *modjep;

#if PY_MAJOR_VERSION >= 3
    PyObject *sysmodules;
    modjep = PyModule_Create(&jep_module_def);
    sysmodules = PyImport_GetModuleDict();
    PyDict_SetItemString(sysmodules, "_jep", modjep);
#else
    PyImport_AddModule("_jep");
    Py_InitModule((char *) "_jep", jep_methods);
#endif
    modjep = PyImport_ImportModule("_jep");
    if(modjep == NULL)
        printf("WARNING: couldn't import module _jep.\n");
    else {
        // stuff for making new pyjarray objects
        PyModule_AddIntConstant(modjep, "JBOOLEAN_ID", JBOOLEAN_ID);
        PyModule_AddIntConstant(modjep, "JINT_ID", JINT_ID);
        PyModule_AddIntConstant(modjep, "JLONG_ID", JLONG_ID);
        PyModule_AddIntConstant(modjep, "JSTRING_ID", JSTRING_ID);
        PyModule_AddIntConstant(modjep, "JDOUBLE_ID", JDOUBLE_ID);
        PyModule_AddIntConstant(modjep, "JSHORT_ID", JSHORT_ID);
        PyModule_AddIntConstant(modjep, "JFLOAT_ID", JFLOAT_ID);
        PyModule_AddIntConstant(modjep, "JCHAR_ID", JCHAR_ID);
        PyModule_AddIntConstant(modjep, "JBYTE_ID", JBYTE_ID);
        PyModule_AddIntConstant(modjep, "USE_NUMPY", USE_NUMPY);
    }

    return modjep;
}


void pyembed_startup(void) {
#ifdef __APPLE__
#ifndef WITH_NEXT_FRAMEWORK
// workaround for
// http://bugs.python.org/issue1602133
    environ = *_NSGetEnviron();
#endif
#endif

    if(mainThreadState != NULL)
        return;

    Py_Initialize();
    PyEval_InitThreads();

    // save a pointer to the main PyThreadState object
    mainThreadState = PyThreadState_Get();
    PyEval_ReleaseThread(mainThreadState);
}


void pyembed_shutdown(void) {
    printf("Shutting down Python...\n");
    PyEval_AcquireThread(mainThreadState);
    Py_Finalize();
}


intptr_t pyembed_thread_init(JNIEnv *env, jobject cl, jobject caller) {
    JepThread *jepThread;
    PyObject  *tdict, *mod_main, *globals;
    
    if(cl == NULL) {
        THROW_JEP(env, "Invalid Classloader.");
        return 0;
    }
    
    PyEval_AcquireThread(mainThreadState);
    
    jepThread = PyMem_Malloc(sizeof(JepThread));
    if(!jepThread) {
        THROW_JEP(env, "Out of memory.");
        PyEval_ReleaseThread(mainThreadState);
        return 0;
    }
    
    jepThread->tstate = Py_NewInterpreter();
    /*
     * Py_NewInterpreter() seems to take the thread state, but we're going to
     * save/release and reacquire it since that doesn't seem documented
     */
    PyEval_SaveThread();
    PyEval_AcquireThread(jepThread->tstate);

    // store java.lang.Class objects for later use.
    // it's a noop if already done, but to synchronize, have the lock first
    if(!cache_primitive_classes(env))
        printf("WARNING: failed to get primitive class types.\n");
    if(!cache_frequent_classes(env))
        printf("WARNING: failed to get frequent class types.\n");


    mod_main = PyImport_AddModule("__main__");                      /* borrowed */
    if(mod_main == NULL) {
        THROW_JEP(env, "Couldn't add module __main__.");
        PyEval_ReleaseThread(jepThread->tstate);
        return 0;
    }
    globals = PyModule_GetDict(mod_main);
    Py_INCREF(globals);

    // init static module
    jepThread->modjep          = initjep();
    jepThread->globals         = globals;
    jepThread->env             = env;
    jepThread->classloader     = (*env)->NewGlobalRef(env, cl);
    jepThread->caller          = (*env)->NewGlobalRef(env, caller);
    jepThread->printStack      = 0;
    jepThread->fqnToPyJmethods = NULL;

    if((tdict = PyThreadState_GetDict()) != NULL) {
        PyObject *key, *t;
        
#if PY_MAJOR_VERSION >= 3
        t   = PyCapsule_New((void *) jepThread, NULL, NULL);
#else
        t   = (PyObject *) PyCObject_FromVoidPtr((void *) jepThread, NULL);
#endif
        key = PyString_FromString(DICT_KEY);
        
        PyDict_SetItem(tdict, key, t);   /* takes ownership */
        
        Py_DECREF(key);
        Py_DECREF(t);
    }
    
    PyEval_ReleaseThread(jepThread->tstate);
    return (intptr_t) jepThread;
}


void pyembed_thread_close(JNIEnv *env, intptr_t _jepThread) {
    JepThread     *jepThread;
    PyObject      *tdict, *key;

    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        printf("WARNING: thread_close, invalid JepThread pointer.\n");
        return;
    }
    
    PyEval_AcquireThread(jepThread->tstate);

    key = PyString_FromString(DICT_KEY);
    if((tdict = PyThreadState_GetDict()) != NULL && key != NULL)
        PyDict_DelItem(tdict, key);
    Py_DECREF(key);

    Py_CLEAR(jepThread->globals);
    Py_CLEAR(jepThread->fqnToPyJmethods);
    Py_CLEAR(jepThread->modjep);

    if(jepThread->classloader) {
        (*env)->DeleteGlobalRef(env, jepThread->classloader);
    }
    if(jepThread->caller) {
        (*env)->DeleteGlobalRef(env, jepThread->caller);
    }
    
    Py_EndInterpreter(jepThread->tstate);
    
    PyMem_Free(jepThread);
    PyEval_ReleaseLock();
}


JNIEnv* pyembed_get_env(void) {
    JavaVM *jvm;
    JNIEnv *env;

    JNI_GetCreatedJavaVMs(&jvm, 1, NULL);
    (*jvm)->AttachCurrentThread(jvm, (void**) &env, NULL);

    return env;
}


// get thread struct when called from internals.
// NULL if not found.
// hold the lock before calling.
JepThread* pyembed_get_jepthread(void) {
    PyObject  *tdict, *t, *key;
    JepThread *ret = NULL;
    
    key = PyString_FromString(DICT_KEY);
    if((tdict = PyThreadState_GetDict()) != NULL && key != NULL) {
        t = PyDict_GetItem(tdict, key); /* borrowed */
        if(t != NULL && !PyErr_Occurred()) {
#if PY_MAJOR_VERSION >= 3
            ret = (JepThread*) PyCapsule_GetPointer(t, NULL);
#else
            ret = (JepThread*) PyCObject_AsVoidPtr(t);
#endif
        }
    }
    
    Py_DECREF(key);
    return ret;
}


// used by _forname
#define LOAD_CLASS_METHOD(env, cl)                                          \
{                                                                           \
    if(loadClassMethod == 0) {                                              \
        jobject clazz;                                                      \
                                                                            \
        clazz = (*env)->GetObjectClass(env, cl);                            \
        if(process_java_exception(env) || !clazz)                           \
            return NULL;                                                    \
                                                                            \
        loadClassMethod =                                                   \
            (*env)->GetMethodID(env,                                        \
                                clazz,                                      \
                                "loadClass",                                \
                                "(Ljava/lang/String;)Ljava/lang/Class;");   \
                                                                            \
        if(process_java_exception(env) || !loadClassMethod) {               \
            (*env)->DeleteLocalRef(env, clazz);                             \
            return NULL;                                                    \
        }                                                                   \
                                                                            \
        (*env)->DeleteLocalRef(env, clazz);                                 \
    }                                                                       \
}


static PyObject* pyembed_jproxy(PyObject *self, PyObject *args) {
    PyThreadState *_save;
    JepThread     *jepThread;
    JNIEnv        *env = NULL;
    PyObject      *pytarget;
    PyObject      *interfaces;
    jclass         clazz;
    jobject        cl;
    jobject        classes;
    Py_ssize_t     inum, i;
    jobject        proxy;

    if(!PyArg_ParseTuple(args, "OO!:jproxy",
                         &pytarget, 
                         &PyList_Type,
                         &interfaces))
        return NULL;

    jepThread = pyembed_get_jepthread();
    if(!jepThread) {
        if(!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Invalid JepThread pointer.");
        return NULL;
    }

    env = jepThread->env;
    cl  = jepThread->classloader;

    Py_UNBLOCK_THREADS;
    clazz = (*env)->FindClass(env, "jep/Proxy");
    Py_BLOCK_THREADS;
    if(process_java_exception(env) || !clazz)
        return NULL;

    if(newProxyMethod == 0) {
        newProxyMethod =
            (*env)->GetStaticMethodID(
                env,
                clazz,
                "newProxyInstance",
                "(JJLjep/Jep;Ljava/lang/ClassLoader;[Ljava/lang/String;)Ljava/lang/Object;");

        if(process_java_exception(env) || !newProxyMethod)
            return NULL;
    }

    inum = (int) PyList_GET_SIZE(interfaces);
    if(inum < 1)
        return PyErr_Format(PyExc_ValueError, "Empty interface list.");

    // now convert string list to java array

    classes = (*env)->NewObjectArray(env, (jsize) inum, JSTRING_TYPE, NULL);
    if(process_java_exception(env) || !classes)
        return NULL;

    for(i = 0; i < inum; i++) {
        char     *str;
        jstring   jstr;
        PyObject *item;

        item = PyList_GET_ITEM(interfaces, i);
        if(!PyString_Check(item))
            return PyErr_Format(PyExc_ValueError, "Item %zd not a string.", i);

        str  = PyString_AsString(item);
        jstr = (*env)->NewStringUTF(env, (const char *) str);

        (*env)->SetObjectArrayElement(env, classes, (jsize) i, jstr);
        (*env)->DeleteLocalRef(env, jstr);
    }

    // do the deed
    proxy = (*env)->CallStaticObjectMethod(env,
                                           clazz,
                                           newProxyMethod,
                                           (jlong) (intptr_t) jepThread,
                                           (jlong) (intptr_t) pytarget,
                                           jepThread->caller,
                                           cl,
                                           classes);
    if(process_java_exception(env) || !proxy)
        return NULL;

    // make sure target doesn't get garbage collected
    Py_INCREF(pytarget);

    return pyjobject_new(env, proxy);
}


static PyObject* pyembed_set_print_stack(PyObject *self, PyObject *args) {
    JepThread *jepThread;
    char      *print = 0;

    if(!PyArg_ParseTuple(args, "b:setPrintStack", &print))
        return NULL;

    jepThread = pyembed_get_jepthread();
    if(!jepThread) {
        if(!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Invalid JepThread pointer.");
        return NULL;
    }

    if(print == 0)
        jepThread->printStack = 0;
    else
        jepThread->printStack = 1;

    Py_INCREF(Py_None);
    return Py_None;
}


static PyObject* pyembed_forname(PyObject *self, PyObject *args) {
    JNIEnv    *env       = NULL;
    char      *name;
    jobject    cl;
    jclass     objclazz;
    jstring    jstr;
    JepThread *jepThread;

    if(!PyArg_ParseTuple(args, "s", &name))
        return NULL;
    
    jepThread = pyembed_get_jepthread();
    if(!jepThread) {
        if(!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Invalid JepThread pointer.");
        return NULL;
    }
    
    env = jepThread->env;
    cl  = jepThread->classloader;
    
    LOAD_CLASS_METHOD(env, cl);
    
    jstr = (*env)->NewStringUTF(env, (const char *) name);
    if(process_java_exception(env) || !jstr)
        return NULL;
    
    objclazz = (jclass) (*env)->CallObjectMethod(env,
                                                 cl,
                                                 loadClassMethod,
                                                 jstr);
    if(process_java_exception(env) || !objclazz) {
        return NULL;
    }
    
    return (PyObject *) pyjobject_new_class(env, objclazz);
}


static PyObject* pyembed_findclass(PyObject *self, PyObject *args) {
    JNIEnv    *env       = NULL;
    char      *name, *p;
    jclass     clazz;
    JepThread *jepThread;
    
    if(!PyArg_ParseTuple(args, "s", &name))
        return NULL;
    
    jepThread = pyembed_get_jepthread();
    if(!jepThread) {
        if(!PyErr_Occurred())
            PyErr_SetString(PyExc_RuntimeError, "Invalid JepThread pointer.");
        return NULL;
    }
    
    env = jepThread->env;
    
    // replace '.' with '/'
    // i'm told this is okay to do with unicode.
    for(p = name; *p != '\0'; p++) {
        if(*p == '.')
            *p = '/';
    }
    
    clazz = (*env)->FindClass(env, name);
    if(process_java_exception(env))
        return NULL;
    
    return (PyObject *) pyjobject_new_class(env, clazz);
}


jobject pyembed_invoke_method(JNIEnv *env,
                              intptr_t _jepThread,
                              const char *cname,
                              jobjectArray args,
                              jintArray types) {
    PyObject         *callable;
    JepThread        *jepThread;
    jobject           ret;
    
    ret = NULL;

    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return ret;
    }

    PyEval_AcquireThread(jepThread->tstate);

    callable = PyDict_GetItemString(jepThread->globals, (char *) cname);
    if(!callable) {
        THROW_JEP(env, "Object was not found in the global dictionary.");
        goto EXIT;
    }
    if(process_py_exception(env, 0))
        goto EXIT;

    ret = pyembed_invoke(env, callable, args, types);

EXIT:
    PyEval_ReleaseThread(jepThread->tstate);

    return ret;
}


// invoke object callable
// **** hold lock before calling ****
jobject pyembed_invoke(JNIEnv *env,
                       PyObject *callable,
                       jobjectArray args,
                       jintArray _types) {
    jobject        ret;
    int            iarg, arglen;
    jint          *types;       /* pinned primitive array */
    jboolean       isCopy;
    PyObject      *pyargs;      /* a tuple */
    PyObject      *pyret;

    types    = NULL;
    ret      = NULL;
    pyret    = NULL;

    if(!PyCallable_Check(callable)) {
        THROW_JEP(env, "pyembed:invoke Invalid callable.");
        return NULL;
    }

    // pin primitive array so we can get to it
    types = (*env)->GetIntArrayElements(env, _types, &isCopy);

    // first thing to do, convert java arguments to a python tuple
    arglen = (*env)->GetArrayLength(env, args);
    pyargs = PyTuple_New(arglen);
    for(iarg = 0; iarg < arglen; iarg++) {
        jobject   val;
        int       typeid;
        PyObject *pyval;

        val = (*env)->GetObjectArrayElement(env, args, iarg);
        if((*env)->ExceptionCheck(env)) /* careful, NULL is okay */
            goto EXIT;

        typeid = (int) types[iarg];

        // now we know the type, convert and add to pyargs.  we know
        pyval = convert_jobject(env, val, typeid);
        if((*env)->ExceptionOccurred(env))
            goto EXIT;

        PyTuple_SET_ITEM(pyargs, iarg, pyval); /* steals */
        if(val)
            (*env)->DeleteLocalRef(env, val);
    } // for(iarg = 0; iarg < arglen; iarg++)

    pyret = PyObject_CallObject(callable, pyargs);
    if(process_py_exception(env, 0) || !pyret)
        goto EXIT;

    // handles errors
    ret = pyembed_box_py(env, pyret);

EXIT:
    Py_XDECREF(pyargs);
    Py_XDECREF(pyret);

    if(types) {
        (*env)->ReleaseIntArrayElements(env,
                                        _types,
                                        types,
                                        JNI_ABORT);

        (*env)->DeleteLocalRef(env, _types);
    }

    return ret;
}


void pyembed_eval(JNIEnv *env,
                  intptr_t _jepThread,
                  char *str) {
    PyObject         *result;
    JepThread        *jepThread;
    
    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return;
    }

    PyEval_AcquireThread(jepThread->tstate);
    
    if(str == NULL)
        goto EXIT;

    if(process_py_exception(env, 1))
        goto EXIT;
    
    result = PyRun_String(str,  /* new ref */
                          Py_single_input,
                          jepThread->globals,
                          jepThread->globals);
    
    // c programs inside some java environments may get buffered output
    fflush(stdout);
    fflush(stderr);
    
    process_py_exception(env, 1);
    
    Py_XDECREF(result);

EXIT:
    PyEval_ReleaseThread(jepThread->tstate);
}


// returns 1 if finished, 0 if not, throws exception otherwise
int pyembed_compile_string(JNIEnv *env,
                           intptr_t _jepThread,
                           char *str) {
    PyObject       *code;
    int             ret = -1;
    JepThread      *jepThread;
    
    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return 0;
    }
    
    if(str == NULL)
        return 0;

    PyEval_AcquireThread(jepThread->tstate);
    
    code = Py_CompileString(str, "<stdin>", Py_single_input);
    
    if(code != NULL) {
        Py_DECREF(code);
        ret = 1;
    }
    else if(PyErr_ExceptionMatches(PyExc_SyntaxError)) {
        PyErr_Clear();
        ret = 0;
    }
    else
        process_py_exception(env, 0);

    PyEval_ReleaseThread(jepThread->tstate);
    return ret;
}


intptr_t pyembed_create_module(JNIEnv *env,
                               intptr_t _jepThread,
                               char *str) {
    PyObject       *module;
    JepThread      *jepThread;
    intptr_t        ret;
    PyObject       *key;

    ret = 0;

    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return 0;
    }

    if(str == NULL)
        return 0;

    PyEval_AcquireThread(jepThread->tstate);

    if(PyImport_AddModule(str) == NULL || process_py_exception(env, 1))
        goto EXIT;

    PyImport_AddModule(str);
    module = PyImport_ImportModuleEx(str,
                                     jepThread->globals,
                                     jepThread->globals,
                                     NULL); /* new ref */

    key = PyString_FromString(str);
    PyDict_SetItem(jepThread->globals,
                   key,
                   module);     /* takes ownership */

    Py_DECREF(key);
    Py_DECREF(module);

    if(process_py_exception(env, 0) || module == NULL)
        ret = 0;
    else
        ret = (intptr_t) module;

EXIT:
    PyEval_ReleaseThread(jepThread->tstate);

    return ret;
}


intptr_t pyembed_create_module_on(JNIEnv *env,
                                  intptr_t _jepThread,
                                  intptr_t _onModule,
                                  char *str) {
    PyObject       *module, *onModule;
    JepThread      *jepThread;
    intptr_t        ret;
    PyObject       *globals;
    PyObject       *key;

    ret = 0;
    globals = 0;

    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return 0;
    }

    if(str == NULL)
        return 0;

    PyEval_AcquireThread(jepThread->tstate);

    onModule = (PyObject *) _onModule;
    if(!PyModule_Check(onModule)) {
        THROW_JEP(env, "Invalid onModule.");
        goto EXIT;
    }

    globals = PyModule_GetDict(onModule);
    Py_INCREF(globals);

    if(PyImport_AddModule(str) == NULL || process_py_exception(env, 1))
        goto EXIT;

    PyImport_AddModule(str);
    module = PyImport_ImportModuleEx(str, globals, globals, NULL); /* new ref */

    key = PyString_FromString(str);
    PyDict_SetItem(globals,
                   key,
                   module);     /* ownership */
    Py_DECREF(key);
    Py_DECREF(module);

    if(process_py_exception(env, 0) || module == NULL)
        ret = 0;
    else
        ret = (intptr_t) module;

EXIT:
    Py_XDECREF(globals);

    PyEval_ReleaseThread(jepThread->tstate);

    return ret;
}


void pyembed_setloader(JNIEnv *env, intptr_t _jepThread, jobject cl) {
    jobject    oldLoader = NULL;
    JepThread *jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return;
    }
    
    if(!cl)
        return;
    
    oldLoader = jepThread->classloader;
    if(oldLoader)
        (*env)->DeleteGlobalRef(env, oldLoader);
    
    jepThread->classloader = (*env)->NewGlobalRef(env, cl);
}


// convert pyobject to boxed java value
jobject pyembed_box_py(JNIEnv *env, PyObject *result) {

    if(result == Py_None) {
        /*
         * To ensure we get back a Java null instead of the word "None", we
         * return NULL in this case.  All the other return NULLs below will
         * set the error indicator which should be checked for and handled by
         * code calling pyembed_box_py.
         */
        return NULL;
    }

    // class and object need to return a new local ref so the object
    // isn't garbage collected.
    if(pyjclass_check(result))
        return (*env)->NewLocalRef(env, ((PyJobject_Object *) result)->clazz);

    if(pyjobject_check(result))
        return (*env)->NewLocalRef(env, ((PyJobject_Object *) result)->object);

    if(PyString_Check(result)) {
        char *s = PyString_AS_STRING(result);
        return (*env)->NewStringUTF(env, (const char *) s);
    }

    if(PyBool_Check(result)) {
        jclass clazz;
        jboolean b = JNI_FALSE;
        if(result == Py_True)
            b = JNI_TRUE;

        clazz = (*env)->FindClass(env, "java/lang/Boolean");

        if(booleanBConstructor == 0) {
            booleanBConstructor = (*env)->GetMethodID(env,
                                                      clazz,
                                                      "<init>",
                                                      "(Z)V");
        }

        if(!process_java_exception(env) && booleanBConstructor)
            return (*env)->NewObject(env, clazz, booleanBConstructor, b);
        else
            return NULL;
    }

#if PY_MAJOR_VERSION < 3
    if(PyInt_Check(result)) {
        jclass clazz;
        jint i = (jint) PyInt_AS_LONG(result);

        clazz = (*env)->FindClass(env, "java/lang/Integer");

        if(integerIConstructor == 0) {
            integerIConstructor = (*env)->GetMethodID(env,
                                                      clazz,
                                                      "<init>",
                                                      "(I)V");
        }

        if(!process_java_exception(env) && integerIConstructor)
            return (*env)->NewObject(env, clazz, integerIConstructor, i);
        else
            return NULL;
    }
#endif

    if(PyLong_Check(result)) {
        jclass clazz;
        jeplong i = PyLong_AsLongLong(result);

        clazz = (*env)->FindClass(env, "java/lang/Long");

        if(longJConstructor == 0) {
            longJConstructor = (*env)->GetMethodID(env,
                                                   clazz,
                                                   "<init>",
                                                   "(J)V");
        }

        if(!process_java_exception(env) && longJConstructor)
            return (*env)->NewObject(env, clazz, longJConstructor, i);
        else
            return NULL;
    }

    if(PyFloat_Check(result)) {
        jclass clazz;
        jdouble d = (jdouble) PyFloat_AS_DOUBLE(result);

        clazz = (*env)->FindClass(env, "java/lang/Double");

        if(doubleDConstructor == 0) {
            doubleDConstructor = (*env)->GetMethodID(env,
                                                    clazz,
                                                    "<init>",
                                                    "(D)V");
        }

        if(!process_java_exception(env) && doubleDConstructor)
            return (*env)->NewObject(env, clazz, doubleDConstructor, d);
        else
            return NULL;
    }

    if(pyjarray_check(result)) {
        PyJarray_Object *t = (PyJarray_Object *) result;
        pyjarray_release_pinned(t, JNI_COMMIT);

        return t->object;
    }

    if(PyList_Check(result) || PyTuple_Check(result)) {
        jclass clazz;
        jobject list;
        Py_ssize_t i;
        Py_ssize_t size;
        int modifiable = PyList_Check(result);

        clazz = (*env)->FindClass(env, "java/util/ArrayList");
        if(arraylistIConstructor == 0) {
            arraylistIConstructor = (*env)->GetMethodID(env,
                                                    clazz,
                                                    "<init>",
                                                    "(I)V");
        }
        if(arraylistAdd == 0) {
            arraylistAdd = (*env)->GetMethodID(env,
                                               clazz,
                                               "add",
                                               "(Ljava/lang/Object;)Z");
        }

        if(process_java_exception(env) || !arraylistIConstructor || !arraylistAdd) {
            return NULL;
        }

        if(modifiable) {
            size = PyList_Size(result);
        } else {
            size = PyTuple_Size(result);
        }
        list = (*env)->NewObject(env, clazz, arraylistIConstructor, (int) size);
        if(process_java_exception(env) || !list) {
            return NULL;
        }

        for(i=0; i < size; i++) {
            PyObject *item;
            jobject value;

            if(modifiable) {
                item = PyList_GetItem(result, i);
            } else {
                item = PyTuple_GetItem(result, i);
            }
            value = pyembed_box_py(env, item);
            if(value == NULL && PyErr_Occurred()) {
                /*
                 * java exceptions will have been transformed to python
                 * exceptions by this point
                 */
                (*env)->DeleteLocalRef(env, list);
                return NULL;
            }
            (*env)->CallBooleanMethod(env, list, arraylistAdd, value);
            if(process_java_exception(env)) {
                (*env)->DeleteLocalRef(env, list);
                return NULL;
            }
        }

        if(modifiable) {
            return list;
        } else {
            // make the tuple unmodifiable in Java
            jclass collections;
            jmethodID unmodifiableList;

            collections = (*env)->FindClass(env, "java/util/Collections");
            if(process_java_exception(env) || !collections) {
                return NULL;
            }

            unmodifiableList = (*env)->GetStaticMethodID(env,
                                                         collections,
                                                         "unmodifiableList",
                                                         "(Ljava/util/List;)Ljava/util/List;");
            if(process_java_exception(env) || !unmodifiableList) {
                return NULL;
            }
            list = (*env)->CallStaticObjectMethod(env,
                                                  collections,
                                                  unmodifiableList,
                                                  list);
            if(process_java_exception(env) || !list) {
                return NULL;
            }
            return list;
        }
    } // end of list and tuple conversion

    if(PyDict_Check(result)) {
        jclass clazz;
        jobject map, jkey, jvalue;
        Py_ssize_t size, pos;
        PyObject *key, *value;

        clazz = (*env)->FindClass(env, "java/util/HashMap");
        if(hashmapIConstructor == 0) {
            hashmapIConstructor = (*env)->GetMethodID(env,
                                                    clazz,
                                                    "<init>",
                                                    "(I)V");
        }
        if(hashmapPut == 0) {
            hashmapPut = (*env)->GetMethodID(env,
                                               clazz,
                                               "put",
                                               "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
        }

        if(process_java_exception(env) || !hashmapIConstructor || !hashmapPut) {
            return NULL;
        }

        size = PyDict_Size(result);
        map = (*env)->NewObject(env, clazz, hashmapIConstructor, (jint) size);
        if(process_java_exception(env) || !map) {
            return NULL;
        }

        pos = 0;
        while(PyDict_Next(result, &pos, &key, &value)) {
            jkey = pyembed_box_py(env, key);
            if(!jkey) {
                return NULL;
            }
            jvalue = pyembed_box_py(env, value);
            if(!jvalue) {
                return NULL;
            }

            (*env)->CallObjectMethod(env, map, hashmapPut, jkey, jvalue);
            if(process_java_exception(env)) {
                return NULL;
            }
        }

        return map;
    }

#if USE_NUMPY
    if(npy_array_check(result)) {
        return convert_pyndarray_jndarray(env, result);
    }
#endif

    // TODO find a better solution than this
    // convert everything else to string
    {
        jobject ret;
        char *tt;
        PyObject *t = PyObject_Str(result);
        tt = PyString_AsString(t);
        ret = (jobject) (*env)->NewStringUTF(env, (const char *) tt);
        Py_DECREF(t);

        return ret;
    }
}


jobject pyembed_getvalue_on(JNIEnv *env,
                            intptr_t _jepThread,
                            intptr_t _onModule,
                            char *str) {
    PyObject       *dict, *result, *onModule;
    jobject         ret = NULL;
    JepThread      *jepThread;

    result = 0;

    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return NULL;
    }
    
    if(str == NULL)
        return NULL;
    
    PyEval_AcquireThread(jepThread->tstate);
    
    if(process_py_exception(env, 1))
        goto EXIT;

    onModule = (PyObject *) _onModule;
    if(!PyModule_Check(onModule)) {
        THROW_JEP(env, "pyembed_getvalue_on: Invalid onModule.");
        goto EXIT;
    }
    
    dict = PyModule_GetDict(onModule);
    Py_INCREF(dict);
    
    result = PyRun_String(str, Py_eval_input, dict, dict);      /* new ref */
    
    process_py_exception(env, 1);
    Py_DECREF(dict);
    
    if(result == NULL)
        goto EXIT;              /* don't return, need to release GIL */
    if(result == Py_None)
        goto EXIT;
    
    // convert results to jobject
    ret = pyembed_box_py(env, result);
    
EXIT:
    PyEval_ReleaseThread(jepThread->tstate);

    Py_XDECREF(result);
    return ret;
}


jobject pyembed_getvalue(JNIEnv *env, intptr_t _jepThread, char *str) {
    PyObject       *result;
    jobject         ret = NULL;
    JepThread      *jepThread;

    result = NULL;
    
    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return NULL;
    }
    
    if(str == NULL)
        return NULL;
    
    PyEval_AcquireThread(jepThread->tstate);
    
    if(process_py_exception(env, 1))
        goto EXIT;
    
    result = PyRun_String(str,  /* new ref */
                          Py_eval_input,
                          jepThread->globals,
                          jepThread->globals);
    
    process_py_exception(env, 1);
    
    if(result == NULL || result == Py_None)
        goto EXIT;              /* don't return, need to release GIL */
    
    // convert results to jobject
    ret = pyembed_box_py(env, result);
    
EXIT:
    PyEval_ReleaseThread(jepThread->tstate);

    Py_XDECREF(result);
    return ret;
}



jobject pyembed_getvalue_array(JNIEnv *env, intptr_t _jepThread, char *str, int typeId) {
    PyObject       *result;
    jobject         ret = NULL;
    JepThread      *jepThread;

    result = NULL;
    
    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return NULL;
    }

    if(str == NULL)
        return NULL;
    
    PyEval_AcquireThread(jepThread->tstate);
    
    if(process_py_exception(env, 1))
        goto EXIT;
    
    result = PyRun_String(str,  /* new ref */
                          Py_eval_input,
                          jepThread->globals,
                          jepThread->globals);
    
    process_py_exception(env, 1);
    
    if(result == NULL || result == Py_None)
        goto EXIT;              /* don't return, need to release GIL */
    
#if PY_MAJOR_VERSION >= 3
    if(PyBytes_Check(result) == 0) {
        PyObject *temp = PyBytes_FromObject(result);
        if(process_py_exception(env, 1) || result == NULL) {
            goto EXIT;
        } else {
            Py_DECREF(result);
            result = temp;
        }
    }
#endif

    if(PyBytes_Check(result)) {
        void *s = (void*) PyBytes_AS_STRING(result);
        Py_ssize_t n = PyBytes_Size(result);

        switch (typeId) {
        case JFLOAT_ID:
            if(n % SIZEOF_FLOAT != 0) {
                THROW_JEP(env, "The Python string is the wrong length.\n");
                goto EXIT;
            }

            ret = (*env)->NewFloatArray(env, (jsize) n / SIZEOF_FLOAT);
            (*env)->SetFloatArrayRegion(env, ret, 0, (jsize) (n / SIZEOF_FLOAT), (jfloat *) s);
            break;

        case JBYTE_ID:
            ret = (*env)->NewByteArray(env, (jsize) n);
            (*env)->SetByteArrayRegion(env, ret, 0, (jsize) n, (jbyte *) s);
            break;

        default:
            THROW_JEP(env, "Internal error: array type not handled.");
            ret = NULL;
            goto EXIT;

        } // switch

    }
    else{
        THROW_JEP(env, "Value is not a string.");
        goto EXIT;
    }
    
    
EXIT:
    PyEval_ReleaseThread(jepThread->tstate);

    Py_XDECREF(result);
    return ret;
}






void pyembed_run(JNIEnv *env,
                 intptr_t _jepThread,
                 char *file) {
    JepThread     *jepThread;
    const char    *ext;
    
    jepThread = (JepThread *) _jepThread;
    if(!jepThread) {
        THROW_JEP(env, "Couldn't get thread objects.");
        return;
    }

    PyEval_AcquireThread(jepThread->tstate);
    
    if(file != NULL) {
        FILE *script = fopen(file, "r");
        if(!script) {
            THROW_JEP(env, "Couldn't open script file.");
            goto EXIT;
        }

        // check if it's a pyc/pyo file
        ext = file + strlen(file) - 4;
        if (maybe_pyc_file(script, file, ext, 0)) {
            /* Try to run a pyc file. First, re-open in binary */
            fclose(script);
            if((script = fopen(file, "rb")) == NULL) {
                THROW_JEP(env, "pyembed_run: Can't reopen .pyc file");
                goto EXIT;
            }

            /* Turn on optimization if a .pyo file is given */
            if(strcmp(ext, ".pyo") == 0)
                Py_OptimizeFlag = 2;
            else
                Py_OptimizeFlag = 0;

            pyembed_run_pyc(jepThread, script);
        }
        else {
            PyRun_File(script,
                       file,
                       Py_file_input,
                       jepThread->globals,
                       jepThread->globals);
        }

        // c programs inside some java environments may get buffered output
        fflush(stdout);
        fflush(stderr);
        
        fclose(script);
        process_py_exception(env, 1);
    }

EXIT:
    PyEval_ReleaseThread(jepThread->tstate);
}


// gratuitously copyied from pythonrun.c::run_pyc_file
static void pyembed_run_pyc(JepThread *jepThread,
                            FILE *fp) {
    PyObject *co;
    PyObject *v;
    long magic;

    long PyImport_GetMagicNumber(void);

    magic = PyMarshal_ReadLongFromFile(fp);
    if (magic != PyImport_GetMagicNumber()) {
        PyErr_SetString(PyExc_RuntimeError, "Bad magic number in .pyc file");
        return;
    }
    (void) PyMarshal_ReadLongFromFile(fp);
    v = (PyObject *) (intptr_t) PyMarshal_ReadLastObjectFromFile(fp);
    if (v == NULL || !PyCode_Check(v)) {
        Py_XDECREF(v);
        PyErr_SetString(PyExc_RuntimeError, "Bad code object in .pyc file");
        return;
    }
    co = v;
#if PY_MAJOR_VERSION >= 3
    v = PyEval_EvalCode(co, jepThread->globals, jepThread->globals);
#else
    v = PyEval_EvalCode((PyCodeObject *) co, jepThread->globals, jepThread->globals);
#endif
    Py_DECREF(co);
    Py_XDECREF(v);
}

/* Check whether a file maybe a pyc file: Look at the extension,
 the file type, and, if we may close it, at the first few bytes. */
// gratuitously copyied from pythonrun.c::run_pyc_file
static int maybe_pyc_file(FILE *fp, const char* filename, const char* ext,
        int closeit) {
    if (strcmp(ext, ".pyc") == 0 || strcmp(ext, ".pyo") == 0)
        return 1;

    /* Only look into the file if we are allowed to close it, since
     it then should also be seekable. */
    if (closeit) {
        /* Read only two bytes of the magic. If the file was opened in
         text mode, the bytes 3 and 4 of the magic (\r\n) might not
         be read as they are on disk. */
        unsigned int halfmagic = (unsigned int) PyImport_GetMagicNumber()
                & 0xFFFF;
        unsigned char buf[2];
        /* Mess:  In case of -x, the stream is NOT at its start now,
         and ungetc() was used to push back the first newline,
         which makes the current stream position formally undefined,
         and a x-platform nightmare.
         Unfortunately, we have no direct way to know whether -x
         was specified.  So we use a terrible hack:  if the current
         stream position is not 0, we assume -x was specified, and
         give up.  Bug 132850 on SourceForge spells out the
         hopelessness of trying anything else (fseek and ftell
         don't work predictably x-platform for text-mode files).
         */
        int ispyc = 0;
        if (ftell(fp) == 0) {
            if (fread(buf, 1, 2, fp) == 2
                    && (buf[1] << 8 | buf[0]) == halfmagic)
                ispyc = 1;
            rewind(fp);
        }

        return ispyc;
    }
    return 0;
}


// -------------------------------------------------- set() things

#define GET_COMMON                                                  \
    JepThread *jepThread;                                           \
                                                                    \
    jepThread = (JepThread *) _jepThread;                           \
    if(!jepThread) {                                                \
        THROW_JEP(env, "Couldn't get thread objects.");             \
        return;                                                     \
    }                                                               \
                                                                    \
    if(name == NULL) {                                              \
        THROW_JEP(env, "name is invalid.");                         \
        return;                                                     \
    }                                                               \
                                                                    \
    PyEval_AcquireThread(jepThread->tstate);                        \
                                                                    \
    pymodule = NULL;                                                \
    if(module != 0)                                                 \
        pymodule = (PyObject *) module;



void pyembed_setparameter_object(JNIEnv *env,
                                 intptr_t _jepThread,
                                 intptr_t module,
                                 const char *name,
                                 jobject value) {
    PyObject      *pyjob;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;
    
    if(value == NULL) {
        Py_INCREF(Py_None);
        pyjob = Py_None;
    }
    else
        pyjob = pyjobject_new(env, value);
    
    if(pyjob) {
        if(pymodule == NULL) {
            PyObject *key = PyString_FromString(name);
            PyDict_SetItem(jepThread->globals,
                           key,
                           pyjob); /* ownership */
            Py_DECREF(key);
            Py_DECREF(pyjob);
        }
        else {
            PyModule_AddObject(pymodule,
                               (char *) name,
                               pyjob); // steals reference
        }
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_array(JNIEnv *env,
                                intptr_t _jepThread,
                                intptr_t module,
                                const char *name,
                                jobjectArray obj) {
    PyObject      *pyjob;
    PyObject      *pymodule;

    // does common things
    GET_COMMON;

    if(obj == NULL) {
        Py_INCREF(Py_None);
        pyjob = Py_None;
    }
    else
        pyjob = pyjarray_new(env, obj);
    
    if(pyjob) {
        if(pymodule == NULL) {
            PyObject *key = PyString_FromString(name);
            PyDict_SetItem(jepThread->globals,
                           key,
                           pyjob); /* ownership */
            Py_DECREF(key);
            Py_DECREF(pyjob);
        }
        else {
            PyModule_AddObject(pymodule,
                               (char *) name,
                               pyjob); // steals reference
        }
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_class(JNIEnv *env,
                                intptr_t _jepThread,
                                intptr_t module,
                                const char *name,
                                jclass value) {
    PyObject      *pyjob;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;
    
    if(value == NULL) {
        Py_INCREF(Py_None);
        pyjob = Py_None;
    }
    else
        pyjob = pyjobject_new_class(env, value);
    
    if(pyjob) {
        if(pymodule == NULL) {
            PyObject *key = PyString_FromString(name);
            PyDict_SetItem(jepThread->globals,
                           key,
                           pyjob); /* ownership */
            Py_DECREF(key);
            Py_DECREF(pyjob);
        }
        else {
            PyModule_AddObject(pymodule,
                               (char *) name,
                               pyjob); // steals reference
        }
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_string(JNIEnv *env,
                                 intptr_t _jepThread,
                                 intptr_t module,
                                 const char *name,
                                 const char *value) {
    PyObject      *pyvalue;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;

    if(value == NULL) {
        Py_INCREF(Py_None);
        pyvalue = Py_None;
    }
    else
        pyvalue = PyString_FromString(value);

    if(pymodule == NULL) {
        PyObject *key = PyString_FromString(name);
        PyDict_SetItem(jepThread->globals,
                       key,
                       pyvalue); /* ownership */
        Py_DECREF(key);
        Py_DECREF(pyvalue);
    }
    else {
        PyModule_AddObject(pymodule,
                           (char *) name,
                           pyvalue); // steals reference
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_int(JNIEnv *env,
                              intptr_t _jepThread,
                              intptr_t module,
                              const char *name,
                              int value) {
    PyObject      *pyvalue;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;
    
    if((pyvalue = Py_BuildValue("i", value)) == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Out of memory.");
        return;
    }
    
    if(pymodule == NULL) {
        PyObject *key = PyString_FromString(name);
        PyDict_SetItem(jepThread->globals,
                       key,
                       pyvalue); /* ownership */
        Py_DECREF(key);
        Py_DECREF(pyvalue);
    }
    else {
        PyModule_AddObject(pymodule,
                           (char *) name,
                           pyvalue); // steals reference
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_long(JNIEnv *env,
                               intptr_t _jepThread,
                               intptr_t module,
                               const char *name,
                               jeplong value) {
    PyObject      *pyvalue;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;
    
    if((pyvalue = PyLong_FromLongLong(value)) == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Out of memory.");
        return;
    }
    
    if(pymodule == NULL) {
        PyObject *key = PyString_FromString(name);
        PyDict_SetItem(jepThread->globals,
                       key,
                       pyvalue); /* ownership */
        Py_DECREF(key);
        Py_DECREF(pyvalue);
    }
    else {
        PyModule_AddObject(pymodule,
                           (char *) name,
                           pyvalue); // steals reference
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_double(JNIEnv *env,
                                 intptr_t _jepThread,
                                 intptr_t module,
                                 const char *name,
                                 double value) {
    PyObject      *pyvalue;
    PyObject      *pymodule;
    
    // does common things
    GET_COMMON;
    
    if((pyvalue = PyFloat_FromDouble(value)) == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Out of memory.");
        return;
    }
    
    if(pymodule == NULL) {
        PyObject *key = PyString_FromString(name);
        PyDict_SetItem(jepThread->globals,
                       key,
                       pyvalue); /* ownership */
        Py_DECREF(key);
        Py_DECREF(pyvalue);
    }
    else {
        PyModule_AddObject(pymodule,
                           (char *) name,
                           pyvalue); // steals reference
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}


void pyembed_setparameter_float(JNIEnv *env,
                                intptr_t _jepThread,
                                intptr_t module,
                                const char *name,
                                float value) {
    PyObject      *pyvalue;
    PyObject      *pymodule;

    // does common things
    GET_COMMON;
    
    if((pyvalue = PyFloat_FromDouble((double) value)) == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Out of memory.");
        return;
    }
    
    if(pymodule == NULL) {
        PyObject *key = PyString_FromString(name);
        PyDict_SetItem(jepThread->globals,
                       key,
                       pyvalue); /* ownership */
        Py_DECREF(key);
        Py_DECREF(pyvalue);
    }
    else {
        PyModule_AddObject(pymodule,
                           (char *) name,
                           pyvalue); // steals reference
    }

    PyEval_ReleaseThread(jepThread->tstate);
    return;
}
