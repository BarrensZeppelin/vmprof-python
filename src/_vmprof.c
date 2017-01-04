/*[clinic input]
module _vmprof
[clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=b443489e38f2be7d]*/

#define _GNU_SOURCE 1

#include <Python.h>
#include <frameobject.h>
#include <signal.h>
// sadly that does not work for python < 3#include "clinic/_vmprof.c.h"

#include "_vmprof.h"

static volatile int is_enabled = 0;

#if defined(__unix__) || defined(__APPLE__)
#include "vmprof_main.h"
#include "trampoline.h"
#include "machine.h"
#else
#include "vmprof_main_win32.h"
#endif

static destructor Original_code_dealloc = 0;
static PyObject *(*Original_PyEval_EvalFrameEx)(PyFrameObject *f,
                                                int throwflag) = 0;

static int emit_code_object(PyCodeObject *co)
{
    char buf[MAX_FUNC_NAME + 1];
    char *co_name, *co_filename;
    int co_firstlineno;
    int sz;
#if PY_MAJOR_VERSION >= 3
    co_name = PyUnicode_AsUTF8(co->co_name);
    if (co_name == NULL)
        return -1;
    co_filename = PyUnicode_AsUTF8(co->co_filename);
    if (co_filename == NULL)
        return -1;
#else
    co_name = PyString_AS_STRING(co->co_name);
    co_filename = PyString_AS_STRING(co->co_filename);
#endif
    co_firstlineno = co->co_firstlineno;

    sz = snprintf(buf, MAX_FUNC_NAME / 2, "py:%s", co_name);
    if (sz < 0) sz = 0;
    if (sz > MAX_FUNC_NAME / 2) sz = MAX_FUNC_NAME / 2;
    snprintf(buf + sz, MAX_FUNC_NAME / 2, ":%d:%s", co_firstlineno,
             co_filename);
    return vmprof_register_virtual_function(buf, CODE_ADDR_TO_UID(co), 500000);
}

static int _look_for_code_object(PyObject *o, void *all_codes)
{
    if (PyCode_Check(o) && !PySet_Contains((PyObject *)all_codes, o)) {
        Py_ssize_t i;
        PyCodeObject *co = (PyCodeObject *)o;
        if (emit_code_object(co) < 0)
            return -1;
        if (PySet_Add((PyObject *)all_codes, o) < 0)
            return -1;

        /* as a special case, recursively look for and add code
           objects found in the co_consts.  The problem is that code
           objects are not created as GC-aware in CPython, so we need
           to hack like this to hope to find most of them. 
        */
        i = PyTuple_Size(co->co_consts);
        while (i > 0) {
            --i;
            if (_look_for_code_object(PyTuple_GET_ITEM(co->co_consts, i),
                                      all_codes) < 0)
                return -1;
        }
    }
    return 0;
}

static void emit_all_code_objects(void)
{
    PyObject *gc_module = NULL, *lst = NULL, *all_codes = NULL;
    Py_ssize_t i, size;

    gc_module = PyImport_ImportModuleNoBlock("gc");
    if (gc_module == NULL)
        goto error;

    lst = PyObject_CallMethod(gc_module, "get_objects", "");
    if (lst == NULL || !PyList_Check(lst))
        goto error;

    all_codes = PySet_New(NULL);
    if (all_codes == NULL)
        goto error;

    size = PyList_GET_SIZE(lst);
    for (i = 0; i < size; i++) {
        PyObject *o = PyList_GET_ITEM(lst, i);
        if (o->ob_type->tp_traverse &&
            o->ob_type->tp_traverse(o, _look_for_code_object, (void *)all_codes)
                < 0)
            goto error;
    }

 error:
    Py_XDECREF(all_codes);
    Py_XDECREF(lst);
    Py_XDECREF(gc_module);
}

static void cpyprof_code_dealloc(PyObject *co)
{
    if (is_enabled) {
        emit_code_object((PyCodeObject *)co);
        /* xxx error return values are ignored */
    }
    Original_code_dealloc(co);
}

static void init_cpyprof(void)
{
#if CPYTHON_HAS_FRAME_EVALUATION
    PyThreadState *tstate = PyThreadState_GET();
    tstate->interp->eval_frame = cpython_vmprof_PyEval_EvalFrameEx;
    vmp_native_enable(0);
    Original_PyEval_EvalFrameEx = cpython_vmprof_PyEval_EvalFrameEx;
#else
    if (vmp_patch_callee_trampoline("PyEval_EvalFrameEx") == 0) {
        Original_PyEval_EvalFrameEx = PyEval_EvalFrameEx;
    } else {
        printf("could not insert trampline\n");
        // TODO dump the first few bytes and tell them to create an issue!
    }
#endif
}


PyObject* cpython_vmprof_PyEval_EvalFrameEx(PyFrameObject *f, int throwflag)
{
    // move the frame to a caller saved register. e.g. rbx!
    // the previous method from did not really work for me
    // (it did not find the correct value in the stack slot)
    register PyFrameObject * rbx asm("rbx");
    asm volatile("mov %%rdi, %0\n\t"
                 "call %1\n\t"
#if CPYTHON_HAS_FRAME_EVALUATION
                : : "r" (rbx), "r" (_PyEval_EvalFrameDefault));
#else
                : : "r" (rbx), "r" (PyEval_EvalFrameEx));
#endif
}

static PyObject *enable_vmprof(PyObject* self, PyObject *args)
{
    int fd;
    int memory = 0;
    int lines = 0;
    int native = 0;
    double interval;
    char *p_error;


    if (!PyArg_ParseTuple(args, "id|iii", &fd, &interval, &memory, &lines, &native)) {
        return NULL;
    }
    assert(fd >= 0 && "file descripter provided to vmprof must not" \
                      " be less then zero.");

    if (is_enabled) {
        PyErr_SetString(PyExc_ValueError, "vmprof is already enabled");
        return NULL;
    }

    init_cpyprof();

    if (!Original_code_dealloc) {
        Original_code_dealloc = PyCode_Type.tp_dealloc;
        PyCode_Type.tp_dealloc = &cpyprof_code_dealloc;
    }

    p_error = vmprof_init(fd, interval, memory, lines, "cpython", native);
    if (p_error) {
        PyErr_SetString(PyExc_ValueError, p_error);
        return NULL;
    }

    if (vmprof_enable(memory) < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }

    is_enabled = 1;
    profile_lines = lines;

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
disable_vmprof(PyObject *module, PyObject *noarg)
{
    if (!is_enabled) {
        PyErr_SetString(PyExc_ValueError, "vmprof is not enabled");
        return NULL;
    }
    is_enabled = 0;
    vmprof_ignore_signals(1);
    emit_all_code_objects();

    if (vmprof_disable() < 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return NULL;
    }
    if (PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
write_all_code_objects(PyObject *module, PyObject *noargs)
{
    if (!is_enabled) {
        PyErr_SetString(PyExc_ValueError, "vmprof is not enabled");
        return NULL;
    }
    emit_all_code_objects();
    if (PyErr_Occurred())
        return NULL;
    Py_INCREF(Py_None);
    return Py_None;
}


static PyObject *
sample_stack_now(PyObject *module, PyObject *noargs)
{
    PyThreadState * tstate = NULL;
    PyObject * list;
    int i;
    int entry_count;
    list = PyList_New(0);
    if (list == NULL) {
        goto error;
    }

    tstate = PyGILState_GetThisThreadState();
    void ** m = (void**)malloc(SINGLE_BUF_SIZE);
    if (m == NULL) {
        PyErr_SetString(PyExc_MemoryError, "could not allocate buffer for stack trace");
        return NULL;
    }
    entry_count = get_stack_trace(tstate, m, MAX_STACK_DEPTH-1, 1);

    for (i = 0; i < entry_count; i++) {
        void * routine_ip = m[i];
        if (ROUTINE_IS_PYTHON(routine_ip)) {
            PyCodeObject * code = (PyCodeObject*)routine_ip;
            PyObject * name = code->co_name;
            PyList_Append(list, name);
        } else {
            // a native routine!
            char name[64];
            vmp_get_symbol_for_ip(routine_ip, name, 64);
            PyObject * str = PyStr_NEW(name);
            if (str == NULL) {
                goto error;
            }
            PyList_Append(list, str);
        }
    }

    free(m);

    Py_INCREF(list);
    return list;
error:
    Py_DECREF(list);
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
testing_enable(PyObject *module, PyObject * noargs)
{
    init_cpyprof();
    is_enabled = 1;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
testing_disable(PyObject *module, PyObject * noargs)
{
    Py_INCREF(Py_None);
    return Py_None;
}


static PyMethodDef VMProfMethods[] = {
    {"enable",  enable_vmprof, METH_VARARGS, "Enable profiling."},
    {"disable", disable_vmprof, METH_NOARGS, "Disable profiling."},
    {"write_all_code_objects", write_all_code_objects, METH_NOARGS,
     "Write eagerly all the IDs of code objects"},
    {"sample_stack_now", sample_stack_now, METH_NOARGS, "Sample the stack now"},
    {"test_enable", testing_enable, METH_NOARGS, "Enable vmprof for testing"},
    {"test_disable", testing_disable, METH_NOARGS, "Enable vmprof for testing"},
    {NULL, NULL, 0, NULL}        /* Sentinel */
};


#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef VmprofModule = {
    PyModuleDef_HEAD_INIT,
    "_vmprof",
    "",  // doc
    -1,  // size
    VMProfMethods
};

PyMODINIT_FUNC PyInit__vmprof(void)
{
    return PyModule_Create(&VmprofModule);
}
#else
PyMODINIT_FUNC init_vmprof(void)
{
    Py_InitModule("_vmprof", VMProfMethods);
}
#endif
